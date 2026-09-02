#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "symbol_table.h"
#include "error/error_reporter.h"
#include "string_intern.h"
#include "common.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int symbol_table_names_equal(const char *lhs, const char *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if (!lhs || !rhs) {
    return 0;
  }
  return strcmp(lhs, rhs) == 0;
}

/* Scopes below this size keep the plain linear scan: building and probing a
 * hash index costs more than a handful of strcmp calls. Function and block
 * scopes almost always stay under this; the global scope (thousands of
 * symbols across all modules) is what the index exists for. */
#define SYMBOL_NAME_INDEX_MIN_SYMBOLS 24

/* Rebuilds a scope's hash index from its current symbol array. */
static int scope_name_index_rebuild(Scope *scope, size_t bucket_count) {
  size_t *buckets = calloc(bucket_count, sizeof(size_t));
  if (!buckets) {
    return 0;
  }
  size_t mask = bucket_count - 1;
  for (size_t i = 0; i < scope->symbol_count; i++) {
    if (!scope->symbols[i] || !scope->symbols[i]->name) {
      continue;
    }
    size_t pos = mettle_fnv1a_hash(scope->symbols[i]->name) & mask;
    while (buckets[pos] != 0) {
      pos = (pos + 1) & mask;
    }
    buckets[pos] = i + 1;
  }
  free(scope->name_index);
  scope->name_index = buckets;
  scope->name_index_bucket_count = bucket_count;
  return 1;
}

/* Ensures the index exists and has room for one more symbol. Returns 0 only on
 * allocation failure; on failure the caller must fall back to a linear scan. */
static int scope_name_index_ensure(Scope *scope) {
  if (scope->symbol_count < SYMBOL_NAME_INDEX_MIN_SYMBOLS) {
    return 0; /* deliberately unindexed: linear scan is cheaper here */
  }
  if (scope->name_index_bucket_count == 0 ||
      ((scope->symbol_count + 1) * 10) >=
          (scope->name_index_bucket_count * 7)) {
    size_t next = scope->name_index_bucket_count == 0
                      ? 64
                      : scope->name_index_bucket_count * 2;
    if (!scope_name_index_rebuild(scope, next)) {
      return 0;
    }
  }
  return 1;
}

/* Called immediately after a symbol is appended at `new_index`. Keeps the
 * scope's name index consistent: ensure() rebuilds and reindexes every
 * symbol (including this one) when the scope first crosses the size threshold
 * or exceeds its load factor, otherwise we insert just the new entry. On
 * allocation failure the index is left empty and lookups fall back to a
 * linear scan, so correctness is preserved either way. */
static void scope_register_appended_symbol(Scope *scope, size_t new_index) {
  if (!scope_name_index_ensure(scope)) {
    return;
  }
  if (scope->name_index_bucket_count == 0) {
    return;
  }
  /* If a rebuild ran inside ensure() it already placed this symbol; detect
   * that so we don't insert a duplicate bucket entry. */
  size_t mask = scope->name_index_bucket_count - 1;
  size_t pos = mettle_fnv1a_hash(scope->symbols[new_index]->name) & mask;
  while (scope->name_index[pos] != 0) {
    if (scope->name_index[pos] == new_index + 1) {
      return; /* already indexed by the rebuild */
    }
    pos = (pos + 1) & mask;
  }
  scope->name_index[pos] = new_index + 1;
}

/* O(1) name lookup within a single scope. Returns the symbol or NULL. Falls
 * back to a linear scan when the scope is small or unindexed. */
static Symbol *scope_lookup_symbol(Scope *scope, const char *name) {
  if (scope->name_index && scope->name_index_bucket_count > 0) {
    size_t mask = scope->name_index_bucket_count - 1;
    size_t pos = mettle_fnv1a_hash(name) & mask;
    while (scope->name_index[pos] != 0) {
      size_t idx = scope->name_index[pos] - 1;
      if (scope->symbols[idx] &&
          symbol_table_names_equal(scope->symbols[idx]->name, name)) {
        return scope->symbols[idx];
      }
      pos = (pos + 1) & mask;
    }
    return NULL;
  }

  for (size_t i = 0; i < scope->symbol_count; i++) {
    if (scope->symbols[i] &&
        symbol_table_names_equal(scope->symbols[i]->name, name)) {
      return scope->symbols[i];
    }
  }
  return NULL;
}

static const char *symbol_table_effective_link_name(const Symbol *symbol) {
  if (!symbol) {
    return NULL;
  }
  if (symbol->is_extern && symbol->link_name && symbol->link_name[0] != '\0') {
    return symbol->link_name;
  }
  return symbol->name;
}

static int symbol_table_link_names_match(const Symbol *lhs, const Symbol *rhs) {
  const char *lhs_name = symbol_table_effective_link_name(lhs);
  const char *rhs_name = symbol_table_effective_link_name(rhs);
  if (!lhs_name || !rhs_name) {
    return lhs_name == rhs_name;
  }
  return symbol_table_names_equal(lhs_name, rhs_name);
}

static int symbol_table_types_compatible(const Type *lhs, const Type *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if (!lhs || !rhs) {
    return 0;
  }
  if (lhs->kind != rhs->kind) {
    return 0;
  }

  switch (lhs->kind) {
  case TYPE_FUNCTION_POINTER:
    if (lhs->fn_param_count != rhs->fn_param_count) {
      return 0;
    }
    if (!symbol_table_types_compatible(lhs->fn_return_type,
                                       rhs->fn_return_type)) {
      return 0;
    }
    for (size_t i = 0; i < lhs->fn_param_count; i++) {
      if (!symbol_table_types_compatible(lhs->fn_param_types[i],
                                         rhs->fn_param_types[i])) {
        return 0;
      }
    }
    return 1;
  case TYPE_ARRAY:
    return lhs->array_size == rhs->array_size &&
           symbol_table_types_compatible(lhs->base_type, rhs->base_type);
  case TYPE_POINTER:
    return symbol_table_types_compatible(lhs->base_type, rhs->base_type);
  case TYPE_SLICE:
    return type_view_rank(lhs) == type_view_rank(rhs) &&
           symbol_table_types_compatible(lhs->base_type, rhs->base_type);
  case TYPE_STRUCT:
    if (lhs->name && rhs->name) {
      return symbol_table_names_equal(lhs->name, rhs->name);
    }
    return lhs->name == rhs->name;
  default:
    return 1;
  }
}

static int symbol_table_function_signatures_match(const Symbol *decl,
                                                  const Symbol *defn) {
  if (!decl || !defn || decl->kind != SYMBOL_FUNCTION ||
      defn->kind != SYMBOL_FUNCTION) {
    return 0;
  }

  if (decl->data.function.parameter_count !=
      defn->data.function.parameter_count) {
    return 0;
  }

  if (decl->is_extern != defn->is_extern) {
    return 0;
  }
  if ((decl->is_extern || defn->is_extern) &&
      !symbol_table_link_names_match(decl, defn)) {
    return 0;
  }

  Type *decl_return = decl->data.function.return_type
                          ? decl->data.function.return_type
                          : decl->type;
  Type *defn_return = defn->data.function.return_type
                          ? defn->data.function.return_type
                          : defn->type;
  if (!symbol_table_types_compatible(decl_return, defn_return)) {
    return 0;
  }

  for (size_t i = 0; i < decl->data.function.parameter_count; i++) {
    Type *decl_param = decl->data.function.parameter_types
                           ? decl->data.function.parameter_types[i]
                           : NULL;
    Type *defn_param = defn->data.function.parameter_types
                           ? defn->data.function.parameter_types[i]
                           : NULL;
    if (!symbol_table_types_compatible(decl_param, defn_param)) {
      return 0;
    }
  }

  return 1;
}

SymbolTable *symbol_table_create(void) {
  SymbolTable *table = malloc(sizeof(SymbolTable));
  if (!table)
    return NULL;

  table->global_scope = malloc(sizeof(Scope));
  if (!table->global_scope) {
    free(table);
    return NULL;
  }

  table->global_scope->type = SCOPE_GLOBAL;
  table->global_scope->scope_id = 0;
  table->global_scope->parent = NULL;
  table->global_scope->symbols = NULL;
  table->global_scope->symbol_count = 0;
  table->global_scope->symbol_capacity = 0;
  table->global_scope->name_index = NULL;
  table->global_scope->name_index_bucket_count = 0;

  table->current_scope = table->global_scope;
  table->next_scope_id = 1;

  return table;
}

static void scope_destroy(Scope *scope) {
  if (!scope)
    return;

  // Free all symbols in this scope
  for (size_t i = 0; i < scope->symbol_count; i++) {
    Symbol *symbol = scope->symbols[i];
    if (symbol) {
      mettle_free_string(symbol->name);
      mettle_free_string(symbol->link_name);
      if (symbol->kind == SYMBOL_FUNCTION) {
        // Free function parameter names (strings we own)
        for (size_t j = 0; j < symbol->data.function.parameter_count; j++) {
          free(symbol->data.function.parameter_names[j]);
          // Note: parameter_types[j] are shared builtin types owned by
          // the type checker, do NOT type_destroy them here.
        }
        free(symbol->data.function.parameter_names);
        free(symbol->data.function.parameter_types);
        // Note: return_type is also a shared builtin type, do NOT destroy.
      }
      // Only destroy types owned by the symbol (struct types).
      // Builtin types (int32, float64, etc.) are shared singletons
      // owned by the type checker, which destroys them separately.
      if (symbol->kind == SYMBOL_STRUCT) {
        type_destroy(symbol->type);
      }
      free(symbol);
    }
  }
  free(scope->symbols);
  free(scope->name_index);
  free(scope);
}

void symbol_table_destroy(SymbolTable *table) {
  if (!table)
    return;

  // Free all scopes starting from current and going up to global
  Scope *current = table->current_scope;
  while (current && current != table->global_scope) {
    Scope *parent = current->parent;
    scope_destroy(current);
    current = parent;
  }

  // Free global scope
  scope_destroy(table->global_scope);
  free(table);
}

int symbol_table_enter_scope(SymbolTable *table, ScopeType type) {
  if (!table)
    return 0;

  Scope *new_scope = malloc(sizeof(Scope));
  if (!new_scope)
    return 0;

  if (table->next_scope_id == SIZE_MAX) {
    free(new_scope);
    return 0;
  }

  new_scope->type = type;
  new_scope->scope_id = table->next_scope_id++;
  new_scope->parent = table->current_scope;
  new_scope->symbols = NULL;
  new_scope->symbol_count = 0;
  new_scope->symbol_capacity = 0;
  new_scope->name_index = NULL;
  new_scope->name_index_bucket_count = 0;

  table->current_scope = new_scope;
  return 1;
}

void symbol_table_exit_scope(SymbolTable *table) {
  if (!table || !table->current_scope ||
      table->current_scope == table->global_scope) {
    return;
  }

  Scope *old_scope = table->current_scope;
  table->current_scope = old_scope->parent;

  // Properly free the old scope
  scope_destroy(old_scope);
}

int symbol_table_declare(SymbolTable *table, Symbol *symbol) {
  if (!table || !symbol || !table->current_scope) {
    return 0; // Failure
  }

  // Validate the declaration
  if (!symbol_table_validate_declaration(table, symbol)) {
    return 0; // Invalid declaration
  }

  // Check for duplicate declaration in current scope only
  Symbol *existing = scope_lookup_symbol(table->current_scope, symbol->name);
  if (existing) {
    // Allow forward declaration resolution for functions
    if (symbol->kind == SYMBOL_FUNCTION && existing->kind == SYMBOL_FUNCTION &&
        existing->is_forward_declaration) {
      if (!symbol_table_function_signatures_match(existing, symbol)) {
        return 0; // Mismatched function signature vs forward declaration
      }
      // Resolve the forward declaration
      existing->is_forward_declaration = 0;
      existing->is_initialized = 1;
      return 1; // Successfully resolved forward declaration
    }
    return 0; // Duplicate declaration
  }

  // Resize symbols array if needed
  if (table->current_scope->symbol_count >=
      table->current_scope->symbol_capacity) {
    size_t new_capacity = table->current_scope->symbol_capacity == 0
                              ? 8
                              : table->current_scope->symbol_capacity * 2;
    Symbol **new_symbols =
        realloc(table->current_scope->symbols, new_capacity * sizeof(Symbol *));
    if (!new_symbols) {
      return 0; // Memory allocation failure
    }
    table->current_scope->symbols = new_symbols;
    table->current_scope->symbol_capacity = new_capacity;
  }

  // Set the symbol's scope
  symbol->scope = table->current_scope;

  // Add symbol to current scope
  size_t new_index = table->current_scope->symbol_count;
  table->current_scope->symbols[new_index] = symbol;
  table->current_scope->symbol_count++;
  scope_register_appended_symbol(table->current_scope, new_index);

  return 1; // Success
}

Symbol *symbol_table_lookup(SymbolTable *table, const char *name) {
  if (!table || !name) {
    return NULL;
  }

  // Search from current scope up to global scope
  Scope *current_scope = table->current_scope;
  while (current_scope) {
    Symbol *found = scope_lookup_symbol(current_scope, name);
    if (found) {
      found->is_used = 1;
      return found;
    }
    // Move to parent scope
    current_scope = current_scope->parent;
  }

  return NULL; // Symbol not found
}

Type *type_create(TypeKind kind, const char *name) {
  Type *type = malloc(sizeof(Type));
  if (!type)
    return NULL;

  type->kind = kind;
  type->name = name ? (char *)string_intern(name) : NULL;
  type->is_volatile = 0;
  type->size = 0;
  type->alignment = 0;
  type->base_type = NULL;
  type->array_size = 0;
  type->view_rank = 0;
  type->fn_param_types = NULL;
  type->fn_param_count = 0;
  type->fn_return_type = NULL;
  type->closure_env = NULL;

  // Initialize struct-specific fields
  type->field_names = NULL;
  type->field_types = NULL;
  type->field_offsets = NULL;
  type->field_bit_offsets = NULL;
  type->field_bit_widths = NULL;
  type->field_count = 0;
  type->tagged_variant_names = NULL;
  type->tagged_variant_tags = NULL;
  type->tagged_variant_payloads = NULL;
  type->tagged_variant_count = 0;
  type->enum_member_names = NULL;
  type->enum_member_values = NULL;
  type->enum_member_count = 0;
  type->tagged_data_offset = 0;
  type->tagged_data_size = 0;
  type->generic_template_name = NULL;
  type->type_table_index = UINT32_MAX;
  type->qualified_name = NULL;

  // Set default sizes
  switch (kind) {
  case TYPE_INT8:
  case TYPE_UINT8:
    type->size = 1;
    type->alignment = 1;
    break;
  case TYPE_INT16:
  case TYPE_UINT16:
  case TYPE_FLOAT16:
  case TYPE_BFLOAT16:
    type->size = 2;
    type->alignment = 2;
    break;
  case TYPE_INT32:
  case TYPE_UINT32:
  case TYPE_FLOAT32:
    type->size = 4;
    type->alignment = 4;
    break;
  case TYPE_INT64:
  case TYPE_UINT64:
  case TYPE_FLOAT64:
  case TYPE_POINTER:
  case TYPE_ENUM:
    type->size = 8;
    type->alignment = 8;
    break;
  default:
    break;
  }

  return type;
}

void type_destroy(Type *type) {
  if (type) {
    // Clean up struct-specific fields
    if (type->field_names) {
      for (size_t i = 0; i < type->field_count; i++) {
        mettle_free_string(type->field_names[i]);
      }
      free(type->field_names);
    }

    if (type->field_types) {
      // Note: Don't destroy field types as they might be shared/referenced
      // elsewhere
      free(type->field_types);
    }

    if (type->field_offsets) {
      free(type->field_offsets);
    }
    if (type->field_bit_offsets) {
      free(type->field_bit_offsets);
    }
    if (type->field_bit_widths) {
      free(type->field_bit_widths);
    }
    if (type->enum_member_names) {
      for (size_t i = 0; i < type->enum_member_count; i++) {
        mettle_free_string(type->enum_member_names[i]);
      }
      free(type->enum_member_names);
    }
    if (type->enum_member_values) {
      free(type->enum_member_values);
    }
    if (type->fn_param_types) {
      free(type->fn_param_types);
    }
    if (type->tagged_variant_names) {
      for (size_t i = 0; i < type->tagged_variant_count; i++) {
        mettle_free_string(type->tagged_variant_names[i]);
      }
      free(type->tagged_variant_names);
    }
    if (type->tagged_variant_tags) {
      free(type->tagged_variant_tags);
    }
    if (type->tagged_variant_payloads) {
      free(type->tagged_variant_payloads);
    }
    if (type->generic_template_name) {
      mettle_free_string(type->generic_template_name);
    }

    mettle_free_string(type->name);
    free(type);
  }
}

Type *type_create_function_pointer(Type **param_types, size_t param_count,
                                   Type *return_type) {
  Type *type = type_create(TYPE_FUNCTION_POINTER, "function");
  if (!type) {
    return NULL;
  }

  type->size = 8;
  type->alignment = 8;
  type->fn_param_count = param_count;
  type->fn_return_type = return_type;

  if (param_count > 0) {
    type->fn_param_types = malloc(param_count * sizeof(Type *));
    if (!type->fn_param_types) {
      type_destroy(type);
      return NULL;
    }
    for (size_t i = 0; i < param_count; i++) {
      type->fn_param_types[i] = param_types ? param_types[i] : NULL;
    }
  }

  return type;
}

Scope *symbol_table_get_current_scope(SymbolTable *table) {
  if (!table)
    return NULL;
  return table->current_scope;
}

static int symbol_kind_allowed(SymbolKind kind, const SymbolKind *kinds,
                               size_t kind_count) {
  if (!kinds || kind_count == 0)
    return 1;
  for (size_t i = 0; i < kind_count; i++) {
    if (kinds[i] == kind)
      return 1;
  }
  return 0;
}

char *symbol_table_suggest_similar(SymbolTable *table, const char *name,
                                   const SymbolKind *kinds,
                                   size_t kind_count) {
  if (!table || !name || name[0] == '\0')
    return NULL;

  /* Collect candidate names across the whole visible scope chain. */
  size_t capacity = 32;
  size_t count = 0;
  const char **names = malloc(capacity * sizeof(*names));
  if (!names)
    return NULL;

  for (Scope *scope = table->current_scope; scope; scope = scope->parent) {
    for (size_t i = 0; i < scope->symbol_count; i++) {
      Symbol *sym = scope->symbols[i];
      if (!sym || !sym->name)
        continue;
      if (!symbol_kind_allowed(sym->kind, kinds, kind_count))
        continue;

      if (count == capacity) {
        size_t new_capacity = capacity * 2;
        const char **grown =
            realloc(names, new_capacity * sizeof(*names));
        if (!grown) {
          free(names);
          return NULL;
        }
        names = grown;
        capacity = new_capacity;
      }
      names[count++] = sym->name;
    }
  }

  char *suggestion =
      error_reporter_closest_candidate(name, names, count);
  free(names);
  return suggestion;
}

Symbol *symbol_create(const char *name, SymbolKind kind, Type *type) {
  if (!name)
    return NULL;

  Symbol *symbol = malloc(sizeof(Symbol));
  if (!symbol)
    return NULL;

  symbol->name = (char *)string_intern(name);
  if (!symbol->name) {
    free(symbol);
    return NULL;
  }

  symbol->kind = kind;
  symbol->type = type;
  symbol->scope = NULL; // Will be set when declared
  symbol->is_initialized = 0;
  symbol->is_forward_declaration = 0;
  symbol->is_extern = 0;
  symbol->is_immutable = 0;
  symbol->is_address_space_binding = 0;
  symbol->address_space = MTLC_ADDRESS_SPACE_DEFAULT;
  symbol->is_builtin = 0;
  symbol->link_name = NULL;
  symbol->decl_line = 0;
  symbol->decl_column = 0;
  symbol->decl_file = NULL;
  symbol->is_used = 0;
  symbol->constant_initializer = NULL;
  symbol->is_comptime_binding = 0;

  // Initialize union data based on symbol kind
  switch (kind) {
  case SYMBOL_VARIABLE:
  case SYMBOL_PARAMETER:
    symbol->data.variable.register_id = -1;
    symbol->data.variable.memory_offset = 0;
    symbol->data.variable.is_in_register = 0;
    symbol->data.variable.is_indirect_param = 0;
    break;
  case SYMBOL_FUNCTION:
    symbol->data.function.parameter_names = NULL;
    symbol->data.function.parameter_types = NULL;
    symbol->data.function.parameter_count = 0;
    symbol->data.function.return_type = NULL;
    symbol->data.function.is_variadic = 0;
    break;
  case SYMBOL_STRUCT:
  case SYMBOL_ENUM:
    // No specific data for struct/enum symbols
    break;
  case SYMBOL_CONSTANT:
    symbol->data.constant.value = 0;
    break;
  case SYMBOL_TAGGED_ENUM_CONSTRUCTOR:
    symbol->data.constructor.enum_type = NULL;
    symbol->data.constructor.tag_value = 0;
    symbol->data.constructor.payload_type = NULL;
    break;
  }

  symbol->has_constant_value = 0;
  symbol->constant_is_float = 0;
  symbol->constant_integer_value = 0;
  symbol->constant_float_value = 0.0;
  symbol->comptime_value = comptime_none();

  return symbol;
}

void symbol_destroy(Symbol *symbol) {
  if (!symbol)
    return;

  mettle_free_string(symbol->name);
  mettle_free_string(symbol->link_name);

  if (symbol->kind == SYMBOL_FUNCTION) {
    // Free function parameter names (strings we own)
    for (size_t i = 0; i < symbol->data.function.parameter_count; i++) {
      free(symbol->data.function.parameter_names[i]);
      // Note: parameter_types[i] are shared builtin types, do NOT destroy
    }
    free(symbol->data.function.parameter_names);
    free(symbol->data.function.parameter_types);
    // Note: return_type is a shared builtin type, do NOT destroy
  }

  // Only destroy types owned by the symbol (struct types)
  if (symbol->kind == SYMBOL_STRUCT) {
    type_destroy(symbol->type);
  }
  free(symbol);
}

Symbol *symbol_table_lookup_current_scope(SymbolTable *table,
                                          const char *name) {
  if (!table || !name || !table->current_scope) {
    return NULL;
  }

  // Search only in current scope
  return scope_lookup_symbol(table->current_scope, name);
}

void symbol_table_insert(SymbolTable *table, Symbol *symbol) {
  if (!table || !symbol || !table->current_scope) {
    return;
  }

  // Resize symbols array if needed
  if (table->current_scope->symbol_count >=
      table->current_scope->symbol_capacity) {
    size_t new_capacity = table->current_scope->symbol_capacity == 0
                              ? 8
                              : table->current_scope->symbol_capacity * 2;
    Symbol **new_symbols =
        realloc(table->current_scope->symbols, new_capacity * sizeof(Symbol *));
    if (!new_symbols) {
      return; // Memory allocation failure
    }
    table->current_scope->symbols = new_symbols;
    table->current_scope->symbol_capacity = new_capacity;
  }

  // Set the symbol's scope
  symbol->scope = table->current_scope;

  // Add symbol to current scope
  size_t new_index = table->current_scope->symbol_count;
  table->current_scope->symbols[new_index] = symbol;
  table->current_scope->symbol_count++;
  scope_register_appended_symbol(table->current_scope, new_index);
}

int symbol_table_declare_forward(SymbolTable *table, Symbol *symbol) {
  if (!table || !symbol || !table->current_scope) {
    return 0; // Failure
  }

  // Only functions can be forward declared
  if (symbol->kind != SYMBOL_FUNCTION) {
    return 0; // Only functions can be forward declared
  }

  // Check if symbol already exists in current scope
  Symbol *existing = symbol_table_lookup_current_scope(table, symbol->name);
  if (existing) {
    // If it's already a forward declaration, check compatibility
    if (existing->kind == SYMBOL_FUNCTION && existing->is_forward_declaration) {
      return symbol_table_function_signatures_match(existing, symbol);
    } else {
      return 0; // Already defined
    }
  }

  // Mark as forward declaration
  symbol->is_forward_declaration = 1;

  // Declare the symbol
  return symbol_table_declare(table, symbol);
}

int symbol_table_resolve_forward_declaration(SymbolTable *table,
                                             Symbol *symbol) {
  if (!table || !symbol || symbol->kind != SYMBOL_FUNCTION) {
    return 0; // Failure
  }

  // Look for existing forward declaration
  Symbol *existing = symbol_table_lookup_current_scope(table, symbol->name);
  if (existing && existing->kind == SYMBOL_FUNCTION &&
      existing->is_forward_declaration) {
    if (!symbol_table_function_signatures_match(existing, symbol)) {
      return 0; // Forward declaration and definition signatures differ
    }
    existing->is_forward_declaration = 0;
    existing->is_initialized = 1;
    return 1; // Successfully resolved
  }

  if (existing) {
    return 0; // Already defined in this scope
  }

  // No forward declaration found, declare normally
  symbol->is_forward_declaration = 0;
  symbol->is_initialized = 1;
  return symbol_table_declare(table, symbol);
}

int symbol_table_validate_declaration(SymbolTable *table, Symbol *symbol) {
  if (!table || !symbol) {
    return 0; // Invalid parameters
  }

  // Check if name is valid (not empty)
  if (!symbol->name || strlen(symbol->name) == 0) {
    return 0; // Invalid name
  }

  // Check if type is valid
  if (!symbol->type) {
    return 0; // Invalid type
  }

  // For functions, validate parameter information
  if (symbol->kind == SYMBOL_FUNCTION) {
    // If parameter count > 0, must have parameter arrays
    if (symbol->data.function.parameter_count > 0) {
      if (!symbol->data.function.parameter_names ||
          !symbol->data.function.parameter_types) {
        return 0; // Invalid function parameters
      }

      // Check each parameter
      for (size_t i = 0; i < symbol->data.function.parameter_count; i++) {
        if (!symbol->data.function.parameter_names[i] ||
            !symbol->data.function.parameter_types[i]) {
          return 0; // Invalid parameter
        }
      }
    }
  }

  // Check for duplicate declaration in current scope
  Symbol *existing = symbol_table_lookup_current_scope(table, symbol->name);
  if (existing) {
    // Allow forward declaration resolution for functions
    if (symbol->kind == SYMBOL_FUNCTION && existing->kind == SYMBOL_FUNCTION &&
        existing->is_forward_declaration) {
      return symbol_table_function_signatures_match(existing, symbol);
    }
    return 0; // Duplicate declaration
  }

  return 1; // Valid declaration
}

// Struct type creation and manipulation functions

Type *type_create_struct(const char *name, char **field_names,
                         Type **field_types, size_t field_count) {
  if (!name || field_count == 0 || !field_names || !field_types) {
    return NULL;
  }

  Type *struct_type = type_create(TYPE_STRUCT, name);
  if (!struct_type) {
    return NULL;
  }

  if (!type_alloc_fields(struct_type, field_count)) {
    type_destroy(struct_type);
    return NULL;
  }

  for (size_t i = 0; i < field_count; i++) {
    if (!type_set_field(struct_type, i, field_names[i], field_types[i], 0)) {
      type_destroy(struct_type);
      return NULL;
    }
  }
  if (!type_compute_layout(struct_type)) {
    type_destroy(struct_type);
    return NULL;
  }

  return struct_type;
}

Type *type_get_field_type(Type *struct_type, const char *field_name) {
  if (!struct_type ||
      (struct_type->kind != TYPE_STRUCT && struct_type->kind != TYPE_STRING &&
       struct_type->kind != TYPE_SLICE) ||
      !field_name) {
    return NULL;
  }

  for (size_t i = 0; i < struct_type->field_count; i++) {
    if (symbol_table_names_equal(struct_type->field_names[i], field_name)) {
      return struct_type->field_types[i];
    }
  }

  if (type_view_rank(struct_type) > 1 && struct_type->field_count > 1 &&
      struct_type->field_types[1] &&
      symbol_table_names_equal(field_name, "length")) {
    return struct_type->field_types[1]->base_type;
  }

  return NULL; // Field not found
}

size_t type_view_rank(const Type *type) {
  if (!type || type->kind != TYPE_SLICE) {
    return 0;
  }
  return type->view_rank > 1 ? type->view_rank : 1;
}

size_t type_get_field_offset(Type *struct_type, const char *field_name) {
  if (!struct_type ||
      (struct_type->kind != TYPE_STRUCT && struct_type->kind != TYPE_STRING &&
       struct_type->kind != TYPE_SLICE) ||
      !field_name) {
    return 0;
  }
  if (type_view_rank(struct_type) > 1 &&
      symbol_table_names_equal(field_name, "length")) {
    return 8;
  }

  for (size_t i = 0; i < struct_type->field_count; i++) {
    if (symbol_table_names_equal(struct_type->field_names[i], field_name)) {
      return struct_type->field_offsets[i];
    }
  }

  return 0; // Field not found
}

int type_is_comptime_only(const Type *type) {
  return type && (type->kind == TYPE_TYPE || type->kind == TYPE_FIELD ||
                  type->kind == TYPE_SEQUENCE);
}

int type_get_field_index(const Type *struct_type, const char *field_name) {
  if (!struct_type ||
      (struct_type->kind != TYPE_STRUCT && struct_type->kind != TYPE_STRING) ||
      !field_name) {
    return -1;
  }

  for (size_t i = 0; i < struct_type->field_count; i++) {
    if (symbol_table_names_equal(struct_type->field_names[i], field_name)) {
      return (int)i;
    }
  }
  return -1;
}
