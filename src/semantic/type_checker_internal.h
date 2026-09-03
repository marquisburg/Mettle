#ifndef TYPE_CHECKER_INTERNAL_H
#define TYPE_CHECKER_INTERNAL_H

// Shared internals for the type checker, split across type_checker*.c modules.
// Public API lives in type_checker.h; this header exposes the cross-module
// helper prototypes and shared file-local types.

#include "type_checker.h"
#include "common.h"
#include "error/error_reporter.h"
#include "string_intern.h"
#include "symbol_table.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TrackedBufferExtent {
  char *name;
  long long byte_count;
  long long known_alignment;
  int scope_depth;
  struct TrackedBufferExtent *next;
} TrackedBufferExtent;

Type *type_checker_parse_array_type(TypeChecker *checker,
                                           const char *name);

int type_checker_ensure_multi_return_type(TypeChecker *checker,
                                          FunctionDeclaration *function,
                                          SourceLocation location);

Type *type_checker_pointer_to(TypeChecker *checker, Type *base);

/* The slice type `T[]`: a pointer to T and a length, in one value. It is what
   a `T[N]` becomes at a boundary that does not know N, and what `new T[n]`
   produces. Canonical per element type. */
Type *type_checker_slice_of(TypeChecker *checker, Type *element);
Type *type_checker_view_of(TypeChecker *checker, Type *element, size_t rank);
Type *type_checker_volatile_of(TypeChecker *checker, Type *base);
Type *type_checker_parse_pointer_type(TypeChecker *checker,
                                             const char *name);

int type_checker_types_equal(const Type *lhs, const Type *rhs);

int type_checker_is_cstring_type(const Type *type);
int type_checker_is_rawptr_type(const Type *type);

int type_checker_is_lvalue_expression(ASTNode *expression);

int type_checker_eval_integer_constant_with_checker(TypeChecker *checker,
                                                           ASTNode *expression,
                                                           long long *out_value);

int type_checker_eval_float_constant_with_checker(TypeChecker *checker,
                                                  ASTNode *expression,
                                                  double *out_value);

/* Compile-time memory diagnostics (type_checker_memory.c). Phase 1 runs per
 * function after the body type-checks, while the function scope is still
 * live: use-after-free, double free, dangling stack addresses, constant
 * out-of-bounds indexes, constant-size memory-op overflows. Returns 0 when
 * it reported a hard error. */
int type_checker_check_function_memory(TypeChecker *checker,
                                       ASTNode *declaration);

/* Phase 2 runs once after the whole program type-checks: ownership
 * summaries (which parameters a function frees or keeps, whether it returns
 * a fresh allocation) are inferred to fixpoint over the call graph, then
 * each body is re-analyzed for cross-call use-after-free, cross-call double
 * free, and leaks that survive borrowing helpers. Warnings only. */
int type_checker_check_program_memory(TypeChecker *checker, ASTNode *program);

int type_checker_eval_integer_constant(ASTNode *expression,
                                              long long *out_value);

Type *type_checker_resolve_sizeof_argument(TypeChecker *checker,
                                                  CallExpression *call,
                                                  SourceLocation location);

Type *type_checker_resolve_typeof_argument(TypeChecker *checker,
                                           CallExpression *call,
                                           SourceLocation location);

int type_checker_eval_offsetof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_offset);

/* `fieldof(T, "name")`: the Field `T.name` names, reached through a
 * compile-time string so a metaprogram can compose the name it looks up. */
int type_checker_eval_fieldof(TypeChecker *checker, CallExpression *call,
                              SourceLocation location,
                              ComptimeValue *out_value);

/* `layoutof(T)`: a digest of everything a stored value of T depends on. Pin it
 * with static_assert and a layout change fails the build instead of being
 * reinterpreted under a shape it was not written with. */
int type_checker_eval_layoutof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_digest);

int type_checker_eval_comptime(TypeChecker *checker, ASTNode *expression,
                               ComptimeValue *out_value);

Type *type_checker_type_value(TypeChecker *checker, Type *referred,
                              ASTNode *expression);

Type *type_checker_field_value(TypeChecker *checker, Type *owner,
                               uint32_t field_index, ASTNode *expression);

int type_checker_validate_rule_signature(TypeChecker *checker,
                                         ASTNode *declaration,
                                         FunctionDeclaration *func_decl,
                                         Type **param_types,
                                         Type *return_type);

int type_checker_validate_static_assert(TypeChecker *checker,
                                               CallExpression *call,
                                               SourceLocation location);

void type_checker_buffer_extent_clear(TypeChecker *checker);

void type_checker_buffer_extent_exit_scope(TypeChecker *checker,
                                                  int scope_depth);

TrackedBufferExtent *
type_checker_buffer_extent_find(TypeChecker *checker, const char *name);

int type_checker_buffer_extent_declare(TypeChecker *checker,
                                              const char *name,
                                              long long byte_count,
                                              long long known_alignment);

int type_checker_buffer_extent_set(TypeChecker *checker, const char *name,
                                          long long byte_count,
                                          long long known_alignment);

long long type_checker_default_heap_alignment(void);

long long
type_checker_extract_allocation_call_alignment(CallExpression *call);

long long type_checker_known_alignment_after_offset(long long base_align,
                                                           long long offset);

const char *type_checker_extract_identifier_name(ASTNode *expression);

long long
type_checker_extract_allocation_call_extent(CallExpression *call);

long long type_checker_extract_known_buffer_extent(TypeChecker *checker,
                                                          ASTNode *expression);

long long
type_checker_extract_known_pointer_alignment(TypeChecker *checker,
                                             ASTNode *expression);

void type_checker_warn_pointer_integer_round_trip(TypeChecker *checker,
                                                 ASTNode *expression,
                                                 CastExpression *cast_expr,
                                                 Type *target_type);
void type_checker_warn_potential_misaligned_cast(TypeChecker *checker,
                                                        ASTNode *expression,
                                                        CastExpression *cast_expr,
                                                        Type *target_type);

void type_checker_warn_recv_buffer_bounds(TypeChecker *checker,
                                                 CallExpression *call);

void type_checker_warn_memcpy_buffer_bounds(TypeChecker *checker,
                                                   CallExpression *call);

int type_checker_ast_contains_node_type(ASTNode *node,
                                               ASTNodeType target_type);

int type_checker_is_null_pointer_constant(ASTNode *expression);

/* Targets that accept a null pointer constant. Function pointers are pointers
 * too: `var f: fn(int32) -> int32 = 0;` is the same idea as a null data
 * pointer, and is how a table of entry points is declared before it is
 * populated at run time. */
int type_checker_type_accepts_null_pointer(const Type *type);

void type_checker_init_tracker_reset(TypeChecker *checker);

int type_checker_init_tracker_ensure_var_capacity(TypeChecker *checker);

int
type_checker_init_tracker_ensure_scope_capacity(TypeChecker *checker);

int type_checker_init_tracker_enter_scope(TypeChecker *checker);

void type_checker_init_tracker_exit_scope(TypeChecker *checker);

int type_checker_init_tracker_declare(TypeChecker *checker,
                                             const char *name,
                                             int initialized);

long long type_checker_init_tracker_find(TypeChecker *checker,
                                                const char *name);

int type_checker_init_tracker_is_initialized(TypeChecker *checker,
                                                    const char *name,
                                                    int *known);

void type_checker_init_tracker_set_initialized(TypeChecker *checker,
                                                      const char *name);

unsigned char *type_checker_init_tracker_capture(TypeChecker *checker,
                                                        size_t *count);

void type_checker_init_tracker_restore(TypeChecker *checker,
                                              const unsigned char *snapshot,
                                              size_t count);

/* Merge a branch's end state into `accumulator`: a variable survives the join
   only where every path initialized it. Used to close an if/else chain, so a
   variable written on all paths is initialized after it. */
void type_checker_init_tracker_join(unsigned char *accumulator,
                                           const unsigned char *branch,
                                           size_t count);

int type_checker_statement_guarantees_termination(ASTNode *statement);

const char *type_checker_decl_link_name(const char *name, int is_extern,
                                               const char *link_name);

const char *type_checker_symbol_link_name(const Symbol *symbol);

int type_checker_link_name_matches_symbol(const Symbol *symbol,
                                                 const char *decl_name,
                                                 int decl_is_extern,
                                                 const char *decl_link_name);

/* Record whether the last parameter was written `T[..]`, so a call gathers
   what follows the fixed parameters into it. Idempotent. */
void type_checker_note_gathered_parameter(FunctionDeclaration *declaration);

int type_checker_register_function_signature(TypeChecker *checker,
                                                    ASTNode *declaration);

Type *type_checker_method_receiver_struct_type(Type *receiver_type);

int type_checker_desugar_struct_method_call(TypeChecker *checker,
                                                   ASTNode *expression,
                                                   CallExpression *call);

Type *type_checker_default_integer_literal_type(TypeChecker *checker,
                                                     NumberLiteral *literal);

int type_checker_is_int64_min_magnitude(const ASTNode *operand);

Type *type_checker_infer_type_internal(TypeChecker *checker,
                                              ASTNode *expression);

/* Shared target-neutral tensor builtin helpers. The epilogue checker lives in
 * its own translation unit to keep complete CodeView debug information in
 * normal MinGW builds. */
const char *type_checker_tensor_option_identifier(ASTNode *node);
int type_checker_tensor_option_u32(TypeChecker *checker, ASTNode *node,
                                   const char *name, uint32_t maximum,
                                   uint32_t *out_value);
MtlcTensorElement type_checker_tensor_element_name(const char *name);
MtlcTensorLayout type_checker_tensor_layout_name(const char *name);
int type_checker_tensor_pointer_matches(Type *type,
                                        MtlcTensorElement element);
Type *type_checker_tensor_epilogue_builtin(TypeChecker *checker,
                                           ASTNode *expression,
                                           CallExpression *call,
                                           int *handled);

Type *type_checker_parse_function_pointer_type(TypeChecker *checker,
                                                      const char *name);

Type *type_checker_closure_env_sentinel(void);

Type *type_checker_build_tagged_enum_type(TypeChecker *checker,
                                                  const char *type_name,
                                                  EnumDeclaration *enum_decl);

int type_checker_process_tagged_enum(TypeChecker *checker,
                                            ASTNode *enum_decl_node);

Type *type_checker_instantiate_generic_enum(TypeChecker *checker,
                                                    const char *generic_name,
                                                    const char *type_arg_str);

int type_checker_check_match_statement(TypeChecker *checker,
                                               ASTNode *statement);

Type *type_checker_check_match_expression(TypeChecker *checker,
                                                 ASTNode *expression);

int type_checker_check_if_statement(TypeChecker *checker,
                                           ASTNode *statement);

int type_checker_check_for_statement(TypeChecker *checker,
                                            ASTNode *statement);

int type_checker_check_switch_statement(TypeChecker *checker,
                                               ASTNode *statement);

/* Check and fold an aggregate literal against the type it initializes
 * (type_checker_aggregate.c). On success the literal's folded byte image and
 * relocations are attached to the node and `target` is returned. Elements that
 * are not compile-time constants are recorded as runtime stores for lowering
 * to emit after the image; `requires_constant` refuses those, for a `const` or
 * a module-scope `var`, where no code runs to perform them. */
Type *type_checker_check_aggregate_literal(TypeChecker *checker,
                                           ASTNode *expression, Type *target,
                                           int requires_constant);

#endif // TYPE_CHECKER_INTERNAL_H
