#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "mtlc/memory.h"
#include "comptime_value.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  TYPE_INT8,
  TYPE_INT16,
  TYPE_INT32,
  TYPE_INT64,
  TYPE_UINT8,
  TYPE_UINT16,
  TYPE_UINT32,
  TYPE_UINT64,
  TYPE_BOOL,
  /* A one-byte character. Byte-identical to uint8 and it widens into every
   * wider integer the same way, so character arithmetic needs no ceremony.
   * The distinction is what interpolation reads: "{c}" writes the character,
   * where a uint8 would write its number. */
  TYPE_CHAR,
  TYPE_FLOAT32,
  TYPE_FLOAT64,
  TYPE_FLOAT16,
  TYPE_BFLOAT16,
  TYPE_STRING,
  TYPE_FUNCTION_POINTER,
  TYPE_POINTER,
  TYPE_ARRAY,
  TYPE_SLICE, /* fat pointer {ptr, len}; element is base_type, length is runtime */
  TYPE_STRUCT,
  TYPE_ENUM,
  TYPE_TAGGED_ENUM,
  TYPE_VOID,
  /* Compile-time only. A TypeRef / FieldRef value has this type. There is no
   * matching MtlcTypeKind: these never reach the backend. */
  TYPE_TYPE,
  TYPE_FIELD,
  /* A `.fields`-style comptime sequence. Answers `len` and `[i]`; a program
   * can observe one but never hold one. */
  TYPE_SEQUENCE
} TypeKind;

typedef struct Type {
  TypeKind kind;
  char *name;
  /* `volatile T`: reading or writing a value of this type is observable in
   * itself. No access may be removed, merged with another, reordered against
   * another volatile access, or served from a register. */
  int is_volatile;
  size_t size;
  size_t alignment;
  struct Type *base_type; // For pointers and arrays
  size_t array_size;      // For arrays
  size_t view_rank;
  struct Type **fn_param_types; // For function pointers
  size_t fn_param_count;        // For function pointers
  struct Type *fn_return_type;  // For function pointers
  // For a capturing closure: the synthesized environment struct type. NULL for
  // a thin function pointer. The closure VALUE is an 8-byte pointer to a heap
  // record whose field 0 is the code pointer and remaining fields are captures.
  struct Type *closure_env;

  // Struct-specific fields. Names are interned and live for the compile;
  // do not strdup them. Layout (byte/bit offsets) is computed in the frontend
  // by type_compute_layout so const eval can read it.
  char **field_names;
  struct Type **field_types;
  size_t *field_offsets;
  uint32_t *field_bit_offsets; /* bit position within the storage unit */
  uint32_t *field_bit_widths;  /* 0 = whole field, not a bitfield */
  size_t field_count;

  // Tagged enum variant info (TYPE_TAGGED_ENUM only)
  char **tagged_variant_names;
  int *tagged_variant_tags;              // discriminant value per variant
  struct Type **tagged_variant_payloads; // payload type per variant (NULL = none)
  size_t tagged_variant_count;

  // Plain TYPE_ENUM members (interned names + integer values). Tagged enums
  // keep their own arrays above; type_enum_variant_at reads the right one.
  char **enum_member_names;
  long long *enum_member_values;
  size_t enum_member_count;
  size_t tagged_data_offset;   // byte offset of the data union inside the struct
  size_t tagged_data_size;     // size of the data union

  // Template info: for un-instantiated generic enum templates
  char *generic_template_name; // base name e.g. "Option" (NULL if not generic)

  /* Index in the type checker's type table once interned. UINT32_MAX means
   * the type has not been interned yet. TypeRef stores this index. */
  uint32_t type_table_index;

  /* Module-qualified spelling for a user-declared type, e.g. "std/net.Point",
   * interned. This is what reflection's `.name` reports, because a bare name
   * cannot distinguish two modules that both declare `Point` and there are no
   * compile-time string operations to recover the module from. NULL for
   * builtins and for structural types (pointers, arrays), whose `name` is
   * already unambiguous. */
  char *qualified_name;
} Type;

typedef enum { SCOPE_GLOBAL, SCOPE_FUNCTION, SCOPE_BLOCK } ScopeType;

typedef struct Scope {
  ScopeType type;
  size_t scope_id;
  struct Scope *parent;
  struct Symbol **symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  /* Open-addressing hash index over `symbols`, keyed by name. Stores
   * (symbol_index + 1); 0 marks an empty bucket. Lets symbol_table_lookup and
   * the declare-time duplicate check avoid a linear strcmp scan per query.
   * Built lazily once a scope grows past a small threshold. */
  size_t *name_index;
  size_t name_index_bucket_count;
} Scope;

typedef enum {
  SYMBOL_VARIABLE,
  SYMBOL_FUNCTION,
  SYMBOL_STRUCT,
  SYMBOL_ENUM,
  SYMBOL_CONSTANT,
  SYMBOL_PARAMETER,
  SYMBOL_TAGGED_ENUM_CONSTRUCTOR
} SymbolKind;

struct ASTNode;

typedef struct Symbol {
  char *name;
  SymbolKind kind;
  Type *type;
  Scope *scope;
  int is_initialized;
  int is_forward_declaration; // For functions that are declared but not defined
  int is_extern;              // For extern declarations (C interop)
  int is_immutable;           // For local `const`: reassignment is rejected
  int is_address_space_binding; // Fixed GPU storage binding; elements stay mutable
  MtlcAddressSpace address_space; // Neutral GPU storage provenance when known
  int is_builtin;             // Compiler-provided (assert/assert_eq test builtins)
  int is_rule;
  /* SYMBOL_FUNCTION declared with `kernel`: a GPU entry point rather than an
   * ordinary function. On the host side an `extern kernel` declaration carries
   * the device signature, so `dispatch` can type-check its arguments and size
   * its own grid; kernel_block mirrors `kernel(block = ...)`, all zero when
   * the declaration omits it. */
  int is_kernel;
  int kernel_block[3];
  int kernel_threads_per_item;
  char *link_name;            // Link-time symbol name for extern declarations
  /* Declaration site, for "previous declaration here" / "defined here"
     diagnostic notes. Zero line when unknown. */
  size_t decl_line;
  size_t decl_column;
  const char *decl_file;
  /* Set on every scope-chain lookup; drives unused-variable warnings. */
  int is_used;
  /* A numeric const keeps its folded value here even when it needs normal
   * storage, such as a local float const. */
  int has_constant_value;
  int constant_is_float;
  long long constant_integer_value;
  double constant_float_value;
  /* Folded compile-time value, including TypeRef / FieldRef. Numeric consts
   * also keep the fields above so existing integer/float folders stay simple. */
  ComptimeValue comptime_value;
  /* The aggregate literal a `const` table was written as, borrowed from the
   * program. `comptime for` reads the rows out of it; nothing else does. */
  struct ASTNode *constant_initializer;
  /* This symbol is a `comptime for` binding, which exists only while the body
   * it binds is being checked. A scalar one is baked into the nodes that read
   * it, because there is nothing left to read afterwards. */
  int is_comptime_binding;
  union {
    struct {
      int register_id;
      int memory_offset;
      int is_in_register;
      /* Set on SYMBOL_PARAMETER when the parameter is passed indirectly
       * per the Microsoft x64 ABI (struct >8 bytes or non-power-of-2 <=8).
       * memory_offset then names a home slot holding a POINTER to the
       * struct, not the struct itself. See docs/struct-abi-design.md. */
      int is_indirect_param;
    } variable;
    struct {
      char **parameter_names;
      Type **parameter_types;
      size_t parameter_count;
      Type *return_type;
      /* The last parameter gathers: a call may pass any number of its element
       * type there, or one slice of them. */
      int is_variadic;
    } function;
    struct {
      long long value;
    } constant;
    struct {
      Type *enum_type;    // The concrete tagged enum type this constructs
      int tag_value;      // Discriminant value for this variant
      Type *payload_type; // NULL if variant carries no payload
    } constructor;
  } data;
} Symbol;

typedef struct SymbolTable {
  Scope *current_scope;
  Scope *global_scope;
  size_t next_scope_id;
} SymbolTable;

// Function declarations
SymbolTable *symbol_table_create(void);
void symbol_table_destroy(SymbolTable *table);
int symbol_table_enter_scope(SymbolTable *table, ScopeType type);
void symbol_table_exit_scope(SymbolTable *table);
int symbol_table_declare(SymbolTable *table, Symbol *symbol);
Symbol *symbol_table_lookup(SymbolTable *table, const char *name);
Symbol *symbol_table_lookup_current_scope(SymbolTable *table, const char *name);
void symbol_table_insert(SymbolTable *table, Symbol *symbol);
int symbol_table_declare_forward(SymbolTable *table, Symbol *symbol);
int symbol_table_resolve_forward_declaration(SymbolTable *table,
                                             Symbol *symbol);
int symbol_table_validate_declaration(SymbolTable *table, Symbol *symbol);
Scope *symbol_table_get_current_scope(SymbolTable *table);

// Returns a heap-allocated name of the in-scope symbol most similar to
// `name` (typo suggestion for "did you mean?"), or NULL if nothing is close
// enough. Walks the full scope chain (current -> ... -> global). When
// `kinds`/`kind_count` are provided, only symbols of those kinds are
// considered; pass NULL/0 to consider every kind. Caller frees the result.
char *symbol_table_suggest_similar(SymbolTable *table, const char *name,
                                   const SymbolKind *kinds, size_t kind_count);

Symbol *symbol_create(const char *name, SymbolKind kind, Type *type);
void symbol_destroy(Symbol *symbol);
Type *type_create(TypeKind kind, const char *name);
Type *type_create_function_pointer(Type **param_types, size_t param_count,
                                   Type *return_type);
void type_destroy(Type *type);

// Struct type creation and manipulation functions
Type *type_create_struct(const char *name, char **field_names,
                         Type **field_types, size_t field_count);
Type *type_get_field_type(Type *struct_type, const char *field_name);
size_t type_get_field_offset(Type *struct_type, const char *field_name);
size_t type_view_rank(const Type *type);
int type_get_field_index(const Type *struct_type, const char *field_name);

/* One ordered struct (or string) field, as the type table answers it. */
typedef struct TypeField {
  const char *name; /* interned */
  struct Type *type;
  size_t byte_offset;
  uint32_t bit_offset;
  uint32_t bit_width; /* 0 = not a bitfield */
} TypeField;

typedef struct TypeEnumVariant {
  const char *name; /* interned */
  long long value;
  struct Type *payload; /* NULL if the variant carries no payload */
} TypeEnumVariant;

int type_alloc_fields(Type *type, size_t field_count);
int type_set_field(Type *type, size_t index, const char *name,
                   Type *field_type, uint32_t bit_width);
int type_compute_layout(Type *type);

size_t type_field_count(const Type *type);
int type_field_at(const Type *type, size_t index, TypeField *out);
int type_field_by_name(const Type *type, const char *name, TypeField *out);

int type_alloc_enum_members(Type *type, size_t count);
int type_set_enum_member(Type *type, size_t index, const char *name,
                         long long value);
size_t type_enum_variant_count(const Type *type);
int type_enum_variant_at(const Type *type, size_t index,
                         TypeEnumVariant *out);

/* Pointer -> pointee. Array/slice -> element. */
Type *type_pointee(const Type *type);
Type *type_element(const Type *type);
/* Array: static length. Slice: 0 (length is runtime). Others: 0. */
size_t type_len(const Type *type);
int type_has_static_len(const Type *type);

/* 1 if `type` is Type or Field: a comptime-only reflection type with no
 * machine layout. Pointers, arrays, and other wrappers are not themselves
 * comptime-only; use type_contains_comptime_only in the type checker. */
int type_is_comptime_only(const Type *type);

#endif // SYMBOL_TABLE_H
