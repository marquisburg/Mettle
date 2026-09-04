#ifndef IR_LOWERING_INTERNAL_H
#define IR_LOWERING_INTERNAL_H

// Shared internals for the AST->IR lowering pass, split across ir_lower*.c
// modules. The public entry point ir_lower_program lives in ir_lowering.h (the
// frontend-facing lowering header); this header exposes the cross-module
// lowering context, helper structs, and static-helper prototypes.

#include "ir_lowering.h" // ir.h + frontend AST/type headers + lowering entry points
#include "../common.h"
#include "compiler/compiler_context.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct IRDeferScope IRDeferScope;

typedef struct {
  char *break_label;
  char *continue_label;
  char *user_label; // optional source-level label for labeled break/continue
  /* Where `fallthrough;` goes: the next case's label, while that case's body
     is being lowered. Borrowed from the switch's label array, and NULL
     everywhere else, including in the last case. */
  const char *fallthrough_label;
  /* The defer chain in effect where this loop or switch was entered. A
     `break` or `continue` targeting this frame leaves every scope between the
     jump and here, so those scopes' deferred statements run before it. */
  IRDeferScope *defers;
} IRControlFrame;

/* One local's binding in the function being lowered. A local is addressed by
 * name all the way down to the backends, which key their slot, float, string
 * and declared-type tables on that name; two same-named locals in different
 * scopes would therefore share one frame slot and one type. A redeclaration at
 * a different type gets a distinct IR name here, and uses resolve to the
 * innermost binding still in scope. Same-typed redeclarations keep the source
 * name and share the slot, exactly as before. */
typedef struct {
  const char *name;      /* source name, borrowed from the AST */
  const char *ir_name;   /* what the IR calls it: `name`, or an owned rename */
  const char *type_text; /* declared type name at this binding, may be NULL */
  int depth;             /* scope depth this binding was declared at */
  int active;            /* cleared once its scope has been left */
  int owns_ir_name;      /* ir_name was allocated here and must be freed */
} IRLocalBinding;

typedef struct {
  int next_temp_id;
  int next_label_id;
  IRControlFrame *control_stack;
  size_t control_count;
  size_t control_capacity;
  char *error_message;
  TypeChecker *type_checker;
  SymbolTable *symbol_table;
  /* The expansion whose body is being lowered, stamped onto every instruction
   * emitted while it is in effect. Saved and restored around each expanded
   * block, so a nested expansion reports the innermost one. */
  const char *current_expansion_note;
  int emit_runtime_checks;
  /* `--safe`: emit an IR_OP_SAFETY_CHECK at every memory access. Independent
   * of emit_runtime_checks, which is the debug-build null and bounds trap and
   * is off under --release. Safety checks are emitted at every optimization
   * level, because the proving happens in ir_safety_resolve_program() rather
   * than by dropping the checks. */
  int emit_safety_checks;
  int emit_refinement_checks;
  int emit_task_checks;
  int emitted_task_check;
  /* While a relational predicate is being lowered as a run-time check, the
     binding it speaks about stands for this operand. */
  const char *refine_binding_name;
  IROperand refine_binding_value;
  int refine_binding_active;
  /* Declared return type name of the function currently being lowered. Used
   * to give a width-less float literal in `return <lit>;` the correct
   * single/double precision (literals always infer to float64 otherwise). */
  const char *current_return_type_name;
  const char *current_function_name;
  /* Monotonic id handed to each `@simd` loop's begin/end marker pair so the
   * release-stage contract verifier can match them. */
  int next_simd_request_id;
  /* Default SimdAttr from a function-level `@simd` decorator. A counted loop in
   * the body with no `@simd` of its own inherits this mode. */
  int current_function_simd_default;
  /* The program being built. Lowering a local aggregate literal parks its
   * folded image here as a hidden module constant and copies from it, so the
   * value is laid out once in the object file rather than stored piecewise. */
  IRProgram *program;
  /* Locals of the function being lowered, oldest first; reset per function. */
  IRLocalBinding *local_bindings;
  size_t local_binding_count;
  size_t local_binding_capacity;
  int local_scope_depth;
  int local_rename_serial;
  /* The statement list the current statement belongs to, and its position in
   * it. An aggregate declared without an initializer reads ahead through these
   * to find out whether its zero-fill is dead before it is emitted. */
  ASTNode **block_statements;
  size_t block_statement_count;
  size_t block_statement_index;
} IRLoweringContext;

typedef struct {
  struct {
    ASTNode *node;
    int is_err;
    /* By-value capture for `defer fn(args...)`: when capture_call_name is
     * non-NULL, the argument values were snapshotted into the named temp
     * locals at the defer point, and the deferred call is replayed against
     * those temps instead of re-evaluating the original argument expressions
     * (which would observe their later, scope-exit values). */
    char *capture_call_name;
    char **capture_arg_temps;
    size_t capture_arg_count;
  } *entries;
  size_t count;
  size_t capacity;
} IRDeferStack;

struct IRDeferScope {
  IRDeferStack stack;
  struct IRDeferScope *parent;
};

extern int g_ir_lowering_explain;

int ir_emit_jump_instruction(IRLoweringContext *context,
                                    IRFunction *function, const char *label,
                                    SourceLocation location);

int ir_emit_label_instruction(IRLoweringContext *context,
                                     IRFunction *function, const char *label,
                                     SourceLocation location);

int ir_emit_simd_marker(IRLoweringContext *context, IRFunction *function,
                               char which, int id, int mode,
                               SourceLocation location);

int ir_emit_unroll_marker(IRLoweringContext *context, IRFunction *function,
                          int factor, SourceLocation location);

int ir_type_is_cstring(Type *type);
int ir_type_is_rawptr(Type *type);

int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression);

int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression);

int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location);

/* An array handed to a slice becomes `{ &a[0], N }` in a hidden local, so the
   extent the type carried travels with the value. */
int ir_should_build_slice_from_array(Type *target_type,
                                     ASTNode *value_expression);
int ir_build_slice_operand_from_array(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      Type *array_type, Type *slice_type,
                                      SourceLocation location);

int ir_should_decay_array_to_address(Type *target_type,
                                     ASTNode *value_expression);

int ir_decay_array_operand_to_address(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      SourceLocation location);

int ir_lower_statement_or_expression(IRLoweringContext *context,
                                            IRFunction *function,
                                            ASTNode *node);

void ir_local_scope_enter(IRLoweringContext *context);

void ir_local_scope_leave(IRLoweringContext *context);

void ir_local_bindings_reset(IRLoweringContext *context);

const char *ir_local_bind(IRLoweringContext *context, const char *name,
                          const char *type_text);

const char *ir_local_bind_parameter(IRLoweringContext *context,
                                    const char *name, const char *type_text);

const char *ir_local_ir_name(IRLoweringContext *context, const char *name);

const IRLocalBinding *ir_local_binding_find(IRLoweringContext *context,
                                            const char *name);

int ir_emit_local_declaration(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name, const char *type_name,
                                     SourceLocation location);

IROperand ir_clone_operand_local(const IROperand *operand);

int ir_try_emit_aggregate_symbol_memcpy(
    IRLoweringContext *context, IRFunction *function, const char *dest_name,
    const IROperand *value, Type *dest_type, SourceLocation location);

/* Copy a folded aggregate literal into a target. The literal is interned as a
 * hidden module constant the first time it is lowered, so the copy is a plain
 * block move from the object file's own data. */
int ir_emit_aggregate_literal_copy(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IROperand *dest_address,
                                   ASTNode *literal_node, Type *dest_type,
                                   SourceLocation location);

int ir_emit_aggregate_literal_copy_to_symbol(IRLoweringContext *context,
                                             IRFunction *function,
                                             const char *dest_name,
                                             ASTNode *literal_node,
                                             Type *dest_type,
                                             SourceLocation location);

int ir_emit_zero_fill_local(IRLoweringContext *context, IRFunction *function,
                            const char *local_name, Type *type,
                            SourceLocation location);

int ir_try_emit_aggregate_address_memcpy(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *dest_addr,
                                         const IROperand *value, Type *dest_type,
                                         SourceLocation location);

Type *ir_resolve_named_type(IRLoweringContext *context,
                                   const char *name);

Type *ir_lookup_symbol_type(IRLoweringContext *context,
                                   const char *name);

int ir_emit_symbol_assignment(IRLoweringContext *context,
                                     IRFunction *function,
                                     const char *name,
                                     const IROperand *value,
                                     SourceLocation location);

int ir_emit_address_with_offset(IRLoweringContext *context,
                                       IRFunction *function,
                                       const IROperand *base_address,
                                       size_t offset,
                                       SourceLocation location,
                                       IROperand *out_address);

int ir_emit_switch_range_dispatch(IRLoweringContext *context,
                                         IRFunction *function,
                                         const IROperand *switch_value,
                                         ASTNode *lo_node, ASTNode *hi_node,
                                         const char *case_label,
                                         SourceLocation loc);

int ir_lower_switch_statement(IRLoweringContext *context,
                                     IRFunction *function, ASTNode *statement,
                                     IRDeferScope *defers);

int ir_lower_match_statement(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *statement,
                                    IRDeferScope *defers);

int ir_lower_match_expression(IRLoweringContext *context,
                                     IRFunction *function,
                                     ASTNode *expression,
                                     IROperand *out_value);

int ir_emit_tagged_enum_construct(IRLoweringContext *context,
                                         IRFunction *function,
                                         Symbol *constructor_symbol,
                                         ASTNode *payload_arg,
                                         SourceLocation location,
                                         IROperand *out_value);

int ir_lower_tagged_enum_constructor_call(IRLoweringContext *context,
                                                 IRFunction *function,
                                                 ASTNode *expression,
                                                 Symbol *constructor_symbol,
                                                 IROperand *out_value);

int ir_emit_deferred_calls(IRLoweringContext *context,
                                  IRFunction *function,
                                  const IRDeferStack *stack);

int ir_emit_deferred_calls_non_err(IRLoweringContext *context,
                                          IRFunction *function,
                                          const IRDeferStack *stack);

int ir_emit_deferred_scopes(IRLoweringContext *context,
                                   IRFunction *function,
                                   const IRDeferScope *scope);

int ir_emit_deferred_scopes_non_err(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferScope *scope);

int ir_emit_return_with_defers(IRLoweringContext *context,
                                      IRFunction *function,
                                      IRDeferScope *defers, IROperand *value,
                                      SourceLocation location);

void ir_set_error(IRLoweringContext *context, const char *format, ...);

char *ir_new_temp_name(IRLoweringContext *context);

char *ir_new_label_name(IRLoweringContext *context, const char *prefix);

int ir_emit(IRLoweringContext *context, IRFunction *function,
                   const IRInstruction *instruction);

extern int g_ir_lowering_refinement_checks;
extern int g_ir_lowering_task_checks;
int ir_emit_refinement_predicate(IRLoweringContext *context,
                                 IRFunction *function, SourceLocation location,
                                 const IROperand *value, const Type *refined,
                                 struct ASTNode *predicate,
                                 const char *binding);
int ir_emit_refinement_check(IRLoweringContext *context, IRFunction *function,
                             SourceLocation location, const IROperand *value,
                             const Type *refined);
int ir_emit_runtime_trap_ex(IRLoweringContext *context,
                                   IRFunction *function,
                                   SourceLocation location, uint32_t kind,
                                   const char *message, const IROperand *arg0,
                                   const IROperand *arg1);

int ir_emit_null_check(IRLoweringContext *context, IRFunction *function,
                              SourceLocation location, const IROperand *value);

/* Bounds-check an index against the length a slice carries. */
int ir_small_float_local(IRLoweringContext *context, IRFunction *function,
                         const char *source_name, const char *ir_name,
                         Type *type);
int ir_emit_small_float_home_store(IRLoweringContext *context,
                                   IRFunction *function, const char *ir_name,
                                   Type *type, const IROperand *value,
                                   SourceLocation location);
int ir_emit_small_float_home_load(IRLoweringContext *context,
                                  IRFunction *function, const char *ir_name,
                                  Type *type, SourceLocation location,
                                  IROperand *out_value);
int ir_emit_load_word(IRLoweringContext *context, IRFunction *function,
                      const IROperand *base_address, size_t offset,
                      SourceLocation location, IROperand *out_value);
int ir_emit_store_word(IRLoweringContext *context, IRFunction *function,
                       const IROperand *base_address, size_t offset,
                       const IROperand *value, SourceLocation location);
int ir_emit_binary_temp(IRLoweringContext *context, IRFunction *function,
                        const char *operator_text, const IROperand *lhs,
                        const IROperand *rhs, SourceLocation location,
                        IROperand *out_value);
int ir_emit_slice_bounds_check(IRLoweringContext *context, IRFunction *function,
                               SourceLocation location,
                               const IROperand *slice_address,
                               const IROperand *index);

int ir_emit_bounds_check(IRLoweringContext *context,
                                IRFunction *function, SourceLocation location,
                                const IROperand *index, size_t array_size);

/* `--safe`: record one access for ir_safety_resolve_program() to prove or
 * check. `base` carries the provenance and `offset` the signed byte
 * displacement from it; `extent` is the object's size in bytes when that is a
 * compile time constant and IR_SAFETY_EXTENT_UNKNOWN when it is not. `what` is
 * a short source-level spelling used in the failure message. A no-op when
 * --safe is off. */
int ir_emit_safety_check(IRLoweringContext *context, IRFunction *function,
                         SourceLocation location, const IROperand *base,
                         const IROperand *offset, long long access_size,
                         long long extent, int access_kind, const char *what);

int ir_push_labeled_control_frame(IRLoweringContext *context,
                                         const char *break_label,
                                         const char *continue_label,
                                         const char *user_label,
                                         IRDeferScope *defers);

int ir_push_control_frame(IRLoweringContext *context,
                                 const char *break_label,
                                 const char *continue_label,
                                 IRDeferScope *defers);

void ir_pop_control_frame(IRLoweringContext *context);

const char *ir_current_break_label(IRLoweringContext *context);

/* The innermost switch case that has a case after it, or NULL. `fallthrough`
   jumps to its label, and its defer chain says which scopes to leave first. */
const IRControlFrame *ir_current_fallthrough_frame(IRLoweringContext *context);

/* Point the innermost control frame at the case a `fallthrough` would enter.
   The switch sets it before each case body and clears it after the last. */
void ir_set_fallthrough_label(IRLoweringContext *context, const char *label);

const char *ir_current_continue_label(IRLoweringContext *context);

const char *ir_find_labeled_break(IRLoweringContext *context,
                                         const char *user_label);

const char *ir_find_labeled_continue(IRLoweringContext *context,
                                            const char *user_label);

/* The frame a `break` / `continue` written here would jump to. `user_label`
   is NULL for the bare forms. Returns NULL when there is no such frame; the
   caller reports that as the error. */
const IRControlFrame *ir_break_target_frame(IRLoweringContext *context,
                                            const char *user_label);

const IRControlFrame *ir_continue_target_frame(IRLoweringContext *context,
                                               const char *user_label);

/* Run the deferred statements of every scope from `from` up to `stop`,
   innermost first, leaving `stop` itself alone. `errdefer` entries are
   skipped: they belong to the function's return, and a jump is not one. */
int ir_emit_defers_until_scope(IRLoweringContext *context,
                               IRFunction *function, const IRDeferScope *from,
                               const IRDeferScope *stop);

int ir_defer_stack_push(IRLoweringContext *context, IRDeferStack *stack,
                               ASTNode *node, int is_err);

void ir_defer_stack_free(IRDeferStack *stack);

int ir_defer_capture_call(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *defer_node,
                                 char **out_call_name, char ***out_temps,
                                 size_t *out_count);

int ir_emit_deferred_calls_filtered(IRLoweringContext *context,
                                           IRFunction *function,
                                           const IRDeferStack *stack,
                                           int include_err);

int ir_lower_deferred_statement(IRLoweringContext *context,
                                       IRFunction *function,
                                       ASTNode *statement);

int ir_expression_is_floating(IRLoweringContext *context,
                                     ASTNode *expression);

int ir_type_is_float64(Type *type);

int ir_type_float_bits(Type *type);

int ir_named_type_float_bits(IRLoweringContext *context,
                                    const char *type_name);

void ir_operand_apply_float_bits(IROperand *operand, int bits);

int ir_symbol_float_bits(IRLoweringContext *context, const char *name);

int ir_local_declared_float_bits(IRLoweringContext *context,
                                        const IRFunction *function,
                                        const char *name);

void ir_assign_apply_float_bits(IRInstruction *instruction,
                                       IROperand *value, int bits);

void ir_access_apply_alias_class(IRInstruction *access, Type *accessed_type);
void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type);

void ir_load_apply_unsigned(IRInstruction *load, Type *loaded_type);

int ir_expression_float_bits(IRLoweringContext *context,
                                    ASTNode *expression);

int ir_binary_operator_is_comparison(const char *op);

int ir_binary_expression_operation_float_bits(IRLoweringContext *context,
                                                    ASTNode *expression,
                                                    BinaryExpression *binary);

int ir_type_storage_size(Type *type);

int ir_type_array_element_stride(Type *element_type);

int ir_type_is_unsigned_integer(Type *type);
const char *ir_narrow_integer_result_type(Type *type, const char *op);
int ir_narrow_integer_shift_bits(Type *type);
int ir_unary_constant_fits(const char *type_name, const char *op,
                           long long value);
int ir_type_is_pointer(Type *type);

int ir_emit_binary_instruction(IRLoweringContext *context,
                                      IRFunction *function,
                                      SourceLocation location, const char *op,
                                      IROperand dest, IROperand lhs,
                                      IROperand rhs);

int ir_emit_scaled_index_offset(IRLoweringContext *context,
                                       IRFunction *function,
                                       SourceLocation location,
                                       const IROperand *index, int stride,
                                       IROperand *out_offset);

int ir_try_lower_pointer_arithmetic(IRLoweringContext *context,
                                           IRFunction *function,
                                           BinaryExpression *binary,
                                           SourceLocation location,
                                           IROperand *out_value);

int ir_make_temp_operand(IRLoweringContext *context,
                                IROperand *out_temp);

int ir_emit_condition_false_branch(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *expression,
                                          const char *false_label);

int ir_emit_condition_true_branch(IRLoweringContext *context,
                                         IRFunction *function,
                                         ASTNode *expression,
                                         const char *true_label);

int ir_lower_call_expression(IRLoweringContext *context,
                                    IRFunction *function, ASTNode *expression,
                                    IROperand *out_value);

Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression);

int ir_emit_address_of_symbol(IRLoweringContext *context,
                                     IRFunction *function, const char *name,
                                     SourceLocation location,
                                     IROperand *out_address);

int ir_lower_lvalue_address(IRLoweringContext *context,
                                   IRFunction *function, ASTNode *expression,
                                   IROperand *out_address, Type **out_type);

int ir_lower_expression(IRLoweringContext *context, IRFunction *function,
                               ASTNode *expression, IROperand *out_value);

int ir_lower_statement_with_defers(IRLoweringContext *context,
                                          IRFunction *function,
                                          ASTNode *statement,
                                          IRDeferScope *defers);

IRFunction *ir_lower_function(IRLoweringContext *context,
                                     ASTNode *declaration);

#endif // IR_LOWERING_INTERNAL_H
