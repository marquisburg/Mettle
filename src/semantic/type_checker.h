#ifndef TYPE_CHECKER_H
#define TYPE_CHECKER_H

#include <stdio.h>
#include "error/error_reporter.h"
#include "parser/ast.h"

// Forward declarations
struct SymbolTable;
struct Type;
struct Symbol;
struct TrackedBufferExtent;
/* Blocks produced by `comptime for` expansion, keyed by block. Opaque
   here; the table and everything that reads it live in comptime_expand.c. */
typedef struct ComptimeExpansionTable ComptimeExpansionTable;
/* Backing store for comptime sequence values, with one memoized run per
   (type, query). Opaque here; it lives in type_query.c. */
typedef struct ComptimeSequenceArena ComptimeSequenceArena;
/* One `comptime for` binding in effect outside any scope; see
   comptime_bindings below. Defined in comptime_expand.c. */
struct ComptimeBindingSlot;

#include "symbol_table.h"

typedef struct {
  SymbolTable *symbol_table;
  int has_error;
  char *error_message;
  ErrorReporter *error_reporter;
  // Built-in type instances
  Type *builtin_int8;
  Type *builtin_int16;
  Type *builtin_int32;
  Type *builtin_int64;
  Type *builtin_uint8;
  Type *builtin_uint16;
  Type *builtin_uint32;
  Type *builtin_uint64;
  Type *builtin_bool;
  Type *builtin_char;
  Type *builtin_float32;
  Type *builtin_float64;
  Type *builtin_float16;
  Type *builtin_bfloat16;
  Type *builtin_string;
  Type *builtin_cstring;
  /* A pointer with no element type: what an allocator hands out and what a
     deallocator takes. It converts to and from any pointer, which is the whole
     contract, and it cannot be indexed or offset because there is no element
     size to do it with. */
  Type *builtin_rawptr;
  Type *builtin_void;
  /* Compile-time only reflection types. Type is the type of a TypeRef;
   * Field is the type of a FieldRef. Neither has a runtime representation. */
  Type *builtin_type;
  Type *builtin_field;
  /* The type of a `comptime for` binding over a constant table. It is a
     compile-time type like Field, so the escape checks that keep reflection
     out of runtime code cover a row without knowing about tables. */
  Type *builtin_row;
  Type *builtin_sequence;

  /* Interned types, indexed by TypeRef.type_index / FieldRef.type_index. */
  Type **type_table;
  size_t type_table_count;
  size_t type_table_capacity;

  /* Created on the first `comptime for`; NULL in a program that has none. */
  ComptimeExpansionTable *expansions;

  /* Expansion bindings in effect, innermost last. A statement-scope expansion
   * puts its binding in the block's own scope; a module-scope one has no such
   * scope to use, so its binding lives here for the length of one declaration.
   * Both read the same expansion-table entry, so the two agree by
   * construction. */
  struct ComptimeBindingSlot *comptime_bindings;
  size_t comptime_binding_count;
  size_t comptime_binding_capacity;

  /* `Kind`, the enum `.kind` answers with. Registered by the compiler rather
   * than declared in a prelude, so reflection needs no import and no flag, and
   * its variants are reachable only as `Kind.Struct` -- they are deliberately
   * not inserted as bare globals, which would claim names like `Struct` and
   * `Array` out of every program's namespace. */
  Type *builtin_kind;
  /* Created on the first sequence query; NULL in a program with none. */
  ComptimeSequenceArena *sequences;

  // Generic enum template cache: uninstantiated enum AST nodes
  ASTNode **generic_enum_templates;
  size_t generic_enum_template_count;
  Symbol *current_function;
  ASTNode *current_function_decl;
  int loop_depth;
  int switch_depth;
  /* One entry per enclosing loop, holding its label or NULL. `break outer`
   * names a loop that has to be one of these; without the stack an unknown
   * label reached IR lowering and came out as an internal compiler error. */
  const char **loop_labels;
  size_t loop_label_count;
  size_t loop_label_capacity;
  char **tracked_var_names;
  unsigned char *tracked_var_initialized;
  int *tracked_var_scope_depth;
  size_t tracked_var_count;
  size_t tracked_var_capacity;
  size_t *tracked_scope_markers;
  size_t tracked_scope_count;
  size_t tracked_scope_capacity;
  int tracked_scope_depth;
  struct TrackedBufferExtent *tracked_buffer_extents;
  /* Target type for an aggregate literal about to be inferred. An aggregate
   * literal has no type of its own - it takes the type of the binding it
   * initializes - so the caller that knows that type parks it here for the one
   * inference call that may see the literal. Consumed (cleared) on read. */
  Type *aggregate_target_type;
  /* Set alongside it when the literal lands somewhere no code runs: a `const`,
     or a module-scope `var`. An element that is not a constant is then an
     error, because there is nowhere for a store to go. Consumed (cleared)
     with the target type. */
  int aggregate_requires_constant;
  /* The module being checked. Module-scope expansion runs before any `const`
     has a symbol, so a `comptime for` over a table finds the declaration here
     and reads the rows straight out of what the program wrote. */
  struct ASTNode *module_program;
  /* Struct types registered as empty placeholders so a cycle of pointers can
     resolve. A cycle has no order that puts every name before its use, so the
     names are all declared first and the fields filled in afterwards. Each
     entry is claimed by the declaration it stands for and dropped from the
     list, so a real duplicate still reports itself. */
  Type **struct_placeholders;
  size_t struct_placeholder_count;
  size_t struct_placeholder_capacity;
} TypeChecker;

// Function declarations
TypeChecker *type_checker_create(SymbolTable *symbol_table);
TypeChecker *
type_checker_create_with_error_reporter(SymbolTable *symbol_table,
                                        ErrorReporter *error_reporter);
void type_checker_destroy(TypeChecker *checker);
int type_checker_check_program(TypeChecker *checker, ASTNode *program);
Type *type_checker_infer_type(TypeChecker *checker, ASTNode *expression);
int type_checker_are_compatible(Type *type1, Type *type2);

// Built-in type system functions
void type_checker_init_builtin_types(TypeChecker *checker);
Type *type_checker_get_type_by_name(TypeChecker *checker, const char *name);
int type_checker_is_integer_type(Type *type);
/* One rule for what may cross the GPU launch boundary, shared by the kernel
 * declaration check and the `dispatch` argument check so the two can never
 * disagree about a type. */
int type_checker_gpu_abi_type(const Type *type);
int type_checker_is_discrete_type(Type *type);
int type_checker_register_variant_constructor(TypeChecker *checker, Type *te,
                                              const char *enum_name,
                                              const char *variant_name,
                                              size_t index);
int type_checker_is_floating_type(Type *type);
int type_checker_is_numeric_type(Type *type);

// Type inference and promotion functions
Type *type_checker_promote_types(TypeChecker *checker, Type *left, Type *right,
                                 const char *operator);
Type *type_checker_get_larger_type(TypeChecker *checker, Type *type1,
                                   Type *type2);
int type_checker_get_type_rank(Type *type);

// Type compatibility and conversion functions
int type_checker_is_assignable(TypeChecker *checker, Type *dest_type,
                               Type *src_type);
/* Assignability with the source expression in hand. Everything
   type_checker_is_assignable accepts is accepted here, plus one case it cannot
   see: a source that folds to a compile-time integer inside the destination's
   range. The value is known at this point, so the conversion cannot lose
   anything, and `var b: uint8 = 200;` needs no cast. */
int type_checker_is_assignable_from(TypeChecker *checker, Type *dest_type,
                                    Type *src_type, ASTNode *src_expr);
int type_checker_is_implicitly_convertible(Type *from_type, Type *to_type);
int type_checker_is_cast_valid(Type *from, Type *to);

/* Value range of an integer (or bool) type. 0 when `type` is neither. */
int type_checker_integer_bounds(const Type *type, long long *out_min,
                                unsigned long long *out_max);
/* Every value of `from` is representable in `to`. This is the line between the
   conversions Mettle performs silently and the ones that need a cast at the
   site: widen silently, narrow loudly. */
int type_checker_int_conversion_is_value_preserving(const Type *from,
                                                    const Type *to);
/* `value`, read with `src_type`'s signedness, lies inside `dest_type`. */
int type_checker_constant_fits_type(const Type *dest_type, const Type *src_type,
                                    long long value);
/* Report an element operation (index, dereference, offset) on a `rawptr`.
   Always returns 1, so callers can write `if (reject(...)) return NULL;`. */
int type_checker_reject_rawptr_element_use(TypeChecker *checker,
                                           SourceLocation location,
                                           const char *what);
/* Report a value that cannot flow into `dest_type`. Picks M0118 when the
   source is a compile-time integer out of range, M0119 when it is a narrowing
   conversion, and the generic type mismatch otherwise. */
void type_checker_report_assign_mismatch(TypeChecker *checker,
                                         const ASTNode *src_expr,
                                         SourceLocation location,
                                         Type *dest_type, Type *src_type);
void type_checker_set_error(TypeChecker *checker, const char *format, ...);
void type_checker_set_error_at_location(TypeChecker *checker,
                                        SourceLocation location,
                                        const char *format, ...);
void type_checker_report_type_mismatch(TypeChecker *checker,
                                       SourceLocation location,
                                       const char *expected,
                                       const char *actual);
void type_checker_report_undefined_symbol(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const char *symbol_type);
void type_checker_report_duplicate_declaration(TypeChecker *checker,
                                               SourceLocation location,
                                               const char *symbol_name);
void type_checker_report_duplicate_declaration_prev(TypeChecker *checker,
                                                    SourceLocation location,
                                                    const char *symbol_name,
                                                    const Symbol *previous);
void type_checker_report_parameter_shadow(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const Symbol *parameter);
/* --report-launches: print each `dispatch` site's geometry as it is checked. */
void type_checker_set_launch_report(int enabled);

/* --report-launches: print each `dispatch` site's geometry as it is checked. */
void type_checker_set_launch_report(int enabled);

void type_checker_report_type_mismatch_node(TypeChecker *checker,
                                            const ASTNode *node,
                                            const char *expected,
                                            const char *actual);
size_t type_checker_node_span_length(const ASTNode *node);
void type_checker_note_declared_here(TypeChecker *checker,
                                     const Symbol *symbol, const char *what);
void type_checker_mark_captures_used(TypeChecker *checker,
                                    const FunctionDeclaration *lam);
void type_checker_warn_unused_locals(TypeChecker *checker);
void type_checker_register_test_builtin(TypeChecker *checker, const char *name,
                                        size_t parameter_count);

/* Intern `type` in the checker's type table and return its index. Already
 * interned types keep their first index. Returns UINT32_MAX on failure. */
uint32_t type_checker_intern_type(TypeChecker *checker, Type *type);
Type *type_checker_type_from_index(const TypeChecker *checker,
                                   uint32_t index);
/* Intern `type` or return an already-interned structurally equal type.
 * Duplicates are destroyed. Pointer/array/slice types go through this so
 * the table can answer pointee/element/len from a stable TypeRef. */
Type *type_checker_canon_type(TypeChecker *checker, Type *type);

/* Replace every `comptime for` directly inside `block` with its expansions:
 * one clone of the body per field, spliced into the block in field order.
 * Runs before the block's statements are checked, because each expansion is
 * checked against a different field type. Returns 0 with a diagnostic
 * reported if a directive could not expand, leaving `block` unmodified.
 *
 * `module_scope` says what the block is a list of. Inside a function it is a
 * list of statements, and each iteration is spliced in as one block, which is
 * also the scope its binding lives in. At module scope it is a list of
 * declarations, so the iteration's declarations are spliced in individually --
 * a generated function has to land in the module, not in a nested scope that
 * would take it back out again. */
int type_checker_expand_comptime_block(TypeChecker *checker, ASTNode *block,
                                       int module_scope);

/* A generated module-scope declaration is checked with its iteration's binding
 * in effect and its expansion note pushed, so `f.offset` resolves and every
 * diagnostic names the iteration. Paired; `leave` undoes exactly what `enter`
 * did. No-op for a declaration the programmer wrote. */
typedef struct {
  size_t bindings_pushed;
  size_t notes_pushed;
} ComptimeDeclScope;
void type_checker_enter_expansion_decl(TypeChecker *checker,
                                       const ASTNode *declaration,
                                       ComptimeDeclScope *scope);
void type_checker_leave_expansion_decl(TypeChecker *checker,
                                       ComptimeDeclScope *scope);

/* The binding a generated declaration is being checked under, or NULL. It is
 * the same kind of symbol a statement-scope expansion puts in its block's
 * scope, so the two resolve identically; only where it is kept differs. */
Symbol *type_checker_lookup_expansion_binding(const TypeChecker *checker,
                                              const char *name);

/* Report every `ident(...)` that reached the checker unresolved. One can only
 * survive expansion if its own directive failed, so this reports the misuse
 * (an `ident(...)` outside any `comptime for`) rather than cascading. */
int type_checker_check_composed_names(TypeChecker *checker, ASTNode *node);

/* The expansion note for `block`, or NULL if `block` is ordinary source. The
 * caller pushes it as a reporter note frame around the block's check so every
 * diagnostic raised inside names the iteration that produced the code. */
const char *type_checker_expansion_note(TypeChecker *checker,
                                        const ASTNode *block,
                                        SourceSpan *out_origin);

/* Bind the `comptime for` variable in the current scope if `block` is an
 * expansion. Call after entering the block's scope; no-op for other blocks. */
int type_checker_declare_expansion_binding(TypeChecker *checker,
                                           const ASTNode *block);

void type_checker_expansions_destroy(ComptimeExpansionTable *table);

/* What compile-time expansion cost. The ledger exists from the first
 * metaprogram so build time can never be spent here unattributed. */
size_t type_checker_expansion_site_count(const TypeChecker *checker);
size_t type_checker_expansion_total_nodes(const TypeChecker *checker);
void type_checker_report_expansion(const TypeChecker *checker, FILE *out);
/* Fail with a located diagnostic when expansion generated more than `budget`
 * nodes, naming the site that generated the most. */
int type_checker_check_expansion_budget(TypeChecker *checker, size_t budget);

/* Fold `offsetof(Field)` to a byte offset from the type table. */
int type_checker_eval_offsetof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_offset);

long long type_checker_layout_digest(const Type *type);

/* Fold `layoutof(T)` to a digest of everything a stored value of T depends on:
 * kind, size, alignment, and each field's name, offset, width and own layout.
 * Pinning it with static_assert turns a layout change into a failed build. */
int type_checker_eval_layoutof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_digest);

/* Fold a compile-time expression. Returns 0 when the expression is not one. */
int type_checker_eval_comptime(TypeChecker *checker, ASTNode *expression,
                               ComptimeValue *out_value);

/* Reflection queries. Each folds to an ordinary compile-time value, so none of
 * them has a runtime representation. A struct field whose name collides with a
 * query wins: the program's own declaration is never shadowed.
 *   Type:  kind, name, size, align, fields, pointee, element, len
 *   Field: name, type, offset, index
 * `name` is module-qualified for user-declared types and bare for a field. */
int type_checker_type_member_exists(const char *member);
int type_checker_eval_type_member(TypeChecker *checker,
                                  ComptimeValue type_value, const char *member,
                                  ComptimeValue *out_value);
int type_checker_field_member_exists(const char *member);

/* One column of a `comptime for` table row, and whether that column exists.
   A row's members are its struct's fields, plus `.index`. */
int type_checker_eval_row_member(TypeChecker *checker, ComptimeValue row,
                                 const char *member, ComptimeValue *out_value);
int type_checker_row_member_exists(TypeChecker *checker, ComptimeValue row,
                                   const char *member);
int type_checker_eval_field_member(TypeChecker *checker, ComptimeValue field,
                                   const char *member,
                                   ComptimeValue *out_value);
/* A sequence answers `len` and `[i]`, and nothing else: observable without
 * being a container the program can hold. */
int type_checker_eval_sequence_member(TypeChecker *checker,
                                      ComptimeValue sequence,
                                      const char *member,
                                      ComptimeValue *out_value);
int type_checker_eval_sequence_index(ComptimeValue sequence, long long index,
                                     ComptimeValue *out_value);

/* Register `Kind` and its qualified-only variants. Call once, after the
 * builtin types exist and the global scope is open. */
void type_checker_register_kind_enum(TypeChecker *checker);
/* Compute and intern `type`'s module-qualified name from the file it was
 * declared in. No-op if already set. */
void type_checker_set_qualified_name(TypeChecker *checker, Type *type,
                                     const char *filename);
/* Type a folded query answer, baking scalar answers into the AST. */
Type *type_checker_comptime_result(TypeChecker *checker, ComptimeValue value,
                                   ASTNode *expression);
/* As above, but types the answer as `Kind` so it compares against Kind.X. */
Type *type_checker_kind_result(TypeChecker *checker, ComptimeValue value,
                               ASTNode *expression);
void type_checker_sequences_destroy(ComptimeSequenceArena *arena);

/* 1 if `type` or any nested pointer/array/function/struct payload is a
 * comptime-only Type or Field. */
int type_contains_comptime_only(const Type *type);

/* Report that `type` (Type or Field, or a type that contains one) cannot
 * exist at runtime. Returns 1 if a diagnostic was issued. */
int type_checker_reject_no_runtime_repr(TypeChecker *checker,
                                        SourceLocation location,
                                        const Type *type);

/* Report that a Type/Field value is being used as a runtime value. Returns 1
 * if a diagnostic was issued. */
int type_checker_reject_comptime_escape(TypeChecker *checker,
                                        SourceLocation location,
                                        const Type *type);

/* Register `struct_decl`'s name as an empty struct type, so a field spelled
   as a pointer to it resolves before its own fields are known. The next call
   to type_checker_process_struct_declaration for that name fills it in place.
   Returns 0 only on allocation failure; a name already declared is left
   alone, so the duplicate is reported where it always was. */
int type_checker_declare_struct_placeholder(TypeChecker *checker,
                                            ASTNode *struct_decl);

// Struct type processing functions
int type_checker_process_struct_declaration(TypeChecker *checker,
                                            ASTNode *struct_decl);
int type_checker_process_enum_declaration(TypeChecker *checker,
                                          ASTNode *enum_decl);
int type_checker_process_declaration(TypeChecker *checker,
                                     ASTNode *declaration);

// Statement and expression validation functions
int type_checker_check_statement(TypeChecker *checker, ASTNode *statement);
int type_checker_check_expression(TypeChecker *checker, ASTNode *expression);
Type *type_checker_check_binary_expression(TypeChecker *checker,
                                           BinaryExpression *binop,
                                           SourceLocation location);

/* Resolve an AST identifier and record the scope that owns its binding. */
Symbol *type_checker_resolve_identifier(TypeChecker *checker,
                                        Identifier *identifier);

#endif // TYPE_CHECKER_H
