#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "import_resolver.h"
#include "codegen/binary_emitter.h"
#include "compiler/compiler_context.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "string_intern.h"
#include "common.h"
#include "runtime/owned.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_absolute_path(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }
#ifdef _WIN32
  if (isalpha((unsigned char)path[0]) && path[1] == ':' &&
      (path[2] == '\\' || path[2] == '/')) {
    return 1;
  }
  if (path[0] == '\\' || path[0] == '/') {
    return 1;
  }
  return 0;
#else
  return path[0] == '/';
#endif
}

static char *canonicalize_path(const char *path) {
  if (!path) {
    return NULL;
  }
  char *full = mettle_realpath(path, NULL);
  if (full) {
    return full;
  }
  return strdup(path);
}

static int path_has_extension(const char *path) {
  if (!path) {
    return 0;
  }

  const char *last_slash = strrchr(path, '/');
  const char *last_backslash = strrchr(path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;
  const char *last_dot = strrchr(path, '.');

  return last_dot && (!last_sep || last_dot > last_sep);
}

static char *directory_from_file_path(const char *file_path) {
  if (!file_path) {
    return strdup(".");
  }

  const char *last_slash = strrchr(file_path, '/');
  const char *last_backslash = strrchr(file_path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;

  if (!last_sep) {
    return strdup(".");
  }

  size_t len = (size_t)(last_sep - file_path + 1);
  char *dir = malloc(len + 1);
  if (!dir) {
    return NULL;
  }

  memcpy(dir, file_path, len);
  dir[len] = '\0';
  return dir;
}

static char *join_paths(const char *left, const char *right) {
  if (!right) {
    return NULL;
  }
  if (!left || left[0] == '\0') {
    return strdup(right);
  }

  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  int has_sep = (left_len > 0 &&
                 (left[left_len - 1] == '/' || left[left_len - 1] == '\\'));

  char *joined = malloc(left_len + right_len + (has_sep ? 1 : 2));
  if (!joined) {
    return NULL;
  }

  memcpy(joined, left, left_len);
  if (!has_sep) {
#ifdef _WIN32
    joined[left_len++] = '\\';
#else
    joined[left_len++] = '/';
#endif
  }
  memcpy(joined + left_len, right, right_len);
  joined[left_len + right_len] = '\0';

  return joined;
}

static int file_exists_readable(const char *path) {
  if (!path) {
    return 0;
  }

  FILE *file = fopen(path, "r");
  if (!file) {
    return 0;
  }

  fclose(file);
  return 1;
}

static char *resolve_candidate_path(const char *candidate_base) {
  if (!candidate_base || candidate_base[0] == '\0') {
    return NULL;
  }

  if (file_exists_readable(candidate_base)) {
    return canonicalize_path(candidate_base);
  }

  if (!path_has_extension(candidate_base)) {
    size_t len = strlen(candidate_base);
    char *with_ext = malloc(len + 8);
    if (!with_ext) {
      return NULL;
    }
    memcpy(with_ext, candidate_base, len);
    memcpy(with_ext + len, ".mettle", 8);

    if (file_exists_readable(with_ext)) {
      char *resolved = canonicalize_path(with_ext);
      free(with_ext);
      return resolved;
    }

    free(with_ext);
  }

  return NULL;
}

static int import_uses_std_namespace(const char *import_path) {
  if (!import_path) {
    return 0;
  }
  return strncmp(import_path, "std/", 4) == 0 ||
         strncmp(import_path, "std\\", 4) == 0;
}

static void trim_trailing_separators(char *path) {
  size_t len = 0;
  if (!path) {
    return;
  }

  len = strlen(path);
  while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
#ifdef _WIN32
    if (len == 3 && isalpha((unsigned char)path[0]) && path[1] == ':') {
      break;
    }
#endif
    if (len == 1) {
      break;
    }
    path[--len] = '\0';
  }
}

static int path_remove_last_component(char *path) {
  char *last_sep = NULL;

  if (!path || path[0] == '\0') {
    return 0;
  }

  trim_trailing_separators(path);
  last_sep = strrchr(path, '/');
  if (!last_sep || strrchr(path, '\\') > last_sep) {
    last_sep = strrchr(path, '\\');
  }
  if (!last_sep) {
    return 0;
  }

#ifdef _WIN32
  if (last_sep == path + 2 && isalpha((unsigned char)path[0]) &&
      path[1] == ':') {
    return 0;
  }
#endif
  if (last_sep == path) {
    return 0;
  }

  *last_sep = '\0';
  return 1;
}

static char *trim_whitespace_in_place(char *text) {
  char *end = NULL;
  if (!text) {
    return NULL;
  }
  while (*text && isspace((unsigned char)*text)) {
    text++;
  }
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    *--end = '\0';
  }
  return text;
}

/* mettle.deps ancestor-walk memoization.
 *
 * resolve_dependency_import() walks from the importer's directory up to the
 * filesystem root, fopen-probing for a "mettle.deps" file at every level. That
 * walk's result (the ordered list of existing mettle.deps files, each paired
 * with the directory it sits in) depends ONLY on the start directory, not on
 * the import string. In a real project every module lives in a handful of
 * directories, so the same multi-syscall walk is repeated for nearly every
 * import edge. Memoize it per canonical start directory. Most projects have
 * no mettle.deps at all, in which case this collapses ~all walks to one. */
typedef struct {
  char *deps_path; /* absolute path to an existing mettle.deps file */
  char *deps_dir;  /* directory containing it (used as join base) */
} DepsFileLocation;

typedef struct {
  char *start_dir; /* canonical directory the walk started from */
  DepsFileLocation *locations;
  size_t location_count;
} DepsWalkCacheEntry;

static DepsWalkCacheEntry *g_deps_walk_cache = NULL;
static size_t g_deps_walk_cache_count = 0;
static size_t g_deps_walk_cache_capacity = 0;

static void deps_walk_cache_reset(void) {
  for (size_t i = 0; i < g_deps_walk_cache_count; i++) {
    free(g_deps_walk_cache[i].start_dir);
    for (size_t j = 0; j < g_deps_walk_cache[i].location_count; j++) {
      free(g_deps_walk_cache[i].locations[j].deps_path);
      free(g_deps_walk_cache[i].locations[j].deps_dir);
    }
    free(g_deps_walk_cache[i].locations);
  }
  free(g_deps_walk_cache);
  g_deps_walk_cache = NULL;
  g_deps_walk_cache_count = 0;
  g_deps_walk_cache_capacity = 0;
}

/* Returns the cached list of existing mettle.deps locations for `start_dir`,
 * performing (and caching) the filesystem walk on first use. The returned
 * array is owned by the cache; the caller must not free it. *out_count is
 * set to the number of locations (may be 0). Returns NULL only on hard
 * allocation failure (caller falls back to NULL = "no dependency import"). */
static const DepsFileLocation *
deps_walk_lookup(const char *start_dir, size_t *out_count) {
  *out_count = 0;
  if (!start_dir || start_dir[0] == '\0') {
    return NULL;
  }

  for (size_t i = 0; i < g_deps_walk_cache_count; i++) {
    if (strcmp(g_deps_walk_cache[i].start_dir, start_dir) == 0) {
      *out_count = g_deps_walk_cache[i].location_count;
      return g_deps_walk_cache[i].locations;
    }
  }

  /* Perform the walk once. */
  DepsFileLocation *locations = NULL;
  size_t location_count = 0;
  size_t location_capacity = 0;

  char *walk_dir = strdup(start_dir);
  while (walk_dir && walk_dir[0] != '\0') {
    char *deps_path = join_paths(walk_dir, "mettle.deps");
    if (deps_path && file_exists_readable(deps_path)) {
      if (location_count >= location_capacity) {
        size_t nc = location_capacity == 0 ? 4 : location_capacity * 2;
        DepsFileLocation *grown =
            realloc(locations, nc * sizeof(DepsFileLocation));
        if (grown) {
          locations = grown;
          location_capacity = nc;
        }
      }
      if (location_count < location_capacity) {
        locations[location_count].deps_path = deps_path;
        locations[location_count].deps_dir = strdup(walk_dir);
        location_count++;
        deps_path = NULL; /* ownership moved into the cache entry */
      }
    }
    free(deps_path);
    if (!path_remove_last_component(walk_dir)) {
      break;
    }
  }
  free(walk_dir);

  if (g_deps_walk_cache_count >= g_deps_walk_cache_capacity) {
    size_t nc =
        g_deps_walk_cache_capacity == 0 ? 16 : g_deps_walk_cache_capacity * 2;
    DepsWalkCacheEntry *grown =
        realloc(g_deps_walk_cache, nc * sizeof(DepsWalkCacheEntry));
    if (grown) {
      g_deps_walk_cache = grown;
      g_deps_walk_cache_capacity = nc;
    }
  }
  if (g_deps_walk_cache_count < g_deps_walk_cache_capacity) {
    g_deps_walk_cache[g_deps_walk_cache_count].start_dir = strdup(start_dir);
    g_deps_walk_cache[g_deps_walk_cache_count].locations = locations;
    g_deps_walk_cache[g_deps_walk_cache_count].location_count = location_count;
    g_deps_walk_cache_count++;
    *out_count = location_count;
    return g_deps_walk_cache[g_deps_walk_cache_count - 1].locations;
  }

  /* Cache full and realloc failed: return a transient result. Leak-free path
   * would require ownership juggling; this branch only triggers under OOM. */
  for (size_t j = 0; j < location_count; j++) {
    free(locations[j].deps_path);
    free(locations[j].deps_dir);
  }
  free(locations);
  return NULL;
}

static char *resolve_dependency_import(const char *current_file_path,
                                      const char *import_path) {
  char *search_dir = NULL;
  char *result = NULL;
  size_t package_name_len = 0;
  const char *package_rest = NULL;

  if (!current_file_path || !import_path || import_path[0] == '\0') {
    return NULL;
  }

  package_rest = strpbrk(import_path, "/\\");
  package_name_len =
      package_rest ? (size_t)(package_rest - import_path) : strlen(import_path);
  if (package_name_len == 0) {
    return NULL;
  }
  if (package_rest) {
    package_rest++;
  }

  search_dir = canonicalize_path(current_file_path);
  if (!search_dir) {
    search_dir = directory_from_file_path(current_file_path);
  } else {
    if (!path_remove_last_component(search_dir)) {
      free(search_dir);
      search_dir = directory_from_file_path(current_file_path);
    }
  }
  if (!search_dir) {
    return NULL;
  }

  /* Resolve the mettle.deps locations once per start directory (memoized). */
  size_t deps_loc_count = 0;
  const DepsFileLocation *deps_locs =
      deps_walk_lookup(search_dir, &deps_loc_count);

  for (size_t li = 0; li < deps_loc_count; li++) {
    const char *deps_path = deps_locs[li].deps_path;
    const char *deps_dir = deps_locs[li].deps_dir;
    {
      FILE *deps_file = fopen(deps_path, "r");
      if (deps_file) {
        char line[2048];
        while (fgets(line, sizeof(line), deps_file)) {
          char *separator = NULL;
          char *name = NULL;
          char *value = NULL;
          char *dep_root = NULL;
          char *candidate_base = NULL;

          name = trim_whitespace_in_place(line);
          if (!name[0] || name[0] == '#') {
            continue;
          }

          separator = strchr(name, '=');
          if (!separator) {
            continue;
          }

          *separator = '\0';
          value = trim_whitespace_in_place(separator + 1);
          name = trim_whitespace_in_place(name);
          if (!name[0] || !value[0]) {
            continue;
          }

          if (strlen(name) != package_name_len ||
              strncmp(name, import_path, package_name_len) != 0) {
            continue;
          }

          dep_root = is_absolute_path(value) ? strdup(value)
                                             : join_paths(deps_dir, value);
          if (!dep_root) {
            continue;
          }

          if (package_rest && package_rest[0] != '\0') {
            candidate_base = join_paths(dep_root, package_rest);
            free(dep_root);
          } else {
            candidate_base = dep_root;
          }

          result = resolve_candidate_path(candidate_base);
          free(candidate_base);
          if (result) {
            fclose(deps_file);
            free(search_dir);
            return result;
          }
        }
        fclose(deps_file);
      }
    }
  }
  free(search_dir);
  return NULL;
}

typedef struct {
  // Fully-resolved module paths that have completed import resolution.
  char **resolved_files;
  size_t resolved_count;
  size_t resolved_capacity;
  // Fully-resolved module paths currently being traversed (recursion stack).
  // Used to detect true circular imports.
  char **active_files;
  size_t active_count;
  size_t active_capacity;
  // Import chain for error reporting (stack of file paths)
  char **import_chain;
  size_t chain_depth;
  size_t chain_capacity;
  ErrorReporter *reporter;
  const ImportResolverOptions *options;
} ImportContext;

typedef struct {
  char *old_name;
  char *new_name;
} NameRewrite;

typedef struct {
  char *alias;
  char **members;
  size_t member_count;
  size_t member_capacity;
} NamespaceBinding;

typedef struct RewriteScope {
  char **names;
  size_t count;
  size_t capacity;
  struct RewriteScope *parent;
} RewriteScope;

static const char *get_declaration_name(ASTNode *decl);
static int is_declaration_exported(ASTNode *decl);

static int replace_interned_string(char **slot, const char *value) {
  char *replacement = NULL;

  if (!slot) {
    return 0;
  }

  if (value) {
    replacement = (char *)string_intern(value);
    if (!replacement) {
      return 0;
    }
  }

  mettle_free_string(*slot);
  *slot = replacement;
  return 1;
}

static int append_string(char ***items, size_t *count, size_t *capacity,
                         const char *value) {
  if (!items || !count || !capacity || !value) {
    return 0;
  }

  if (*count >= *capacity) {
    size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);
    char **grown = realloc(*items, new_capacity * sizeof(char *));
    if (!grown) {
      return 0;
    }
    *items = grown;
    *capacity = new_capacity;
  }

  (*items)[*count] = strdup(value);
  if (!(*items)[*count]) {
    return 0;
  }

  (*count)++;
  return 1;
}

static char *duplicate_string_slice(const char *value, size_t length) {
  char *copy = NULL;

  if (!value) {
    return NULL;
  }

  copy = malloc(length + 1);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, value, length);
  copy[length] = '\0';
  return copy;
}

static void free_string_array(char **items, size_t count) {
  mettle_free_string_array(items, count);
}

static int clone_string_array(char **items, size_t count, char ***out_items,
                              size_t *out_count, size_t *out_capacity) {
  *out_items = NULL;
  *out_count = 0;
  *out_capacity = 0;

  for (size_t i = 0; i < count; i++) {
    if (!append_string(out_items, out_count, out_capacity, items[i])) {
      free_string_array(*out_items, *out_count);
      *out_items = NULL;
      *out_count = 0;
      *out_capacity = 0;
      return 0;
    }
  }

  return 1;
}

static int import_context_init_child(ImportContext *child,
                                     const ImportContext *parent) {
  if (!child || !parent) {
    return 0;
  }

  memset(child, 0, sizeof(*child));
  child->reporter = parent->reporter;
  child->options = parent->options;

  if (!clone_string_array(parent->active_files, parent->active_count,
                          &child->active_files, &child->active_count,
                          &child->active_capacity)) {
    return 0;
  }

  if (!clone_string_array(parent->import_chain, parent->chain_depth,
                          &child->import_chain, &child->chain_depth,
                          &child->chain_capacity)) {
    free_string_array(child->active_files, child->active_count);
    memset(child, 0, sizeof(*child));
    child->reporter = parent->reporter;
    child->options = parent->options;
    return 0;
  }

  return 1;
}

static void import_context_destroy(ImportContext *ctx) {
  if (!ctx) {
    return;
  }

  free_string_array(ctx->resolved_files, ctx->resolved_count);
  free_string_array(ctx->active_files, ctx->active_count);
  free_string_array(ctx->import_chain, ctx->chain_depth);
  memset(ctx, 0, sizeof(*ctx));
}

static const char *find_name_rewrite(const NameRewrite *rewrites,
                                     size_t rewrite_count,
                                     const char *name) {
  if (!rewrites || !name) {
    return NULL;
  }

  for (size_t i = 0; i < rewrite_count; i++) {
    if (strcmp(rewrites[i].old_name, name) == 0) {
      return rewrites[i].new_name;
    }
  }

  return NULL;
}

static const char *find_name_rewrite_slice(const NameRewrite *rewrites,
                                           size_t rewrite_count,
                                           const char *name,
                                           size_t name_len) {
  if (!rewrites || !name) {
    return NULL;
  }

  for (size_t i = 0; i < rewrite_count; i++) {
    if (strlen(rewrites[i].old_name) == name_len &&
        strncmp(rewrites[i].old_name, name, name_len) == 0) {
      return rewrites[i].new_name;
    }
  }

  return NULL;
}

static int add_name_rewrite(NameRewrite **rewrites, size_t *rewrite_count,
                            size_t *rewrite_capacity, const char *old_name,
                            const char *new_name) {
  if (!rewrites || !rewrite_count || !rewrite_capacity || !old_name ||
      !new_name) {
    return 0;
  }

  for (size_t i = 0; i < *rewrite_count; i++) {
    if (strcmp((*rewrites)[i].old_name, old_name) == 0) {
      return 1;
    }
  }

  if (*rewrite_count >= *rewrite_capacity) {
    size_t new_capacity = *rewrite_capacity == 0 ? 8 : (*rewrite_capacity * 2);
    NameRewrite *grown = realloc(*rewrites, new_capacity * sizeof(NameRewrite));
    if (!grown) {
      return 0;
    }
    *rewrites = grown;
    *rewrite_capacity = new_capacity;
  }

  (*rewrites)[*rewrite_count].old_name = strdup(old_name);
  if (!(*rewrites)[*rewrite_count].old_name) {
    return 0;
  }

  (*rewrites)[*rewrite_count].new_name = strdup(new_name);
  if (!(*rewrites)[*rewrite_count].new_name) {
    free((*rewrites)[*rewrite_count].old_name);
    (*rewrites)[*rewrite_count].old_name = NULL;
    return 0;
  }

  (*rewrite_count)++;
  return 1;
}

static void free_name_rewrites(NameRewrite *rewrites, size_t rewrite_count) {
  if (!rewrites) {
    return;
  }
  for (size_t i = 0; i < rewrite_count; i++) {
    free(rewrites[i].old_name);
    free(rewrites[i].new_name);
  }
  free(rewrites);
}

static NamespaceBinding *find_namespace_binding(NamespaceBinding *bindings,
                                                size_t binding_count,
                                                const char *alias) {
  if (!bindings || !alias) {
    return NULL;
  }

  for (size_t i = 0; i < binding_count; i++) {
    if (strcmp(bindings[i].alias, alias) == 0) {
      return &bindings[i];
    }
  }

  return NULL;
}

static const NamespaceBinding *
find_namespace_binding_const(const NamespaceBinding *bindings,
                             size_t binding_count, const char *alias) {
  return find_namespace_binding((NamespaceBinding *)bindings, binding_count,
                                alias);
}

static int has_namespace_binding(const NamespaceBinding *bindings,
                                 size_t binding_count, const char *alias) {
  return find_namespace_binding_const(bindings, binding_count, alias) != NULL;
}

static int add_namespace_binding(NamespaceBinding **bindings,
                                 size_t *binding_count,
                                 size_t *binding_capacity,
                                 const char *alias) {
  if (!bindings || !binding_count || !binding_capacity || !alias) {
    return 0;
  }

  if (has_namespace_binding(*bindings, *binding_count, alias)) {
    return 1;
  }

  if (*binding_count >= *binding_capacity) {
    size_t new_capacity = *binding_capacity == 0 ? 4 : (*binding_capacity * 2);
    NamespaceBinding *grown =
        realloc(*bindings, new_capacity * sizeof(NamespaceBinding));
    if (!grown) {
      return 0;
    }
    *bindings = grown;
    *binding_capacity = new_capacity;
  }

  (*bindings)[*binding_count].alias = strdup(alias);
  if (!(*bindings)[*binding_count].alias) {
    return 0;
  }
  (*bindings)[*binding_count].members = NULL;
  (*bindings)[*binding_count].member_count = 0;
  (*bindings)[*binding_count].member_capacity = 0;

  (*binding_count)++;
  return 1;
}

static int namespace_binding_allows_member(const NamespaceBinding *bindings,
                                           size_t binding_count,
                                           const char *alias,
                                           const char *member) {
  const NamespaceBinding *binding =
      find_namespace_binding_const(bindings, binding_count, alias);
  if (!binding || !member) {
    return 0;
  }

  for (size_t i = 0; i < binding->member_count; i++) {
    if (strcmp(binding->members[i], member) == 0) {
      return 1;
    }
  }

  return 0;
}

static int add_namespace_binding_member(NamespaceBinding **bindings,
                                        size_t *binding_count,
                                        size_t *binding_capacity,
                                        const char *alias,
                                        const char *member) {
  if (!bindings || !binding_count || !binding_capacity || !alias || !member) {
    return 0;
  }

  if (!add_namespace_binding(bindings, binding_count, binding_capacity,
                             alias)) {
    return 0;
  }

  NamespaceBinding *binding =
      find_namespace_binding(*bindings, *binding_count, alias);
  if (!binding) {
    return 0;
  }

  for (size_t i = 0; i < binding->member_count; i++) {
    if (strcmp(binding->members[i], member) == 0) {
      return 1;
    }
  }

  if (binding->member_count >= binding->member_capacity) {
    size_t new_capacity =
        binding->member_capacity == 0 ? 8 : binding->member_capacity * 2;
    char **grown =
        realloc(binding->members, new_capacity * sizeof(char *));
    if (!grown) {
      return 0;
    }
    binding->members = grown;
    binding->member_capacity = new_capacity;
  }

  binding->members[binding->member_count] = strdup(member);
  if (!binding->members[binding->member_count]) {
    return 0;
  }
  binding->member_count++;
  return 1;
}

static void free_namespace_bindings(NamespaceBinding *bindings,
                                    size_t binding_count) {
  if (!bindings) {
    return;
  }
  for (size_t i = 0; i < binding_count; i++) {
    free(bindings[i].alias);
    free_string_array(bindings[i].members, bindings[i].member_count);
  }
  free(bindings);
}

static int scope_contains_current(const RewriteScope *scope, const char *name) {
  if (!scope || !name) {
    return 0;
  }

  for (size_t i = 0; i < scope->count; i++) {
    if (strcmp(scope->names[i], name) == 0) {
      return 1;
    }
  }

  return 0;
}

static int scope_contains(const RewriteScope *scope, const char *name) {
  const RewriteScope *current = scope;
  while (current) {
    if (scope_contains_current(current, name)) {
      return 1;
    }
    current = current->parent;
  }
  return 0;
}

static int scope_add_name(RewriteScope *scope, const char *name) {
  if (!scope || !name) {
    return 1;
  }

  if (scope_contains_current(scope, name)) {
    return 1;
  }

  return append_string(&scope->names, &scope->count, &scope->capacity, name);
}

static void scope_cleanup(RewriteScope *scope) {
  if (!scope) {
    return;
  }
  free_string_array(scope->names, scope->count);
  scope->names = NULL;
  scope->count = 0;
  scope->capacity = 0;
  scope->parent = NULL;
}

static int is_identifier_start_char(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

static int is_identifier_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

static int append_text_fragment(char **buffer, size_t *length, size_t *capacity,
                                const char *text, size_t text_len) {
  if (!buffer || !length || !capacity || (!text && text_len > 0)) {
    return 0;
  }

  if (*length + text_len + 1 > *capacity) {
    size_t new_capacity = *capacity == 0 ? 64 : *capacity;
    while (*length + text_len + 1 > new_capacity) {
      new_capacity *= 2;
    }
    char *grown = realloc(*buffer, new_capacity);
    if (!grown) {
      return 0;
    }
    *buffer = grown;
    *capacity = new_capacity;
  }

  if (text_len > 0) {
    memcpy(*buffer + *length, text, text_len);
    *length += text_len;
  }
  (*buffer)[*length] = '\0';
  return 1;
}

static char *build_qualified_name(const char *alias, const char *name) {
  size_t alias_len = 0;
  size_t name_len = 0;
  char *qualified = NULL;

  if (!alias || !name) {
    return NULL;
  }

  alias_len = strlen(alias);
  name_len = strlen(name);
  qualified = malloc(alias_len + name_len + 3);
  if (!qualified) {
    return NULL;
  }

  snprintf(qualified, alias_len + name_len + 3, "%s__%s", alias, name);
  return qualified;
}

static int rename_string_if_needed(char **slot, const NameRewrite *rewrites,
                                   size_t rewrite_count) {
  const char *replacement = NULL;

  if (!slot || !*slot) {
    return 1;
  }

  replacement = find_name_rewrite(rewrites, rewrite_count, *slot);
  if (!replacement) {
    return 1;
  }

  return replace_interned_string(slot, replacement);
}

static char *rewrite_type_string(const char *type_name,
                                 const NameRewrite *rewrites,
                                 size_t rewrite_count,
                                 const NamespaceBinding *bindings,
                                 size_t binding_count) {
  char *rewritten = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t i = 0;

  if (!type_name) {
    return NULL;
  }

  while (type_name[i] != '\0') {
    if (is_identifier_start_char(type_name[i])) {
      size_t ident_start = i;
      size_t ident_len = 1;
      const char *replacement = NULL;

      while (is_identifier_char(type_name[i + ident_len])) {
        ident_len++;
      }

      if (type_name[ident_start + ident_len] == '.' &&
          is_identifier_start_char(type_name[ident_start + ident_len + 1])) {
        size_t member_start = ident_start + ident_len + 1;
        size_t member_len = 1;
        char *alias =
            duplicate_string_slice(type_name + ident_start, ident_len);
        char *member = NULL;
        char *qualified = NULL;

        if (!alias) {
          free(rewritten);
          return NULL;
        }

        while (is_identifier_char(type_name[member_start + member_len])) {
          member_len++;
        }

        member = duplicate_string_slice(type_name + member_start, member_len);
        if (!member) {
          free(alias);
          free(rewritten);
          return NULL;
        }

        if (namespace_binding_allows_member(bindings, binding_count, alias,
                                            member)) {
          qualified = build_qualified_name(alias, member);
          if (!qualified ||
              !append_text_fragment(&rewritten, &length, &capacity, qualified,
                                    strlen(qualified))) {
            free(alias);
            free(member);
            free(qualified);
            free(rewritten);
            return NULL;
          }
          free(alias);
          free(member);
          free(qualified);
          i = member_start + member_len;
          continue;
        }

        free(alias);
        free(member);
      }

      replacement = find_name_rewrite_slice(rewrites, rewrite_count,
                                            type_name + ident_start, ident_len);
      if (replacement) {
        if (!append_text_fragment(&rewritten, &length, &capacity, replacement,
                                  strlen(replacement))) {
          free(rewritten);
          return NULL;
        }
      } else if (!append_text_fragment(&rewritten, &length, &capacity,
                                       type_name + ident_start, ident_len)) {
        free(rewritten);
        return NULL;
      }

      i = ident_start + ident_len;
      continue;
    }

    if (!append_text_fragment(&rewritten, &length, &capacity, &type_name[i],
                              1)) {
      free(rewritten);
      return NULL;
    }
    i++;
  }

  if (!rewritten) {
    rewritten = strdup(type_name);
  }

  return rewritten;
}

/* Inline assembly is a text blob to every other pass, but the names inside it
 * are real references: `call helper` reaches a Mettle function and `{flag}`
 * binds a global. An import renames a module's private declarations, so those
 * names have to travel with them. Identifiers inside quoted runs are data
 * (`db "text"`) and are left alone, and a name the enclosing scope declares is
 * a local, which no import renames. */
static char *rewrite_assembly_text(const char *assembly,
                                   const NameRewrite *rewrites,
                                   size_t rewrite_count,
                                   const RewriteScope *scope) {
  char *rewritten = NULL;
  size_t length = 0;
  size_t capacity = 0;
  size_t i = 0;

  if (!assembly) {
    return NULL;
  }

  while (assembly[i] != '\0') {
    if (assembly[i] == '"' || assembly[i] == '\'') {
      char quote = assembly[i];
      size_t start = i++;
      while (assembly[i] != '\0' && assembly[i] != quote) {
        if (assembly[i] == '\\' && assembly[i + 1] != '\0') {
          i++;
        }
        i++;
      }
      if (assembly[i] != '\0') {
        i++;
      }
      if (!append_text_fragment(&rewritten, &length, &capacity,
                                assembly + start, i - start)) {
        free(rewritten);
        return NULL;
      }
      continue;
    }

    if (is_identifier_start_char(assembly[i])) {
      size_t start = i;
      size_t ident_len = 1;
      const char *replacement = NULL;
      char *token = NULL;

      while (is_identifier_char(assembly[start + ident_len])) {
        ident_len++;
      }

      token = duplicate_string_slice(assembly + start, ident_len);
      if (!token) {
        free(rewritten);
        return NULL;
      }
      if (!scope_contains(scope, token)) {
        replacement = find_name_rewrite_slice(rewrites, rewrite_count,
                                              assembly + start, ident_len);
      }
      free(token);

      if (!append_text_fragment(&rewritten, &length, &capacity,
                                replacement ? replacement : assembly + start,
                                replacement ? strlen(replacement)
                                            : ident_len)) {
        free(rewritten);
        return NULL;
      }

      i = start + ident_len;
      continue;
    }

    if (!append_text_fragment(&rewritten, &length, &capacity, &assembly[i],
                              1)) {
      free(rewritten);
      return NULL;
    }
    i++;
  }

  if (!rewritten) {
    rewritten = strdup(assembly);
  }

  return rewritten;
}

static int rewrite_type_string_in_place(char **slot, const NameRewrite *rewrites,
                                        size_t rewrite_count,
                                        const NamespaceBinding *bindings,
                                        size_t binding_count) {
  char *rewritten = NULL;

  if (!slot || !*slot) {
    return 1;
  }

  rewritten = rewrite_type_string(*slot, rewrites, rewrite_count, bindings,
                                  binding_count);
  if (!rewritten) {
    return 0;
  }

  if (!replace_interned_string(slot, rewritten)) {
    free(rewritten);
    return 0;
  }

  free(rewritten);
  return 1;
}

static int rewrite_node_names(ASTNode *node, const NameRewrite *rewrites,
                              size_t rewrite_count,
                              const NamespaceBinding *bindings,
                              size_t binding_count, RewriteScope *scope,
                              int program_creates_scope);

static int rebuild_call_children(ASTNode *node) {
  CallExpression *call = NULL;

  if (!node || node->type != AST_FUNCTION_CALL || !node->data) {
    return 0;
  }

  call = (CallExpression *)node->data;
  free(node->children);
  node->children = NULL;
  node->child_count = 0;

  for (size_t i = 0; i < call->argument_count; i++) {
    ast_add_child(node, call->arguments[i]);
  }
  if (call->object) {
    ast_add_child(node, call->object);
  }

  return 1;
}

static int rewrite_program_names(ASTNode *program, const NameRewrite *rewrites,
                                 size_t rewrite_count,
                                 const NamespaceBinding *bindings,
                                 size_t binding_count, RewriteScope *scope,
                                 int create_scope) {
  Program *prog = NULL;
  RewriteScope block_scope;
  RewriteScope *active_scope = scope;

  if (!program || program->type != AST_PROGRAM) {
    return 1;
  }

  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  if (create_scope) {
    memset(&block_scope, 0, sizeof(block_scope));
    block_scope.parent = scope;
    active_scope = &block_scope;
  }

  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (!rewrite_node_names(prog->declarations[i], rewrites, rewrite_count,
                            bindings, binding_count, active_scope, 1)) {
      if (create_scope) {
        scope_cleanup(&block_scope);
      }
      return 0;
    }
  }

  if (create_scope) {
    scope_cleanup(&block_scope);
  }

  return 1;
}

static int rewrite_declaration_names(ASTNode *node,
                                     const NameRewrite *rewrites,
                                     size_t rewrite_count,
                                     const NamespaceBinding *bindings,
                                     size_t binding_count,
                                     RewriteScope *scope,
                                     int program_creates_scope);
static int rewrite_function_declaration_names(ASTNode *node,
                                              const NameRewrite *rewrites,
                                              size_t rewrite_count,
                                              const NamespaceBinding *bindings,
                                              size_t binding_count,
                                              RewriteScope *scope,
                                              int program_creates_scope);
static void collect_declaration_dependency_names(ASTNode *node, char ***names,
                                                 size_t *count,
                                                 size_t *capacity);

static int rewrite_node_names(ASTNode *node, const NameRewrite *rewrites,
                              size_t rewrite_count,
                              const NamespaceBinding *bindings,
                              size_t binding_count, RewriteScope *scope,
                              int program_creates_scope) {
  if (!node) {
    return 1;
  }

  switch (node->type) {
  case AST_PROGRAM:
    return rewrite_program_names(node, rewrites, rewrite_count, bindings,
                                 binding_count, scope, program_creates_scope);

  case AST_VAR_DECLARATION: {
    VarDeclaration *var_decl = (VarDeclaration *)node->data;
    if (!var_decl) {
      return 1;
    }

    if (!rewrite_type_string_in_place(&var_decl->type_name, rewrites,
                                      rewrite_count, bindings, binding_count)) {
      return 0;
    }

    if (var_decl->initializer &&
        !rewrite_node_names(var_decl->initializer, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }

    if (scope) {
      return scope_add_name(scope, var_decl->name);
    }

    return rename_string_if_needed(&var_decl->name, rewrites, rewrite_count);
  }

  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION:
  case AST_STRUCT_DECLARATION:
  case AST_ENUM_DECLARATION:
  case AST_TYPE_DECLARATION:
  case AST_TRAIT_DECLARATION:
  case AST_IMPL_DECLARATION:
    return rewrite_declaration_names(node, rewrites, rewrite_count, bindings,
                                     binding_count, scope,
                                     program_creates_scope);

  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)node->data;
    if (!assignment) {
      return 1;
    }

    if (assignment->target &&
        !rewrite_node_names(assignment->target, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }

    if (assignment->value &&
        !rewrite_node_names(assignment->value, rewrites, rewrite_count, bindings,
                            binding_count, scope, 1)) {
      return 0;
    }

    if (assignment->variable_name &&
        !scope_contains(scope, assignment->variable_name) &&
        !rename_string_if_needed(&assignment->variable_name, rewrites,
                                 rewrite_count)) {
      return 0;
    }

    return 1;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)node->data;
    if (!call) {
      return 1;
    }

    for (size_t i = 0; i < call->type_arg_count; i++) {
      if (!rewrite_type_string_in_place(&call->type_args[i], rewrites,
                                        rewrite_count, bindings,
                                        binding_count)) {
        return 0;
      }
    }

    if (call->object &&
        !rewrite_node_names(call->object, rewrites, rewrite_count, bindings,
                            binding_count, scope, 1)) {
      return 0;
    }

    for (size_t i = 0; i < call->argument_count; i++) {
      if (!rewrite_node_names(call->arguments[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }

    if (call->object && call->object->type == AST_IDENTIFIER) {
      Identifier *object_ident = (Identifier *)call->object->data;
      if (object_ident && object_ident->name &&
          !scope_contains(scope, object_ident->name) &&
          namespace_binding_allows_member(bindings, binding_count,
                                          object_ident->name,
                                          call->function_name)) {
        char *qualified =
            build_qualified_name(object_ident->name, call->function_name);
        if (!qualified) {
          return 0;
        }
        if (!replace_interned_string(&call->function_name, qualified)) {
          free(qualified);
          return 0;
        }
        free(qualified);
        call->object = NULL;
        if (!rebuild_call_children(node)) {
          return 0;
        }
        return 1;
      }
    }

    if (!call->object && call->function_name &&
        !scope_contains(scope, call->function_name) &&
        !rename_string_if_needed(&call->function_name, rewrites,
                                 rewrite_count)) {
      return 0;
    }

    return 1;
  }

  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *fp_call = (FuncPtrCall *)node->data;
    if (!fp_call) {
      return 1;
    }

    if (fp_call->function &&
        !rewrite_node_names(fp_call->function, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    for (size_t i = 0; i < fp_call->argument_count; i++) {
      if (!rewrite_node_names(fp_call->arguments[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }
    return 1;
  }

  case AST_GPU_LAUNCH:
    /* A launch contains only expressions and introduces no bindings. Rewriting
     * its children handles namespaced kernel-handle/argument references while
     * keeping launch semantics independent of runtime function names. */
    for (size_t i = 0; i < node->child_count; i++) {
      if (!rewrite_node_names(node->children[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }
    return 1;

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)node->data;
    if (!identifier || !identifier->name ||
        scope_contains(scope, identifier->name)) {
      return 1;
    }
    return rename_string_if_needed(&identifier->name, rewrites, rewrite_count);
  }

  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)node->data;
    ASTNode **old_children = NULL;

    if (!member) {
      return 1;
    }

    if (member->object &&
        !rewrite_node_names(member->object, rewrites, rewrite_count, bindings,
                            binding_count, scope, 1)) {
      return 0;
    }

    if (member->object && member->object->type == AST_IDENTIFIER) {
      Identifier *object_ident = (Identifier *)member->object->data;
      if (object_ident && object_ident->name &&
          !scope_contains(scope, object_ident->name) &&
          namespace_binding_allows_member(bindings, binding_count,
                                          object_ident->name,
                                          member->member)) {
        Identifier *identifier = NULL;
        char *qualified = build_qualified_name(object_ident->name, member->member);
        if (!qualified) {
          return 0;
        }

        old_children = node->children;
        node->children = NULL;
        node->child_count = 0;

        ast_destroy_node(member->object);
        free(old_children);

        mettle_free_string(member->member);
        free(member);

        identifier = malloc(sizeof(Identifier));
        if (!identifier) {
          free(qualified);
          return 0;
        }
        identifier->name = (char *)string_intern(qualified);
        identifier->scope_id = AST_SCOPE_ID_UNRESOLVED;
        free(qualified);
        if (!identifier->name) {
          free(identifier);
          return 0;
        }

        node->type = AST_IDENTIFIER;
        node->data = identifier;
      }
    }

    return 1;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)node->data;
    if (!binary) {
      return 1;
    }
    return rewrite_node_names(binary->left, rewrites, rewrite_count, bindings,
                              binding_count, scope, 1) &&
           rewrite_node_names(binary->right, rewrites, rewrite_count, bindings,
                              binding_count, scope, 1);
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)node->data;
    if (!unary) {
      return 1;
    }
    return rewrite_node_names(unary->operand, rewrites, rewrite_count, bindings,
                              binding_count, scope, 1);
  }

  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index_expr = (ArrayIndexExpression *)node->data;
    if (!index_expr) {
      return 1;
    }
    return rewrite_node_names(index_expr->array, rewrites, rewrite_count,
                              bindings, binding_count, scope, 1) &&
           rewrite_node_names(index_expr->index, rewrites, rewrite_count,
                              bindings, binding_count, scope, 1);
  }

  case AST_NEW_EXPRESSION: {
    NewExpression *new_expr = (NewExpression *)node->data;
    if (!new_expr) {
      return 1;
    }
    return rewrite_type_string_in_place(&new_expr->type_name, rewrites,
                                        rewrite_count, bindings, binding_count);
  }

  case AST_CAST_EXPRESSION: {
    CastExpression *cast_expr = (CastExpression *)node->data;
    if (!cast_expr) {
      return 1;
    }
    if (!rewrite_type_string_in_place(&cast_expr->type_name, rewrites,
                                      rewrite_count, bindings, binding_count)) {
      return 0;
    }
    return rewrite_node_names(cast_expr->operand, rewrites, rewrite_count,
                              bindings, binding_count, scope, 1);
  }

  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret = (ReturnStatement *)node->data;
    if (!ret || !ret->value) {
      return 1;
    }
    return rewrite_node_names(ret->value, rewrites, rewrite_count, bindings,
                              binding_count, scope, 1);
  }

  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)node->data;
    if (!if_stmt) {
      return 1;
    }
    if (if_stmt->condition &&
        !rewrite_node_names(if_stmt->condition, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    if (if_stmt->then_branch &&
        !rewrite_node_names(if_stmt->then_branch, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    for (size_t i = 0; i < if_stmt->else_if_count; i++) {
      if (if_stmt->else_ifs[i].condition &&
          !rewrite_node_names(if_stmt->else_ifs[i].condition, rewrites,
                              rewrite_count, bindings, binding_count, scope,
                              1)) {
        return 0;
      }
      if (if_stmt->else_ifs[i].body &&
          !rewrite_node_names(if_stmt->else_ifs[i].body, rewrites,
                              rewrite_count, bindings, binding_count, scope,
                              1)) {
        return 0;
      }
    }
    if (if_stmt->else_branch &&
        !rewrite_node_names(if_stmt->else_branch, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    return 1;
  }

  case AST_WHILE_STATEMENT: {
    WhileStatement *while_stmt = (WhileStatement *)node->data;
    if (!while_stmt) {
      return 1;
    }
    if (while_stmt->condition &&
        !rewrite_node_names(while_stmt->condition, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    if (while_stmt->body &&
        !rewrite_node_names(while_stmt->body, rewrites, rewrite_count, bindings,
                            binding_count, scope, 1)) {
      return 0;
    }
    return 1;
  }

  case AST_FOR_STATEMENT: {
    ForStatement *for_stmt = (ForStatement *)node->data;
    RewriteScope loop_scope;
    if (!for_stmt) {
      return 1;
    }

    memset(&loop_scope, 0, sizeof(loop_scope));
    loop_scope.parent = scope;

    if (for_stmt->initializer &&
        !rewrite_node_names(for_stmt->initializer, rewrites, rewrite_count,
                            bindings, binding_count, &loop_scope, 1)) {
      scope_cleanup(&loop_scope);
      return 0;
    }
    if (for_stmt->condition &&
        !rewrite_node_names(for_stmt->condition, rewrites, rewrite_count,
                            bindings, binding_count, &loop_scope, 1)) {
      scope_cleanup(&loop_scope);
      return 0;
    }
    if (for_stmt->increment &&
        !rewrite_node_names(for_stmt->increment, rewrites, rewrite_count,
                            bindings, binding_count, &loop_scope, 1)) {
      scope_cleanup(&loop_scope);
      return 0;
    }
    if (for_stmt->body &&
        !rewrite_node_names(for_stmt->body, rewrites, rewrite_count, bindings,
                            binding_count, &loop_scope, 1)) {
      scope_cleanup(&loop_scope);
      return 0;
    }

    scope_cleanup(&loop_scope);
    return 1;
  }

  case AST_SWITCH_STATEMENT: {
    SwitchStatement *switch_stmt = (SwitchStatement *)node->data;
    if (!switch_stmt) {
      return 1;
    }
    if (switch_stmt->expression &&
        !rewrite_node_names(switch_stmt->expression, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    for (size_t i = 0; i < switch_stmt->case_count; i++) {
      if (!rewrite_node_names(switch_stmt->cases[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }
    return 1;
  }

  case AST_CASE_CLAUSE: {
    CaseClause *case_clause = (CaseClause *)node->data;
    if (!case_clause) {
      return 1;
    }
    if (case_clause->value &&
        !rewrite_node_names(case_clause->value, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    if (case_clause->body &&
        !rewrite_node_names(case_clause->body, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    return 1;
  }

  case AST_MATCH_STATEMENT: {
    MatchStatement *match = (MatchStatement *)node->data;
    if (!match) {
      return 1;
    }

    if (match->expression &&
        !rewrite_node_names(match->expression, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }

    for (size_t i = 0; i < match->arm_count; i++) {
      RewriteScope arm_scope;
      RewriteScope *body_scope = scope;

      if (!match->arms[i].is_default && match->arms[i].variant_name &&
          !rename_string_if_needed(&match->arms[i].variant_name, rewrites,
                                   rewrite_count)) {
        return 0;
      }

      if (match->arms[i].binding_name) {
        memset(&arm_scope, 0, sizeof(arm_scope));
        arm_scope.parent = scope;
        body_scope = &arm_scope;
        if (!scope_add_name(&arm_scope, match->arms[i].binding_name)) {
          scope_cleanup(&arm_scope);
          return 0;
        }
      }

      if (match->arms[i].body &&
          !rewrite_node_names(match->arms[i].body, rewrites, rewrite_count,
                              bindings, binding_count, body_scope, 1)) {
        if (body_scope == &arm_scope) {
          scope_cleanup(&arm_scope);
        }
        return 0;
      }

      if (body_scope == &arm_scope) {
        scope_cleanup(&arm_scope);
      }
    }

    return 1;
  }

  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *defer_stmt = (DeferStatement *)node->data;
    if (!defer_stmt || !defer_stmt->statement) {
      return 1;
    }
    return rewrite_node_names(defer_stmt->statement, rewrites, rewrite_count,
                              bindings, binding_count, scope, 1);
  }

  case AST_INLINE_ASM: {
    InlineAsm *asm_block = (InlineAsm *)node->data;
    char *rewritten = NULL;

    if (!asm_block || !asm_block->assembly_code) {
      return 1;
    }

    rewritten = rewrite_assembly_text(asm_block->assembly_code, rewrites,
                                      rewrite_count, scope);
    if (!rewritten) {
      return 0;
    }
    if (!replace_interned_string(&asm_block->assembly_code, rewritten)) {
      free(rewritten);
      return 0;
    }
    free(rewritten);
    return 1;
  }

  default:
    return 1;
  }
}

static int rewrite_declaration_names(ASTNode *node,
                                     const NameRewrite *rewrites,
                                     size_t rewrite_count,
                                     const NamespaceBinding *bindings,
                                     size_t binding_count,
                                     RewriteScope *scope,
                                     int program_creates_scope) {
  switch (node->type) {
  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION:
    return rewrite_function_declaration_names(
        node, rewrites, rewrite_count, bindings, binding_count, scope, program_creates_scope);
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *struct_decl = (StructDeclaration *)node->data;
    if (!struct_decl) {
      return 1;
    }

    if (!scope &&
        !rename_string_if_needed(&struct_decl->name, rewrites, rewrite_count)) {
      return 0;
    }

    for (size_t i = 0; i < struct_decl->field_count; i++) {
      if (!rewrite_type_string_in_place(&struct_decl->field_types[i], rewrites,
                                        rewrite_count, bindings,
                                        binding_count)) {
        return 0;
      }
    }

    for (size_t i = 0; i < struct_decl->type_param_count; i++) {
      if (struct_decl->type_param_traits &&
          !rewrite_type_string_in_place(&struct_decl->type_param_traits[i],
                                        rewrites, rewrite_count, bindings,
                                        binding_count)) {
        return 0;
      }
    }

    for (size_t i = 0; i < struct_decl->method_count; i++) {
      if (!rewrite_node_names(struct_decl->methods[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }

    return 1;
  }

  case AST_TYPE_DECLARATION: {
    TypeDeclaration *type_decl = (TypeDeclaration *)node->data;
    if (!type_decl) {
      return 1;
    }
    if (!scope &&
        !rename_string_if_needed(&type_decl->name, rewrites, rewrite_count)) {
      return 0;
    }
    if (!rewrite_type_string_in_place(&type_decl->base_type, rewrites,
                                      rewrite_count, bindings,
                                      binding_count)) {
      return 0;
    }
    if (type_decl->predicate &&
        !rewrite_node_names(type_decl->predicate, rewrites, rewrite_count,
                            bindings, binding_count, scope, 1)) {
      return 0;
    }
    return 1;
  }

  case AST_ENUM_DECLARATION: {
    EnumDeclaration *enum_decl = (EnumDeclaration *)node->data;
    if (!enum_decl) {
      return 1;
    }

    if (!scope &&
        !rename_string_if_needed(&enum_decl->name, rewrites, rewrite_count)) {
      return 0;
    }

    if (!scope) {
      for (size_t i = 0; i < enum_decl->variant_count; i++) {
        if (!rename_string_if_needed(&enum_decl->variants[i].name, rewrites,
                                     rewrite_count)) {
          return 0;
        }
      }
    }

    for (size_t i = 0; i < enum_decl->variant_count; i++) {
      if (enum_decl->variants[i].value &&
          !rewrite_node_names(enum_decl->variants[i].value, rewrites,
                              rewrite_count, bindings, binding_count, scope,
                              1)) {
        return 0;
      }
    }

    return 1;
  }

  case AST_TRAIT_DECLARATION: {
    TraitDeclaration *trait_decl = (TraitDeclaration *)node->data;
    if (!trait_decl) {
      return 1;
    }

    if (!scope &&
        !rename_string_if_needed(&trait_decl->name, rewrites, rewrite_count)) {
      return 0;
    }

    for (size_t i = 0; i < trait_decl->method_count; i++) {
      if (!rewrite_node_names(trait_decl->methods[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }

    return 1;
  }

  case AST_IMPL_DECLARATION: {
    ImplDeclaration *impl_decl = (ImplDeclaration *)node->data;
    if (!impl_decl) {
      return 1;
    }

    if (!rewrite_type_string_in_place(&impl_decl->trait_name, rewrites,
                                      rewrite_count, bindings, binding_count)) {
      return 0;
    }

    if (!rewrite_type_string_in_place(&impl_decl->for_type_name, rewrites,
                                      rewrite_count, bindings,
                                      binding_count)) {
      return 0;
    }

    for (size_t i = 0; i < impl_decl->method_count; i++) {
      if (!rewrite_node_names(impl_decl->methods[i], rewrites, rewrite_count,
                              bindings, binding_count, scope, 1)) {
        return 0;
      }
    }

    return 1;
  }
  default:
    return 1;
  }
}

static int rewrite_function_declaration_names(ASTNode *node,
                                              const NameRewrite *rewrites,
                                              size_t rewrite_count,
                                              const NamespaceBinding *bindings,
                                              size_t binding_count,
                                              RewriteScope *scope,
                                              int program_creates_scope) {
  switch (node->type) {
  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION: {
    FunctionDeclaration *func_decl = (FunctionDeclaration *)node->data;
    RewriteScope function_scope;

    if (!func_decl) {
      return 1;
    }

    if (!scope &&
        !rename_string_if_needed(&func_decl->name, rewrites, rewrite_count)) {
      return 0;
    }

    if (!rewrite_type_string_in_place(&func_decl->return_type, rewrites,
                                      rewrite_count, bindings, binding_count)) {
      return 0;
    }

    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      if (!rewrite_type_string_in_place(&func_decl->parameter_types[i], rewrites,
                                        rewrite_count, bindings,
                                        binding_count)) {
        return 0;
      }
    }

    for (size_t i = 0; i < func_decl->type_param_count; i++) {
      if (func_decl->type_param_traits &&
          !rewrite_type_string_in_place(&func_decl->type_param_traits[i],
                                        rewrites, rewrite_count, bindings,
                                        binding_count)) {
        return 0;
      }
    }

    memset(&function_scope, 0, sizeof(function_scope));
    function_scope.parent = scope;
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      if (!scope_add_name(&function_scope, func_decl->parameter_names[i])) {
        scope_cleanup(&function_scope);
        return 0;
      }
    }

    if (func_decl->body &&
        !rewrite_program_names(func_decl->body, rewrites, rewrite_count,
                               bindings, binding_count, &function_scope, 0)) {
      scope_cleanup(&function_scope);
      return 0;
    }

    scope_cleanup(&function_scope);
    return 1;
  }
  default:
    return 1;
  }
}

/* Renaming an imported declaration must not rename the symbol a foreign
 * declaration links against: `extern fn putchar(...)` still has to resolve to
 * putchar after the import gives it a namespaced or private name. Pin the
 * original spelling as the link name before the rewrite runs. */
static int preserve_extern_link_name(ASTNode *decl) {
  char **link_name = NULL;
  const char *decl_name = NULL;

  if (!decl || !decl->data) {
    return 1;
  }

  if (decl->type == AST_FUNCTION_DECLARATION) {
    FunctionDeclaration *func = (FunctionDeclaration *)decl->data;
    if (!func->is_extern) {
      return 1;
    }
    link_name = &func->link_name;
    decl_name = func->name;
  } else if (decl->type == AST_VAR_DECLARATION) {
    VarDeclaration *var = (VarDeclaration *)decl->data;
    if (!var->is_extern) {
      return 1;
    }
    link_name = &var->link_name;
    decl_name = var->name;
  } else {
    return 1;
  }

  if (!decl_name || (*link_name && (*link_name)[0] != '\0')) {
    return 1;
  }

  *link_name = strdup(decl_name);
  return *link_name != NULL;
}

static int collect_namespaced_rewrites(NameRewrite **rewrites,
                                       size_t *rewrite_count,
                                       size_t *rewrite_capacity,
                                       ASTNode *decl, const char *alias) {
  const char *decl_name = NULL;
  char *qualified_name = NULL;

  if (!decl || !alias) {
    return 1;
  }

  if (!preserve_extern_link_name(decl)) {
    return 0;
  }

  decl_name = get_declaration_name(decl);
  if (decl_name) {
    qualified_name = build_qualified_name(alias, decl_name);
    if (!qualified_name ||
        !add_name_rewrite(rewrites, rewrite_count, rewrite_capacity, decl_name,
                          qualified_name)) {
      free(qualified_name);
      return 0;
    }
    free(qualified_name);
  }

  if (decl->type == AST_ENUM_DECLARATION) {
    EnumDeclaration *enum_decl = (EnumDeclaration *)decl->data;
    if (enum_decl) {
      for (size_t i = 0; i < enum_decl->variant_count; i++) {
        qualified_name =
            build_qualified_name(alias, enum_decl->variants[i].name);
        if (!qualified_name ||
            !add_name_rewrite(rewrites, rewrite_count, rewrite_capacity,
                              enum_decl->variants[i].name, qualified_name)) {
          free(qualified_name);
          return 0;
        }
        free(qualified_name);
      }
    }
  }

  return 1;
}

static int add_namespace_members_for_declaration(
    NamespaceBinding **bindings, size_t *binding_count,
    size_t *binding_capacity, ASTNode *decl, const char *alias) {
  const char *decl_name = NULL;

  if (!decl || !alias) {
    return 1;
  }

  decl_name = get_declaration_name(decl);
  if (decl_name && !add_namespace_binding_member(bindings, binding_count,
                                                 binding_capacity, alias,
                                                 decl_name)) {
    return 0;
  }

  if (decl->type == AST_ENUM_DECLARATION) {
    EnumDeclaration *enum_decl = (EnumDeclaration *)decl->data;
    if (enum_decl) {
      for (size_t i = 0; i < enum_decl->variant_count; i++) {
        if (!add_namespace_binding_member(bindings, binding_count,
                                          binding_capacity, alias,
                                          enum_decl->variants[i].name)) {
          return 0;
        }
      }
    }
  }

  return 1;
}

static char *build_private_import_name(const char *module_path,
                                       const char *name) {
  if (!name) {
    return NULL;
  }

  unsigned long long hash =
      (unsigned long long)mettle_fnv1a_hash(module_path ? module_path : "");
  size_t capacity = strlen(name) + 64;
  char *private_name = malloc(capacity);
  if (!private_name) {
    return NULL;
  }

  snprintf(private_name, capacity, "__import_%llx_%s", hash, name);
  return private_name;
}

static int import_selected_name(const ImportDeclaration *import_decl,
                                const char *name) {
  if (!import_decl || !name) {
    return 0;
  }

  for (size_t i = 0; i < import_decl->selected_count; i++) {
    if (strcmp(import_decl->selected_names[i], name) == 0) {
      return 1;
    }
  }

  return 0;
}

static int declaration_is_public_for_import(
    const ImportDeclaration *import_decl, ASTNode *decl, int has_any_export) {
  const char *name = get_declaration_name(decl);

  if (!name) {
    return 1;
  }

  if (import_decl && import_decl->selected_count > 0) {
    return import_selected_name(import_decl, name);
  }

  if (has_any_export) {
    return is_declaration_exported(decl);
  }

  return 1;
}

static int collect_private_dependency_rewrites(
    NameRewrite **rewrites, size_t *rewrite_count, size_t *rewrite_capacity,
    ASTNode *decl, const char *module_path) {
  const char *decl_name = NULL;
  char *private_name = NULL;

  if (!decl) {
    return 1;
  }

  if (!preserve_extern_link_name(decl)) {
    return 0;
  }

  decl_name = get_declaration_name(decl);
  if (decl_name) {
    private_name = build_private_import_name(module_path, decl_name);
    if (!private_name ||
        !add_name_rewrite(rewrites, rewrite_count, rewrite_capacity, decl_name,
                          private_name)) {
      free(private_name);
      return 0;
    }
    free(private_name);
  }

  if (decl->type == AST_ENUM_DECLARATION) {
    EnumDeclaration *enum_decl = (EnumDeclaration *)decl->data;
    if (enum_decl) {
      for (size_t i = 0; i < enum_decl->variant_count; i++) {
        private_name =
            build_private_import_name(module_path, enum_decl->variants[i].name);
        if (!private_name ||
            !add_name_rewrite(rewrites, rewrite_count, rewrite_capacity,
                              enum_decl->variants[i].name, private_name)) {
          free(private_name);
          return 0;
        }
        free(private_name);
      }
    }
  }

  return 1;
}

/* Path-resolution memoization.
 *
 * resolve_import_path() walks the filesystem (_fullpath / fopen probes / .deps
 * reads) for every import edge. In a real project the same module is imported
 * from dozens of places, so the same (current_file, import_path) pair resolves
 * to the same file many times over. The result depends only on those two
 * strings plus the resolver options (fixed for one compilation), so it is safe
 * to memoize. The cache is process-static and reset at the start of every
 * top-level resolve so independent compilations never share state. */
typedef struct {
  char *key;    /* "<current_file_path>\n<import_path>" */
  char *value;  /* resolved absolute path, or NULL if unresolvable */
  int resolved; /* 1 if this entry represents a completed resolution */
} ImportPathCacheEntry;

static ImportPathCacheEntry *g_import_path_cache = NULL;
static size_t g_import_path_cache_count = 0;
static size_t g_import_path_cache_capacity = 0;

/* Resolved file path -> the module spelling it was imported as. Reflection
 * qualifies type names with this, so `.name` can tell two modules' `Point`
 * apart. Keyed on the resolved path rather than the import edge, so a type
 * reports the module it was DEFINED in no matter how many hops of transitive
 * import reached it. Both strings are interned and outlive the compile. */
typedef struct {
  const char *path;
  const char *module;
} ModuleNameEntry;

static ModuleNameEntry *g_module_names = NULL;
static size_t g_module_name_count = 0;
static size_t g_module_name_capacity = 0;

static void module_name_registry_reset(void) {
  free(g_module_names);
  g_module_names = NULL;
  g_module_name_count = 0;
  g_module_name_capacity = 0;
}

/* First spelling wins: a file reached as both "std/net" and "../std/net" is
 * one module, and picking the first keeps the answer stable within a build. */
static void module_name_registry_record(const char *resolved_path,
                                        const char *module_name) {
  if (!resolved_path || !module_name) {
    return;
  }
  const char *path = string_intern(resolved_path);
  if (!path) {
    return;
  }
  for (size_t i = 0; i < g_module_name_count; i++) {
    if (g_module_names[i].path == path) {
      return;
    }
  }
  if (g_module_name_count == g_module_name_capacity) {
    size_t next = g_module_name_capacity ? g_module_name_capacity * 2 : 32;
    ModuleNameEntry *grown =
        realloc(g_module_names, next * sizeof(ModuleNameEntry));
    if (!grown) {
      return;
    }
    g_module_names = grown;
    g_module_name_capacity = next;
  }
  g_module_names[g_module_name_count].path = path;
  g_module_names[g_module_name_count].module = string_intern(module_name);
  g_module_name_count++;
}

const char *import_resolver_module_for_file(const char *resolved_path) {
  if (!resolved_path || !g_module_names) {
    return NULL;
  }
  for (size_t i = 0; i < g_module_name_count; i++) {
    if (g_module_names[i].path == resolved_path ||
        strcmp(g_module_names[i].path, resolved_path) == 0) {
      return g_module_names[i].module;
    }
  }
  return NULL;
}

static void import_path_cache_reset(void) {
  for (size_t i = 0; i < g_import_path_cache_count; i++) {
    free(g_import_path_cache[i].key);
    free(g_import_path_cache[i].value);
  }
  free(g_import_path_cache);
  g_import_path_cache = NULL;
  g_import_path_cache_count = 0;
  g_import_path_cache_capacity = 0;
}

/* Builds the lookup key. current_file_path may be NULL (root program). */
static char *import_path_cache_make_key(const char *current_file_path,
                                        const char *import_path) {
  const char *cur = current_file_path ? current_file_path : "";
  size_t cur_len = strlen(cur);
  size_t imp_len = strlen(import_path);
  char *key = malloc(cur_len + 1 + imp_len + 1);
  if (!key) {
    return NULL;
  }
  memcpy(key, cur, cur_len);
  key[cur_len] = '\n';
  memcpy(key + cur_len + 1, import_path, imp_len);
  key[cur_len + 1 + imp_len] = '\0';
  return key;
}

static char *resolve_import_path_uncached(ImportContext *ctx,
                                          const char *current_file_path,
                                          const char *import_path);

static char *resolve_import_path(ImportContext *ctx,
                                 const char *current_file_path,
                                 const char *import_path) {
  if (!import_path || import_path[0] == '\0') {
    return NULL;
  }

  char *key = import_path_cache_make_key(current_file_path, import_path);
  if (key) {
    for (size_t i = 0; i < g_import_path_cache_count; i++) {
      if (strcmp(g_import_path_cache[i].key, key) == 0) {
        free(key);
        const char *cached = g_import_path_cache[i].value;
        /* Callers free the returned string, so hand back a fresh copy. */
        if (!cached) {
          return NULL;
        }
        size_t n = strlen(cached) + 1;
        char *copy = malloc(n);
        if (copy) {
          memcpy(copy, cached, n);
        }
        return copy;
      }
    }
  }

  char *resolved =
      resolve_import_path_uncached(ctx, current_file_path, import_path);

  if (key) {
    if (g_import_path_cache_count >= g_import_path_cache_capacity) {
      size_t new_cap =
          g_import_path_cache_capacity == 0 ? 32
                                            : g_import_path_cache_capacity * 2;
      ImportPathCacheEntry *grown = realloc(
          g_import_path_cache, new_cap * sizeof(ImportPathCacheEntry));
      if (grown) {
        g_import_path_cache = grown;
        g_import_path_cache_capacity = new_cap;
      }
    }
    if (g_import_path_cache_count < g_import_path_cache_capacity) {
      char *value_copy = NULL;
      if (resolved) {
        size_t n = strlen(resolved) + 1;
        value_copy = malloc(n);
        if (value_copy) {
          memcpy(value_copy, resolved, n);
        }
      }
      /* Only cache when we could store the value faithfully (a NULL value is
       * itself a valid "unresolvable" result). */
      if (resolved == NULL || value_copy != NULL) {
        g_import_path_cache[g_import_path_cache_count].key = key;
        g_import_path_cache[g_import_path_cache_count].value = value_copy;
        g_import_path_cache[g_import_path_cache_count].resolved = 1;
        g_import_path_cache_count++;
        key = NULL; /* ownership transferred to the cache */
      } else {
        free(value_copy);
      }
    }
    free(key); /* no-op if ownership was transferred */
  }

  return resolved;
}

static char *resolve_import_path_uncached(ImportContext *ctx,
                                          const char *current_file_path,
                                          const char *import_path) {
  if (!import_path || import_path[0] == '\0') {
    return NULL;
  }

  if (is_absolute_path(import_path)) {
    return resolve_candidate_path(import_path);
  }

  if (ctx && ctx->options && ctx->options->stdlib_directory &&
      import_uses_std_namespace(import_path)) {
    /* On the native ELF (Linux) target, prefer an OS-specific
     * `<name>.linux.mettle` sibling so std modules like io/bench/process can
     * ship syscall-based variants while Windows keeps the plain `.mettle`
     * file. The import path has no extension (e.g. "std/io"), so we append
     * ".linux" and let resolve_candidate_path add the ".mettle" extension. */
    if (ctx->options->target_is_elf && !path_has_extension(import_path)) {
      size_t base_len = strlen(import_path);
      /* ".linux.mettle" + NUL. The `.linux` infix makes path_has_extension
       * true, so resolve_candidate_path would not auto-append `.mettle`; spell
       * out the full extension here. */
      char *linux_import = malloc(base_len + 14);
      if (linux_import) {
        memcpy(linux_import, import_path, base_len);
        memcpy(linux_import + base_len, ".linux.mettle", 14);
        char *linux_candidate =
            join_paths(ctx->options->stdlib_directory, linux_import);
        free(linux_import);
        if (linux_candidate) {
          char *resolved = resolve_candidate_path(linux_candidate);
          free(linux_candidate);
          if (resolved) {
            return resolved;
          }
        }
      }
    }

    char *std_candidate =
        join_paths(ctx->options->stdlib_directory, import_path);
    if (std_candidate) {
      char *resolved = resolve_candidate_path(std_candidate);
      free(std_candidate);
      if (resolved) {
        return resolved;
      }
    }
  }

  {
    char *dependency_candidate =
        resolve_dependency_import(current_file_path, import_path);
    if (dependency_candidate) {
      return dependency_candidate;
    }
  }

  char *current_dir = directory_from_file_path(current_file_path);
  if (current_dir) {
    char *local_candidate = join_paths(current_dir, import_path);
    if (local_candidate) {
      char *resolved = resolve_candidate_path(local_candidate);
      free(local_candidate);
      free(current_dir);
      if (resolved) {
        return resolved;
      }
    } else {
      free(current_dir);
    }
  }

  if (ctx && ctx->options && ctx->options->import_directories) {
    for (size_t i = 0; i < ctx->options->import_directory_count; i++) {
      const char *include_dir = ctx->options->import_directories[i];
      if (!include_dir || include_dir[0] == '\0') {
        continue;
      }

      char *include_candidate = join_paths(include_dir, import_path);
      if (!include_candidate) {
        continue;
      }

      char *resolved = resolve_candidate_path(include_candidate);
      free(include_candidate);
      if (resolved) {
        return resolved;
      }
    }
  }

  return resolve_candidate_path(import_path);
}

static char *read_file_content(const char *filename) {
  FILE *file = fopen(filename, "r");
  if (!file)
    return NULL;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  fseek(file, 0, SEEK_SET);
  char *buffer = malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }
  size_t bytes_read = fread(buffer, 1, size, file);
  if (bytes_read < (size_t)size && ferror(file)) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[bytes_read] = '\0';
  fclose(file);
  return buffer;
}

static void stamp_source_locations(ASTNode *node, const char *filename) {
  if (!node || !filename) {
    return;
  }

  if (!node->location.filename) {
    node->location.filename = filename;
  }

  for (size_t i = 0; i < node->child_count; i++) {
    stamp_source_locations(node->children[i], filename);
  }
}

// Check if a declaration is exported (for filtering during import)
static int is_declaration_exported(ASTNode *decl) {
  if (!decl || !decl->data)
    return 0;

  if (decl->type == AST_FUNCTION_DECLARATION) {
    FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
    return fd->is_exported || fd->rewrite_role;
  }
  if (decl->type == AST_STRUCT_DECLARATION) {
    return ((StructDeclaration *)decl->data)->is_exported;
  }
  if (decl->type == AST_ENUM_DECLARATION) {
    return ((EnumDeclaration *)decl->data)->is_exported;
  }
  if (decl->type == AST_TYPE_DECLARATION) {
    return ((TypeDeclaration *)decl->data)->is_exported;
  }
  if (decl->type == AST_TRAIT_DECLARATION) {
    return ((TraitDeclaration *)decl->data)->is_exported;
  }
  if (decl->type == AST_VAR_DECLARATION) {
    return ((VarDeclaration *)decl->data)->is_exported;
  }
  if (decl->type == AST_IMPL_DECLARATION) {
    return 1;
  }
  return 0;
}

static const char *get_declaration_name(ASTNode *decl) {
  if (!decl || !decl->data) {
    return NULL;
  }

  switch (decl->type) {
  case AST_FUNCTION_DECLARATION:
    return ((FunctionDeclaration *)decl->data)->name;
  case AST_STRUCT_DECLARATION:
    return ((StructDeclaration *)decl->data)->name;
  case AST_ENUM_DECLARATION:
    return ((EnumDeclaration *)decl->data)->name;
  case AST_TYPE_DECLARATION:
    return ((TypeDeclaration *)decl->data)->name;
  case AST_TRAIT_DECLARATION:
    return ((TraitDeclaration *)decl->data)->name;
  case AST_VAR_DECLARATION:
    return ((VarDeclaration *)decl->data)->name;
  default:
    return NULL;
  }
}

// Return values:
//   1: added
//   0: already present
//  -1: internal failure (e.g. allocation failure)
static int path_set_add(char ***paths, size_t *count, size_t *capacity,
                        const char *path);
static char *format_import_chain(ImportContext *ctx);

/* Open-addressing name -> declaration-index map, used by the export
 * dependency closure to resolve a called name to its declaration in O(1)
 * instead of a linear scan. Keys are borrowed pointers into the imported
 * module's AST (valid for the lifetime of the map). Stores (decl_index + 1)
 * so 0 marks an empty bucket. */
typedef struct {
  const char **keys;
  size_t *vals; /* decl_index + 1; 0 = empty */
  size_t bucket_count;
} DeclNameMap;

static void decl_name_map_init(DeclNameMap *m, size_t expected_entries) {
  size_t cap = 16;
  while (cap < (expected_entries + 1) * 2) {
    cap *= 2;
  }
  m->keys = calloc(cap, sizeof(const char *));
  m->vals = calloc(cap, sizeof(size_t));
  m->bucket_count = (m->keys && m->vals) ? cap : 0;
  if (!m->keys || !m->vals) {
    free(m->keys);
    free(m->vals);
    m->keys = NULL;
    m->vals = NULL;
  }
}

static void decl_name_map_put(DeclNameMap *m, const char *key,
                              size_t decl_index) {
  if (m->bucket_count == 0 || !key) {
    return;
  }
  size_t mask = m->bucket_count - 1;
  size_t pos = mettle_fnv1a_hash(key) & mask;
  while (m->vals[pos] != 0) {
    if (strcmp(m->keys[pos], key) == 0) {
      return; /* first declaration of a name wins, matching prior behavior */
    }
    pos = (pos + 1) & mask;
  }
  m->keys[pos] = key;
  m->vals[pos] = decl_index + 1;
}

static int decl_name_map_get(const DeclNameMap *m, const char *key,
                             size_t *out_index) {
  if (m->bucket_count == 0 || !key) {
    return 0;
  }
  size_t mask = m->bucket_count - 1;
  size_t pos = mettle_fnv1a_hash(key) & mask;
  while (m->vals[pos] != 0) {
    if (strcmp(m->keys[pos], key) == 0) {
      *out_index = m->vals[pos] - 1;
      return 1;
    }
    pos = (pos + 1) & mask;
  }
  return 0;
}

static void decl_name_map_destroy(DeclNameMap *m) {
  free(m->keys);
  free(m->vals);
  m->keys = NULL;
  m->vals = NULL;
  m->bucket_count = 0;
}

static void collect_type_name_dependencies(const char *type_name,
                                           char ***names, size_t *count,
                                           size_t *capacity) {
  if (!type_name || !names || !count || !capacity) {
    return;
  }

  for (size_t i = 0; type_name[i] != '\0';) {
    if (!is_identifier_start_char(type_name[i])) {
      i++;
      continue;
    }

    size_t start = i;
    i++;
    while (is_identifier_char(type_name[i])) {
      i++;
    }

    char *name = duplicate_string_slice(type_name + start, i - start);
    if (name) {
      (void)path_set_add(names, count, capacity, name);
      free(name);
    }
  }
}

/* Every identifier an asm block names, minus the ones inside quoted runs. The
 * closure cannot tell an instruction mnemonic from a symbol reference, and it
 * does not need to: an extra name keeps a declaration that would otherwise be
 * dropped, and keeping one costs nothing. */
static void collect_assembly_identifiers(const char *assembly, char ***names,
                                         size_t *count, size_t *capacity) {
  size_t i = 0;

  if (!assembly) {
    return;
  }

  while (assembly[i] != '\0') {
    if (assembly[i] == '"' || assembly[i] == '\'') {
      char quote = assembly[i];
      i++;
      while (assembly[i] != '\0' && assembly[i] != quote) {
        if (assembly[i] == '\\' && assembly[i + 1] != '\0') {
          i++;
        }
        i++;
      }
      if (assembly[i] != '\0') {
        i++;
      }
      continue;
    }

    if (is_identifier_start_char(assembly[i])) {
      size_t start = i;
      size_t ident_len = 1;
      char *token = NULL;

      while (is_identifier_char(assembly[start + ident_len])) {
        ident_len++;
      }

      token = duplicate_string_slice(assembly + start, ident_len);
      if (token) {
        (void)path_set_add(names, count, capacity, token);
        free(token);
      }
      i = start + ident_len;
      continue;
    }

    i++;
  }
}

static void collect_dependency_names(ASTNode *node, char ***names,
                                     size_t *count, size_t *capacity) {
  if (!node || !names || !count || !capacity) {
    return;
  }

  switch (node->type) {
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (!prog) {
      return;
    }
    for (size_t i = 0; i < prog->declaration_count; i++) {
      collect_dependency_names(prog->declarations[i], names, count, capacity);
    }
    return;
  }
  case AST_VAR_DECLARATION: {
    VarDeclaration *var = (VarDeclaration *)node->data;
    if (var) {
      collect_type_name_dependencies(var->type_name, names, count, capacity);
      if (var->initializer) {
        collect_dependency_names(var->initializer, names, count, capacity);
      }
    }
    return;
  }
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)node->data;
    if (id && id->name && id->name[0] != '\0') {
      (void)path_set_add(names, count, capacity, id->name);
    }
    return;
  }
  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION:
  case AST_STRUCT_DECLARATION:
  case AST_ENUM_DECLARATION:
  case AST_TYPE_DECLARATION:
  case AST_TRAIT_DECLARATION:
  case AST_IMPL_DECLARATION:
    collect_declaration_dependency_names(node, names, count, capacity);
    return;
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)node->data;
    if (!call) {
      return;
    }
    if (call->function_name && call->function_name[0] != '\0') {
      (void)path_set_add(names, count, capacity, call->function_name);
    }
    for (size_t i = 0; i < call->type_arg_count; i++) {
      collect_type_name_dependencies(call->type_args[i], names, count,
                                     capacity);
    }
    if (call->object) {
      collect_dependency_names(call->object, names, count, capacity);
    }
    for (size_t i = 0; i < call->argument_count; i++) {
      collect_dependency_names(call->arguments[i], names, count, capacity);
    }
    return;
  }
  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *call = (FuncPtrCall *)node->data;
    if (!call) {
      return;
    }
    if (call->function) {
      collect_dependency_names(call->function, names, count, capacity);
    }
    for (size_t i = 0; i < call->argument_count; i++) {
      collect_dependency_names(call->arguments[i], names, count, capacity);
    }
    return;
  }
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      collect_dependency_names(node->children[i], names, count, capacity);
    }
    return;
  case AST_ASSIGNMENT: {
    Assignment *assign = (Assignment *)node->data;
    if (!assign) {
      return;
    }
    /* `name = value` keeps the target in variable_name rather than in a node,
     * so a global that a module only ever writes is reachable through this
     * string alone. */
    if (assign->variable_name && assign->variable_name[0] != '\0') {
      (void)path_set_add(names, count, capacity, assign->variable_name);
    }
    if (assign->target) {
      collect_dependency_names(assign->target, names, count, capacity);
    }
    if (assign->value) {
      collect_dependency_names(assign->value, names, count, capacity);
    }
    return;
  }
  case AST_RETURN_STATEMENT:
    if (node->child_count > 0) {
      collect_dependency_names(node->children[0], names, count, capacity);
    }
    return;
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *bin = (BinaryExpression *)node->data;
    if (bin) {
      collect_dependency_names(bin->left, names, count, capacity);
      collect_dependency_names(bin->right, names, count, capacity);
    }
    return;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *un = (UnaryExpression *)node->data;
    if (un && un->operand) {
      collect_dependency_names(un->operand, names, count, capacity);
    }
    return;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)node->data;
    if (member && member->object) {
      collect_dependency_names(member->object, names, count, capacity);
    }
    return;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *idx = (ArrayIndexExpression *)node->data;
    if (idx) {
      collect_dependency_names(idx->array, names, count, capacity);
      collect_dependency_names(idx->index, names, count, capacity);
    }
    return;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *new_expr = (NewExpression *)node->data;
    if (new_expr) {
      collect_type_name_dependencies(new_expr->type_name, names, count,
                                     capacity);
    }
    return;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)node->data;
    if (!cast) {
      return;
    }
    collect_type_name_dependencies(cast->type_name, names, count, capacity);
    collect_dependency_names(cast->operand, names, count, capacity);
    return;
  }
  case AST_IF_STATEMENT:
  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT:
    for (size_t i = 0; i < node->child_count; i++) {
      collect_dependency_names(node->children[i], names, count, capacity);
    }
    return;
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *sw = (SwitchStatement *)node->data;
    if (sw) {
      if (sw->expression) {
        collect_dependency_names(sw->expression, names, count, capacity);
      }
      for (size_t i = 0; i < sw->case_count; i++) {
        collect_dependency_names(sw->cases[i], names, count, capacity);
      }
    }
    return;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value) {
        collect_dependency_names(cc->value, names, count, capacity);
      }
      if (cc->value_high) {
        collect_dependency_names(cc->value_high, names, count, capacity);
      }
      if (cc->body) {
        collect_dependency_names(cc->body, names, count, capacity);
      }
    }
    return;
  }
  case AST_MATCH_STATEMENT: {
    MatchStatement *match = (MatchStatement *)node->data;
    if (!match) {
      return;
    }
    collect_dependency_names(match->expression, names, count, capacity);
    for (size_t i = 0; i < match->arm_count; i++) {
      if (match->arms[i].variant_name && !match->arms[i].is_default) {
        (void)path_set_add(names, count, capacity,
                           match->arms[i].variant_name);
      }
      collect_dependency_names(match->arms[i].body, names, count, capacity);
    }
    return;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *defer = (DeferStatement *)node->data;
    if (defer) {
      collect_dependency_names(defer->statement, names, count, capacity);
    }
    return;
  }
  case AST_INLINE_ASM: {
    InlineAsm *asm_block = (InlineAsm *)node->data;
    if (asm_block && asm_block->assembly_code) {
      collect_assembly_identifiers(asm_block->assembly_code, names, count,
                                   capacity);
    }
    return;
  }
  default:
    return;
  }
}

static void collect_declaration_dependency_names(ASTNode *node, char ***names,
                                                 size_t *count,
                                                 size_t *capacity) {
  switch (node->type) {
  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION: {
    FunctionDeclaration *func = (FunctionDeclaration *)node->data;
    if (!func) {
      return;
    }
    collect_type_name_dependencies(func->return_type, names, count, capacity);
    for (size_t i = 0; i < func->parameter_count; i++) {
      collect_type_name_dependencies(func->parameter_types[i], names, count,
                                     capacity);
    }
    for (size_t i = 0; i < func->type_param_count; i++) {
      if (func->type_param_traits) {
        collect_type_name_dependencies(func->type_param_traits[i], names,
                                       count, capacity);
      }
    }
    if (func->body) {
      collect_dependency_names(func->body, names, count, capacity);
    }
    return;
  }
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *strct = (StructDeclaration *)node->data;
    if (!strct) {
      return;
    }
    for (size_t i = 0; i < strct->field_count; i++) {
      collect_type_name_dependencies(strct->field_types[i], names, count,
                                     capacity);
    }
    for (size_t i = 0; i < strct->type_param_count; i++) {
      if (strct->type_param_traits) {
        collect_type_name_dependencies(strct->type_param_traits[i], names,
                                       count, capacity);
      }
    }
    for (size_t i = 0; i < strct->method_count; i++) {
      collect_dependency_names(strct->methods[i], names, count, capacity);
    }
    return;
  }
  case AST_TYPE_DECLARATION: {
    TypeDeclaration *td = (TypeDeclaration *)node->data;
    if (!td) {
      return;
    }
    collect_type_name_dependencies(td->base_type, names, count, capacity);
    if (td->predicate) {
      collect_dependency_names(td->predicate, names, count, capacity);
    }
    return;
  }
  case AST_ENUM_DECLARATION: {
    EnumDeclaration *enm = (EnumDeclaration *)node->data;
    if (!enm) {
      return;
    }
    for (size_t i = 0; i < enm->variant_count; i++) {
      collect_type_name_dependencies(enm->variants[i].payload_type, names,
                                     count, capacity);
      if (enm->variants[i].value) {
        collect_dependency_names(enm->variants[i].value, names, count,
                                 capacity);
      }
    }
    return;
  }
  case AST_TRAIT_DECLARATION: {
    TraitDeclaration *trait = (TraitDeclaration *)node->data;
    if (!trait) {
      return;
    }
    for (size_t i = 0; i < trait->method_count; i++) {
      collect_dependency_names(trait->methods[i], names, count, capacity);
    }
    return;
  }
  case AST_IMPL_DECLARATION: {
    ImplDeclaration *impl = (ImplDeclaration *)node->data;
    if (!impl) {
      return;
    }
    collect_type_name_dependencies(impl->trait_name, names, count, capacity);
    collect_type_name_dependencies(impl->for_type_name, names, count,
                                   capacity);
    for (size_t i = 0; i < impl->method_count; i++) {
      collect_dependency_names(impl->methods[i], names, count, capacity);
    }
    return;
  }
  default:
    return;
  }
}

static int expand_import_dependency_closure(Program *prog_data,
                                            int *include_flags,
                                            char ***required_names,
                                            size_t *required_count,
                                            size_t *required_capacity) {
  if (!prog_data || !include_flags || !required_names || !required_count ||
      !required_capacity) {
    return 0;
  }

  size_t decl_count = prog_data->declaration_count;
  DeclNameMap name_map;
  decl_name_map_init(&name_map, decl_count);
  if (decl_count > 0 && name_map.bucket_count == 0) {
    return 0;
  }

  for (size_t i = 0; i < decl_count; i++) {
    const char *name = get_declaration_name(prog_data->declarations[i]);
    if (name) {
      decl_name_map_put(&name_map, name, i);
    }
  }

  size_t *worklist =
      malloc(decl_count ? decl_count * sizeof(size_t) : sizeof(size_t));
  if (!worklist) {
    decl_name_map_destroy(&name_map);
    return 0;
  }

  size_t worklist_len = 0;
  for (size_t i = 0; i < decl_count; i++) {
    if (include_flags[i]) {
      worklist[worklist_len++] = i;
    }
  }

  while (worklist_len > 0) {
    size_t decl_index = worklist[--worklist_len];
    size_t names_before = *required_count;

    collect_dependency_names(prog_data->declarations[decl_index],
                             required_names, required_count,
                             required_capacity);

    for (size_t n = names_before; n < *required_count; n++) {
      size_t target = 0;
      if (decl_name_map_get(&name_map, (*required_names)[n], &target) &&
          !include_flags[target]) {
        include_flags[target] = 1;
        worklist[worklist_len++] = target;
      }
    }
  }

  free(worklist);
  decl_name_map_destroy(&name_map);
  return 1;
}

static int find_declaration_index(Program *prog_data, const char *name,
                                  size_t *out_index) {
  if (!prog_data || !name) {
    return 0;
  }

  for (size_t i = 0; i < prog_data->declaration_count; i++) {
    const char *decl_name = get_declaration_name(prog_data->declarations[i]);
    if (decl_name && strcmp(decl_name, name) == 0) {
      if (out_index) {
        *out_index = i;
      }
      return 1;
    }
  }

  return 0;
}

static void report_selective_import_error(ImportContext *ctx,
                                          ASTNode *import_node,
                                          const char *module_name,
                                          const char *selected_name,
                                          const char *reason) {
  if (!ctx || !ctx->reporter || !import_node || !selected_name || !reason) {
    return;
  }

  char *chain = format_import_chain(ctx);
  char message[1024];
  snprintf(message, sizeof(message),
           "Could not import '%s' from '%s': %s (import chain: %s)",
           selected_name, module_name ? module_name : "<unknown>", reason,
           chain ? chain : "");
  error_reporter_add_error(ctx->reporter, ERROR_SEMANTIC,
                           import_node->location, message);
  free(chain);
}

static int path_set_contains(char **paths, size_t count, const char *path) {
  if (!paths || !path) {
    return 0;
  }

  for (size_t i = 0; i < count; i++) {
    if (strcmp(paths[i], path) == 0) {
      return 1;
    }
  }

  return 0;
}

// Return values:
//   1: added
//   0: already present
//  -1: internal failure (e.g. allocation failure)
static int path_set_add(char ***paths, size_t *count, size_t *capacity,
                        const char *path) {
  if (!paths || !count || !capacity || !path) {
    return -1;
  }

  if (path_set_contains(*paths, *count, path)) {
    return 0;
  }

  if (*count >= *capacity) {
    size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    char **new_paths = realloc(*paths, new_capacity * sizeof(char *));
    if (!new_paths) {
      return -1;
    }
    *paths = new_paths;
    *capacity = new_capacity;
  }

  (*paths)[*count] = strdup(path);
  if (!(*paths)[*count]) {
    return -1;
  }
  (*count)++;
  return 1;
}

static void path_set_remove(char **paths, size_t *count, const char *path) {
  if (!paths || !count || !path) {
    return;
  }

  for (size_t i = 0; i < *count; i++) {
    if (strcmp(paths[i], path) == 0) {
      free(paths[i]);
      if (i + 1 < *count) {
        memmove(&paths[i], &paths[i + 1], ((*count) - i - 1) * sizeof(char *));
      }
      (*count)--;
      return;
    }
  }
}

static int push_import_chain(ImportContext *ctx, const char *path) {
  if (!ctx || !path) {
    return 0;
  }

  if (ctx->chain_depth >= ctx->chain_capacity) {
    size_t new_capacity =
        ctx->chain_capacity == 0 ? 8 : ctx->chain_capacity * 2;
    char **new_chain =
        realloc(ctx->import_chain, new_capacity * sizeof(char *));
    if (!new_chain) {
      return 0;
    }
    ctx->import_chain = new_chain;
    ctx->chain_capacity = new_capacity;
  }

  ctx->import_chain[ctx->chain_depth] = strdup(path);
  if (!ctx->import_chain[ctx->chain_depth]) {
    return 0;
  }
  ctx->chain_depth++;
  return 1;
}

static void pop_import_chain(ImportContext *ctx) {
  if (ctx && ctx->chain_depth > 0) {
    ctx->chain_depth--;
    free(ctx->import_chain[ctx->chain_depth]);
    ctx->import_chain[ctx->chain_depth] = NULL;
  }
}

// Build a human-readable import chain string like "main.mettle -> utils.mettle ->
// math.mettle"
static char *format_import_chain(ImportContext *ctx) {
  if (!ctx || ctx->chain_depth == 0)
    return strdup("");

  // Calculate required length
  size_t total_len = 0;
  for (size_t i = 0; i < ctx->chain_depth; i++) {
    total_len += strlen(ctx->import_chain[i]);
    if (i < ctx->chain_depth - 1)
      total_len += 4; // " -> "
  }

  char *chain_str = malloc(total_len + 1);
  if (!chain_str)
    return strdup("");

  chain_str[0] = '\0';
  for (size_t i = 0; i < ctx->chain_depth; i++) {
    strcat(chain_str, ctx->import_chain[i]);
    if (i < ctx->chain_depth - 1) {
      strcat(chain_str, " -> ");
    }
  }

  return chain_str;
}

static void process_import_strs_in_node(ImportContext *ctx, ASTNode *node,
                                        const char *current_file_path,
                                        int *had_error) {
  if (!node)
    return;

  if (node->type == AST_IMPORT_STR) {
    ImportStrExpression *import_str = (ImportStrExpression *)node->data;
    char *full_path =
        resolve_import_path(ctx, current_file_path, import_str->file_path);

    if (!full_path) {
      if (ctx->reporter) {
        char *chain = format_import_chain(ctx);
        char error_msg[1024];
        snprintf(error_msg, sizeof(error_msg),
                 "Could not resolve embedded file '%s' (import chain: %s)",
                 import_str->file_path, chain);
        error_reporter_add_error(ctx->reporter, ERROR_IO, node->location,
                                 error_msg);
        free(chain);
      }
      *had_error = 1;
      return;
    }

    char *source = read_file_content(full_path);
    if (!source) {
      if (ctx->reporter) {
        char *chain = format_import_chain(ctx);
        char error_msg[1024];
        snprintf(error_msg, sizeof(error_msg),
                 "Could not read embedded file '%s' (import chain: %s)",
                 import_str->file_path, chain);
        error_reporter_add_error(ctx->reporter, ERROR_IO, node->location,
                                 error_msg);
        free(chain);
      }
      free(full_path);
      *had_error = 1;
      return;
    }

    free(full_path);

    // Free old data and change node type in-place
    free(import_str->file_path);
    free(import_str);

    StringLiteral *string_literal = malloc(sizeof(StringLiteral));
    string_literal->value = source;

    node->type = AST_STRING_LITERAL;
    node->data = string_literal;
    return;
  }

  // Traverse via typed data structures to avoid cycles in children[].
  switch (node->type) {
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++) {
        process_import_strs_in_node(ctx, prog->declarations[i],
                                    current_file_path, had_error);
      }
    }
    break;
  }
  case AST_VAR_DECLARATION: {
    VarDeclaration *var = (VarDeclaration *)node->data;
    if (var && var->initializer) {
      process_import_strs_in_node(ctx, var->initializer, current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION: {
    FunctionDeclaration *func = (FunctionDeclaration *)node->data;
    if (func && func->body) {
      process_import_strs_in_node(ctx, func->body, current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *strct = (StructDeclaration *)node->data;
    if (strct) {
      for (size_t i = 0; i < strct->method_count; i++) {
        process_import_strs_in_node(ctx, strct->methods[i], current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_ENUM_DECLARATION: {
    EnumDeclaration *enm = (EnumDeclaration *)node->data;
    if (enm) {
      for (size_t i = 0; i < enm->variant_count; i++) {
        if (enm->variants[i].value) {
          process_import_strs_in_node(ctx, enm->variants[i].value,
                                      current_file_path, had_error);
        }
      }
    }
    break;
  }
  case AST_TRAIT_DECLARATION: {
    TraitDeclaration *trait = (TraitDeclaration *)node->data;
    if (trait) {
      for (size_t i = 0; i < trait->method_count; i++) {
        process_import_strs_in_node(ctx, trait->methods[i], current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_IMPL_DECLARATION: {
    ImplDeclaration *impl = (ImplDeclaration *)node->data;
    if (impl) {
      for (size_t i = 0; i < impl->method_count; i++) {
        process_import_strs_in_node(ctx, impl->methods[i], current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)node->data;
    if (call) {
      if (call->object) {
        process_import_strs_in_node(ctx, call->object, current_file_path,
                                    had_error);
      }
      for (size_t i = 0; i < call->argument_count; i++) {
        process_import_strs_in_node(ctx, call->arguments[i], current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *call = (FuncPtrCall *)node->data;
    if (call) {
      if (call->function) {
        process_import_strs_in_node(ctx, call->function, current_file_path,
                                    had_error);
      }
      for (size_t i = 0; i < call->argument_count; i++) {
        process_import_strs_in_node(ctx, call->arguments[i], current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      process_import_strs_in_node(ctx, node->children[i], current_file_path,
                                  had_error);
    }
    break;
  case AST_ASSIGNMENT: {
    Assignment *assign = (Assignment *)node->data;
    if (assign) {
      if (assign->target) {
        process_import_strs_in_node(ctx, assign->target, current_file_path,
                                    had_error);
      }
      if (assign->value) {
        process_import_strs_in_node(ctx, assign->value, current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    // Return value is child[0] if present
    if (node->child_count > 0) {
      process_import_strs_in_node(ctx, node->children[0], current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *bin = (BinaryExpression *)node->data;
    if (bin) {
      process_import_strs_in_node(ctx, bin->left, current_file_path, had_error);
      process_import_strs_in_node(ctx, bin->right, current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *un = (UnaryExpression *)node->data;
    if (un && un->operand) {
      process_import_strs_in_node(ctx, un->operand, current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)node->data;
    if (member && member->object) {
      process_import_strs_in_node(ctx, member->object, current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *idx = (ArrayIndexExpression *)node->data;
    if (idx) {
      if (idx->array) {
        process_import_strs_in_node(ctx, idx->array, current_file_path,
                                    had_error);
      }
      if (idx->index) {
        process_import_strs_in_node(ctx, idx->index, current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)node->data;
    if (cast && cast->operand) {
      process_import_strs_in_node(ctx, cast->operand, current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_IF_STATEMENT:
  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT: {
    // These use children[] but don't create cycles
    for (size_t i = 0; i < node->child_count; i++) {
      process_import_strs_in_node(ctx, node->children[i], current_file_path,
                                  had_error);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *sw = (SwitchStatement *)node->data;
    if (sw) {
      if (sw->expression) {
        process_import_strs_in_node(ctx, sw->expression, current_file_path,
                                    had_error);
      }
      for (size_t i = 0; i < sw->case_count; i++) {
        process_import_strs_in_node(ctx, sw->cases[i], current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value) {
        process_import_strs_in_node(ctx, cc->value, current_file_path,
                                    had_error);
      }
      if (cc->value_high) {
        process_import_strs_in_node(ctx, cc->value_high, current_file_path,
                                    had_error);
      }
      if (cc->body) {
        process_import_strs_in_node(ctx, cc->body, current_file_path,
                                    had_error);
      }
    }
    break;
  }
  case AST_MATCH_STATEMENT: {
    MatchStatement *match = (MatchStatement *)node->data;
    if (match) {
      if (match->expression) {
        process_import_strs_in_node(ctx, match->expression, current_file_path,
                                    had_error);
      }
      for (size_t i = 0; i < match->arm_count; i++) {
        if (match->arms[i].body) {
          process_import_strs_in_node(ctx, match->arms[i].body,
                                      current_file_path, had_error);
        }
      }
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *defer = (DeferStatement *)node->data;
    if (defer && defer->statement) {
      process_import_strs_in_node(ctx, defer->statement, current_file_path,
                                  had_error);
    }
    break;
  }
  default:
    // Leaf nodes or nodes that can't contain import_str (identifiers,
    // literals, member access, etc.)
    break;
  }
}

// A guarded import (`import "..." if windows|linux;`) is included only when its
// platform matches the requested output target.
static int import_platform_matches(const char *guard, int target_is_elf) {
  if (!guard) {
    return 1; // unconditional import
  }
  const char *target = target_is_elf ? "linux" : "windows";
  return strcmp(guard, target) == 0;
}

static ASTNode *process_imports_recursive(ImportContext *ctx, ASTNode *program,
                                          const char *current_file_path,
                                          int *had_error, int is_nested) {
  (void)is_nested; // Reserved for future use (e.g. filtering main from nested
                   // imports)
  if (!program || program->type != AST_PROGRAM)
    return program;
  Program *prog_data = (Program *)program->data;

  ASTNode **new_declarations = NULL;
  size_t new_declaration_count = 0;
  size_t new_capacity = 0;
  NamespaceBinding *namespace_bindings = NULL;
  size_t namespace_binding_count = 0;
  size_t namespace_binding_capacity = 0;

#define ADD_DECL(decl_node)                                                    \
  do {                                                                         \
    if (new_declaration_count >= new_capacity) {                               \
      size_t grown_capacity = new_capacity == 0 ? 16 : new_capacity * 2;       \
      ASTNode **grown =                                                        \
          realloc(new_declarations, grown_capacity * sizeof(ASTNode *));       \
      if (!grown) {                                                            \
        *had_error = 1;                                                        \
        goto process_imports_cleanup;                                          \
      }                                                                        \
      new_capacity = grown_capacity;                                           \
      new_declarations = grown;                                                \
    }                                                                          \
    new_declarations[new_declaration_count++] = (decl_node);                   \
  } while (0)

  for (size_t i = 0; i < prog_data->declaration_count; i++) {
    ASTNode *decl = prog_data->declarations[i];
    if (decl->type == AST_IMPORT) {
      ImportDeclaration *import_decl = (ImportDeclaration *)decl->data;

      // Drop imports guarded for a different platform before resolving them,
      // so a platform-specific module is never even looked up off-target.
      if (!import_platform_matches(
              import_decl->platform_guard,
              ctx && ctx->options && ctx->options->target_is_elf)) {
        ast_destroy_node(decl);
        continue;
      }

      char *full_path =
          resolve_import_path(ctx, current_file_path, import_decl->module_name);

      if (!full_path) {
        if (ctx->reporter) {
          char *chain = format_import_chain(ctx);
          char error_msg[1024];
          snprintf(error_msg, sizeof(error_msg),
                   "Could not resolve imported file '%s' (import chain: %s)",
                   import_decl->module_name, chain);
          error_reporter_add_error(ctx->reporter, ERROR_IO, decl->location,
                                   error_msg);
          free(chain);
        }
        ast_destroy_node(decl);
        *had_error = 1;
        continue;
      }

      if (path_set_contains(ctx->active_files, ctx->active_count, full_path)) {
        if (ctx->reporter) {
          char *chain = format_import_chain(ctx);
          char error_msg[1024];
          snprintf(error_msg, sizeof(error_msg),
                   "Circular import of '%s' (import chain: %s -> %s)",
                   import_decl->module_name, chain, import_decl->module_name);
          error_reporter_add_warning(ctx->reporter, ERROR_IO, decl->location,
                                     error_msg);
          free(chain);
        }
        free(full_path);
        ast_destroy_node(decl);
        continue;
      }

      if (!import_decl->namespace_alias && import_decl->selected_count == 0 &&
          path_set_contains(ctx->resolved_files, ctx->resolved_count,
                            full_path)) {
        // Duplicate plain import of an already-resolved module is a no-op.
        free(full_path);
        ast_destroy_node(decl);
        continue;
      }

      int add_active_status = path_set_add(&ctx->active_files, &ctx->active_count,
                                           &ctx->active_capacity, full_path);
      if (add_active_status < 0) {
        if (ctx->reporter) {
          error_reporter_add_error(
              ctx->reporter, ERROR_INTERNAL, decl->location,
              "Failed to track active imports (out of memory)");
        }
        free(full_path);
        ast_destroy_node(decl);
        *had_error = 1;
        continue;
      }

      if (!push_import_chain(ctx, import_decl->module_name)) {
        path_set_remove(ctx->active_files, &ctx->active_count, full_path);
        free(full_path);
        ast_destroy_node(decl);
        *had_error = 1;
        continue;
      }

      char *source = read_file_content(full_path);
      if (!source) {
        if (ctx->reporter) {
          char *chain = format_import_chain(ctx);
          char error_msg[1024];
          snprintf(error_msg, sizeof(error_msg),
                   "Could not read imported file '%s' (import chain: %s)",
                   import_decl->module_name, chain);
          error_reporter_add_error(ctx->reporter, ERROR_IO, decl->location,
                                   error_msg);
          free(chain);
        }
        path_set_remove(ctx->active_files, &ctx->active_count, full_path);
        pop_import_chain(ctx);
        free(full_path);
        ast_destroy_node(decl);
        *had_error = 1;
        continue;
      }

      const char *previous_reporter_filename =
          error_reporter_current_filename(ctx->reporter);
      const char *previous_reporter_source =
          error_reporter_current_source_code(ctx->reporter);
      if (ctx->reporter &&
          !error_reporter_set_source_context(ctx->reporter, full_path,
                                             source)) {
        error_reporter_add_error(ctx->reporter, ERROR_INTERNAL, decl->location,
                                 "Failed to register imported source file");
        path_set_remove(ctx->active_files, &ctx->active_count, full_path);
        pop_import_chain(ctx);
        free(full_path);
        free(source);
        ast_destroy_node(decl);
        *had_error = 1;
        continue;
      }
      mettle_compiler_ctx_set_current_filename(full_path);
      module_name_registry_record(full_path, import_decl->module_name);

      size_t parse_error_count_before =
          ctx->reporter ? (size_t)error_reporter_get_error_count(ctx->reporter)
                        : 0;
      Lexer *lexer = lexer_create(source);
      Parser *parser = parser_create_with_error_reporter(lexer, ctx->reporter);
      ASTNode *imported_program = parser_parse_program(parser);

      int import_succeeded = 0;
      if (parser->had_error || !imported_program) {
        if (ctx->reporter &&
            (size_t)error_reporter_get_error_count(ctx->reporter) ==
                parse_error_count_before) {
          char *chain = format_import_chain(ctx);
          char error_msg[1024];
          snprintf(error_msg, sizeof(error_msg),
                   "Parse error in imported file '%s' (import chain: %s)",
                   import_decl->module_name, chain);
          SourceLocation fallback_location = source_location_create(1, 1);
          fallback_location.filename = parser ? parser->source_filename : full_path;
          error_reporter_add_error(ctx->reporter, ERROR_SYNTAX,
                                   fallback_location, error_msg);
          free(chain);
        }
        *had_error = 1;
        if (imported_program) {
          ast_destroy_node(imported_program);
        }
      } else {
        ImportContext child_ctx;
        ImportContext *import_ctx = ctx;
        int using_child_ctx = 0;
        stamp_source_locations(imported_program, parser->source_filename);

        if (import_decl->namespace_alias) {
          if (!import_context_init_child(&child_ctx, ctx)) {
            if (ctx->reporter) {
              error_reporter_add_error(
                  ctx->reporter, ERROR_INTERNAL, decl->location,
                  "Failed to initialize namespaced import context");
            }
            *had_error = 1;
            ast_destroy_node(imported_program);
            imported_program = NULL;
          } else {
            import_ctx = &child_ctx;
            using_child_ctx = 1;
          }
        }

        import_succeeded = imported_program != NULL;
        if (imported_program) {
          // Recursively resolve imports in the imported module
          imported_program = process_imports_recursive(import_ctx,
                                                       imported_program,
                                                       full_path, had_error, 1);
          process_import_strs_in_node(import_ctx, imported_program, full_path,
                                      had_error);
        }

        if (imported_program) {
          Program *imported_prog_data = (Program *)imported_program->data;
          int has_any_export = 0;
          int include_all = 0;
          int *include_flags = NULL;
          char **required_names = NULL;
          size_t required_count = 0;
          size_t required_capacity = 0;
          NameRewrite *name_rewrites = NULL;
          size_t name_rewrite_count = 0;
          size_t name_rewrite_capacity = 0;

          for (size_t j = 0; j < imported_prog_data->declaration_count; j++) {
            if (is_declaration_exported(imported_prog_data->declarations[j])) {
              has_any_export = 1;
              break;
            }
          }

          if (import_decl->selected_count > 0) {
            include_flags =
                calloc(imported_prog_data->declaration_count, sizeof(int));
            if (!include_flags) {
              *had_error = 1;
              import_succeeded = 0;
              if (ctx->reporter) {
                error_reporter_add_error(
                    ctx->reporter, ERROR_INTERNAL, decl->location,
                    "Failed to process selective import (out of memory)");
              }
            } else {
              for (size_t s = 0; s < import_decl->selected_count; s++) {
                const char *selected_name = import_decl->selected_names[s];
                size_t selected_index = 0;

                if (!find_declaration_index(imported_prog_data, selected_name,
                                            &selected_index)) {
                  report_selective_import_error(
                      ctx, decl, import_decl->module_name, selected_name,
                      "no top-level declaration with that name exists");
                  *had_error = 1;
                  import_succeeded = 0;
                  continue;
                }

                if (has_any_export &&
                    !is_declaration_exported(
                        imported_prog_data->declarations[selected_index])) {
                  report_selective_import_error(
                      ctx, decl, import_decl->module_name, selected_name,
                      "the declaration is not exported by that module");
                  *had_error = 1;
                  import_succeeded = 0;
                  continue;
                }

                if (path_set_add(&required_names, &required_count,
                                 &required_capacity, selected_name) < 0) {
                  if (ctx->reporter) {
                    error_reporter_add_error(
                        ctx->reporter, ERROR_INTERNAL, decl->location,
                        "Failed to process selective import (out of memory)");
                  }
                  *had_error = 1;
                  import_succeeded = 0;
                  break;
                }

                include_flags[selected_index] = 1;
              }

              if (import_succeeded &&
                  !expand_import_dependency_closure(
                      imported_prog_data, include_flags, &required_names,
                      &required_count, &required_capacity)) {
                if (ctx->reporter) {
                  error_reporter_add_error(
                      ctx->reporter, ERROR_INTERNAL, decl->location,
                      "Failed to process selective import dependencies");
                }
                *had_error = 1;
                import_succeeded = 0;
              }
            }
          } else {
            if (has_any_export) {
              include_flags =
                  calloc(imported_prog_data->declaration_count, sizeof(int));
              if (!include_flags) {
                include_all = 1;
                *had_error = 1;
                if (ctx->reporter) {
                  error_reporter_add_error(
                      ctx->reporter, ERROR_INTERNAL, decl->location,
                      "Failed to process exports (out of memory)");
                }
              } else {
                /* Export dependency closure.
                 *
                 * Previous implementation: a `while(changed)` fixpoint that
                 * rescanned every declaration each pass and tested membership
                 * with a linear strcmp over the large required-names set,
                 * O(D^2 * names) per module, the dominant import-phase cost on
                 * real projects.
                 *
                 * New implementation: build a name -> declaration-index map
                 * once (O(D)), then drive a worklist. Each declaration is
                 * processed at most once; every dependency name is looked up
                 * O(1) in the map
                 * to discover the next declaration to pull in. Net cost is
                 * O(D + total_dependency_names). The result (which declarations
                 * end up included) is identical to the fixpoint. */
                size_t decl_count = imported_prog_data->declaration_count;
                DeclNameMap name_map;
                decl_name_map_init(&name_map, decl_count);
                for (size_t j = 0; j < decl_count; j++) {
                  const char *dn =
                      get_declaration_name(imported_prog_data->declarations[j]);
                  if (dn) {
                    decl_name_map_put(&name_map, dn, j);
                  }
                }

                /* Worklist of declaration indices whose called-name set still
                 * needs to be expanded. Seed with all exported declarations. */
                size_t *worklist = malloc(decl_count ? decl_count *
                                                           sizeof(size_t)
                                                     : sizeof(size_t));
                size_t worklist_len = 0;
                if (!worklist) {
                  /* Fall back to include-all on allocation failure rather than
                   * silently dropping declarations. */
                  include_all = 1;
                  *had_error = 1;
                  if (ctx->reporter) {
                    error_reporter_add_error(
                        ctx->reporter, ERROR_INTERNAL, decl->location,
                        "Failed to process exports (out of memory)");
                  }
                  decl_name_map_destroy(&name_map);
                } else {
                  for (size_t j = 0; j < decl_count; j++) {
                    if (is_declaration_exported(
                            imported_prog_data->declarations[j])) {
                      include_flags[j] = 1;
                      worklist[worklist_len++] = j;
                    }
                  }

                  while (worklist_len > 0) {
                    size_t j = worklist[--worklist_len];
                    size_t names_before = required_count;
                    collect_dependency_names(
                        imported_prog_data->declarations[j], &required_names,
                        &required_count, &required_capacity);
                    /* Only the freshly added names can unlock new
                     * declarations; older names were already resolved. */
                    for (size_t n = names_before; n < required_count; n++) {
                      size_t target;
                      if (decl_name_map_get(&name_map, required_names[n],
                                            &target) &&
                          !include_flags[target]) {
                        include_flags[target] = 1;
                        worklist[worklist_len++] = target;
                      }
                    }
                  }
                  free(worklist);
                  decl_name_map_destroy(&name_map);
                }
              }
            } else {
              include_all = 1;
            }
          }

          if (import_succeeded && !import_decl->namespace_alias &&
              (import_decl->selected_count > 0 || has_any_export)) {
            NameRewrite *private_rewrites = NULL;
            size_t private_rewrite_count = 0;
            size_t private_rewrite_capacity = 0;

            for (size_t j = 0; j < imported_prog_data->declaration_count; j++) {
              int include_decl = include_all;
              if (!include_decl && include_flags) {
                include_decl = include_flags[j];
              }
              if (!include_decl ||
                  declaration_is_public_for_import(
                      import_decl, imported_prog_data->declarations[j],
                      has_any_export)) {
                continue;
              }

              if (!collect_private_dependency_rewrites(
                      &private_rewrites, &private_rewrite_count,
                      &private_rewrite_capacity,
                      imported_prog_data->declarations[j], full_path)) {
                if (ctx->reporter) {
                  error_reporter_add_error(
                      ctx->reporter, ERROR_INTERNAL, decl->location,
                      "Failed to isolate private import dependencies");
                }
                *had_error = 1;
                import_succeeded = 0;
                break;
              }
            }

            if (import_succeeded && private_rewrite_count > 0 &&
                !rewrite_program_names(imported_program, private_rewrites,
                                       private_rewrite_count, NULL, 0, NULL,
                                       0)) {
              if (ctx->reporter) {
                error_reporter_add_error(
                    ctx->reporter, ERROR_INTERNAL, decl->location,
                    "Failed to rewrite private import dependencies");
              }
              *had_error = 1;
              import_succeeded = 0;
            }

            free_name_rewrites(private_rewrites, private_rewrite_count);
          }

          if (import_succeeded && import_decl->namespace_alias) {
            for (size_t j = 0; j < imported_prog_data->declaration_count; j++) {
              int include_decl = include_all;
              if (!include_decl && include_flags) {
                include_decl = include_flags[j];
              }
              if (!include_decl) {
                continue;
              }
              if (!collect_namespaced_rewrites(
                      &name_rewrites, &name_rewrite_count,
                      &name_rewrite_capacity,
                      imported_prog_data->declarations[j],
                      import_decl->namespace_alias)) {
                if (ctx->reporter) {
                  error_reporter_add_error(
                      ctx->reporter, ERROR_INTERNAL, decl->location,
                      "Failed to build namespaced import bindings");
                }
                *had_error = 1;
                import_succeeded = 0;
                break;
              }

              if ((!has_any_export ||
                   is_declaration_exported(
                       imported_prog_data->declarations[j])) &&
                  !add_namespace_members_for_declaration(
                      &namespace_bindings, &namespace_binding_count,
                      &namespace_binding_capacity,
                      imported_prog_data->declarations[j],
                      import_decl->namespace_alias)) {
                if (ctx->reporter) {
                  error_reporter_add_error(
                      ctx->reporter, ERROR_INTERNAL, decl->location,
                      "Failed to record namespace import member");
                }
                *had_error = 1;
                import_succeeded = 0;
                break;
              }
            }

            if (import_succeeded &&
                !rewrite_program_names(imported_program, name_rewrites,
                                       name_rewrite_count, NULL, 0, NULL, 0)) {
              if (ctx->reporter) {
                error_reporter_add_error(
                    ctx->reporter, ERROR_INTERNAL, decl->location,
                    "Failed to rewrite namespaced import declarations");
              }
              *had_error = 1;
              import_succeeded = 0;
            }

            if (import_succeeded &&
                !add_namespace_binding(&namespace_bindings,
                                       &namespace_binding_count,
                                       &namespace_binding_capacity,
                                       import_decl->namespace_alias)) {
              if (ctx->reporter) {
                error_reporter_add_error(
                    ctx->reporter, ERROR_INTERNAL, decl->location,
                    "Failed to record namespace alias");
              }
              *had_error = 1;
              import_succeeded = 0;
            }
          }

          for (size_t j = 0; j < imported_prog_data->declaration_count; j++) {
            ASTNode *imp_decl = imported_prog_data->declarations[j];
            int include_decl = import_succeeded ? include_all : 0;
            if (import_succeeded && !include_decl && include_flags) {
              include_decl = include_flags[j];
            }

            if (include_decl) {
              ADD_DECL(imp_decl);
            } else {
              ast_destroy_node(imp_decl);
            }
          }

          free_name_rewrites(name_rewrites, name_rewrite_count);
          if (required_names) {
            for (size_t j = 0; j < required_count; j++) {
              free(required_names[j]);
            }
            free(required_names);
          }
          free(include_flags);

          // Cleanup imported program AST container
          free(imported_prog_data->declarations);
          free(imported_prog_data);
          free(imported_program->children);
          free(imported_program);
        }

        if (using_child_ctx) {
          import_context_destroy(&child_ctx);
        }
      }

      parser_destroy(parser);
      lexer_destroy(lexer);
      if (ctx->reporter) {
        error_reporter_set_source_context(ctx->reporter,
                                          previous_reporter_filename,
                                          previous_reporter_source);
        mettle_compiler_ctx_set_current_filename(previous_reporter_filename);
      }
      free(source);
      path_set_remove(ctx->active_files, &ctx->active_count, full_path);
      if (import_succeeded && !import_decl->namespace_alias &&
          import_decl->selected_count == 0) {
        int add_resolved_status = path_set_add(
            &ctx->resolved_files, &ctx->resolved_count, &ctx->resolved_capacity,
            full_path);
        if (add_resolved_status < 0) {
          if (ctx->reporter) {
            error_reporter_add_error(
                ctx->reporter, ERROR_INTERNAL, decl->location,
                "Failed to track resolved imports (out of memory)");
          }
          *had_error = 1;
        }
      }
      pop_import_chain(ctx);
      free(full_path);

      ast_destroy_node(decl); // destroy the AST_IMPORT node itself

    } else {
      ADD_DECL(decl);
    }
  }

#undef ADD_DECL

process_imports_cleanup:
  // Replace old declarations in current program
  free(prog_data->declarations);
  prog_data->declarations = new_declarations;
  prog_data->declaration_count = new_declaration_count;

  // Update children array to match declarations perfectly
  free(program->children);
  if (new_declaration_count > 0) {
    program->children = malloc(new_declaration_count * sizeof(ASTNode *));
    if (!program->children) {
      *had_error = 1;
      program->child_count = 0;
      return program;
    }
    program->child_count = new_declaration_count;
    for (size_t i = 0; i < new_declaration_count; i++) {
      program->children[i] = new_declarations[i];
    }
  } else {
    program->children = NULL;
    program->child_count = 0;
  }

  if (namespace_binding_count > 0 &&
      !rewrite_program_names(program, NULL, 0, namespace_bindings,
                             namespace_binding_count, NULL, 0)) {
    *had_error = 1;
  }

  free_namespace_bindings(namespace_bindings, namespace_binding_count);

  return program;
}

int resolve_imports_with_options(ASTNode *program, const char *base_path,
                                 ErrorReporter *reporter,
                                 const ImportResolverOptions *options) {
  if (!program || program->type != AST_PROGRAM)
    return 0;

  /* Fresh path-resolution caches per top-level compilation. */
  import_path_cache_reset();
  module_name_registry_reset();
  deps_walk_cache_reset();

  ImportContext ctx;
  ctx.resolved_files = NULL;
  ctx.resolved_count = 0;
  ctx.resolved_capacity = 0;
  ctx.active_files = NULL;
  ctx.active_count = 0;
  ctx.active_capacity = 0;
  ctx.import_chain = NULL;
  ctx.chain_depth = 0;
  ctx.chain_capacity = 0;
  ctx.reporter = reporter;
  ctx.options = options;

  // Register the root file as active + push onto chain
  char *abs_base = canonicalize_path(base_path);
  if (abs_base) {
    path_set_add(&ctx.active_files, &ctx.active_count, &ctx.active_capacity,
                 abs_base);
    free(abs_base);
  }

  // Extract filename for readable chain
  const char *last_slash = strrchr(base_path, '/');
  const char *last_backslash = strrchr(base_path, '\\');
  const char *last_sep =
      (last_slash > last_backslash) ? last_slash : last_backslash;
  const char *base_name = last_sep ? last_sep + 1 : base_path;
  if (!push_import_chain(&ctx, base_name)) {
    for (size_t i = 0; i < ctx.resolved_count; i++) {
      free(ctx.resolved_files[i]);
    }
    free(ctx.resolved_files);
    for (size_t i = 0; i < ctx.active_count; i++) {
      free(ctx.active_files[i]);
    }
    free(ctx.active_files);
    free(ctx.import_chain);
    import_path_cache_reset();
    deps_walk_cache_reset();
    return 0;
  }

  int had_error = 0;
  process_imports_recursive(&ctx, program, base_path, &had_error, 0);
  process_import_strs_in_node(&ctx, program, base_path, &had_error);

  // Cleanup
  pop_import_chain(&ctx);
  for (size_t i = 0; i < ctx.resolved_count; i++) {
    free(ctx.resolved_files[i]);
  }
  free(ctx.resolved_files);
  for (size_t i = 0; i < ctx.active_count; i++) {
    free(ctx.active_files[i]);
  }
  free(ctx.active_files);
  free(ctx.import_chain);

  /* Release cache memory; it is rebuilt fresh on the next top-level resolve.
   * The module-name registry is deliberately NOT released here: the type
   * checker reads it after resolution to qualify type names, and it is reset
   * at the start of the next top-level resolve instead. */
  import_path_cache_reset();
  deps_walk_cache_reset();

  return !had_error;
}

int resolve_imports(ASTNode *program, const char *base_path,
                    ErrorReporter *reporter) {
  ImportResolverOptions options = {0};
  return resolve_imports_with_options(program, base_path, reporter, &options);
}
