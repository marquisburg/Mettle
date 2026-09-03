#ifndef AST_H
#define AST_H

#include "simd_attr.h"
#include "source_location.h"
#include "mtlc/memory.h"
#include "mtlc/tensor.h"
#include <stddef.h>

/* A parsed identifier has no binding until semantic analysis resolves it.
 * Scope ids are stable for the lifetime of one symbol table and let later
 * passes distinguish two bindings that share a source name. */
typedef size_t ASTScopeId;
#define AST_SCOPE_ID_UNRESOLVED ((ASTScopeId)-1)

typedef enum {
  AST_PROGRAM,
  AST_IMPORT,
  AST_IMPORT_STR,
  AST_VAR_DECLARATION,
  AST_FUNCTION_DECLARATION,
  AST_STRUCT_DECLARATION,
  AST_ENUM_DECLARATION,
  AST_TYPE_DECLARATION,
  AST_EFFECT_DECLARATION,
  AST_TRAIT_DECLARATION,
  AST_IMPL_DECLARATION,
  AST_METHOD_DECLARATION,
  AST_ASSIGNMENT,
  AST_FUNCTION_CALL,
  AST_FUNC_PTR_CALL,
  AST_GPU_LAUNCH,
  AST_RETURN_STATEMENT,
  AST_IF_STATEMENT,
  AST_WHILE_STATEMENT,
  AST_FOR_STATEMENT,
  AST_SWITCH_STATEMENT,
  AST_CASE_CLAUSE,
  AST_MATCH_STATEMENT,
  AST_BREAK_STATEMENT,
  AST_CONTINUE_STATEMENT,
  /* `quiesce;` : a point the programmer names as safe to swap code at. Carries
   * no data; the location is the whole content. */
  AST_QUIESCE_STATEMENT,
  AST_DEFER_STATEMENT,
  AST_ERRDEFER_STATEMENT,
  AST_INLINE_ASM,
  AST_IDENTIFIER,
  AST_NUMBER_LITERAL,
  AST_STRING_LITERAL,
  AST_BINARY_EXPRESSION,
  AST_UNARY_EXPRESSION,
  AST_MEMBER_ACCESS,
  AST_INDEX_EXPRESSION,
  AST_NEW_EXPRESSION,
  AST_CAST_EXPRESSION,
  AST_LAMBDA_EXPRESSION,
  AST_CLOSURE_ADAPT_EXPRESSION,
  AST_BARRIER_STATEMENT,
  AST_AGGREGATE_LITERAL,
  /* `comptime for f in typeof(T).fields { ... }`. Replaced by its expansions
   * during const eval, so no pass after the expander ever sees one. */
  AST_COMPTIME_FOR,
  /* `fallthrough;` inside a switch case: continue into the next case's body.
     A case ends at the next one without it. */
  AST_FALLTHROUGH_STATEMENT,
  /* Not a node kind: the number of them, so a table can be indexed by one. */
  AST_NODE_TYPE_COUNT
} ASTNodeType;

/* SourceLocation moved to ../source_location.h so the backend IR can share it
 * without depending on this AST header. */

typedef struct ASTNode {
  ASTNodeType type;
  SourceLocation location;
  struct ASTNode **children;
  size_t child_count;
  void *data;                 // Node-specific data
  struct Type *resolved_type; // Cached type from semantic analysis
  struct Type *proven_refinement;
} ASTNode;

typedef struct {
  char *module_name;
  char *namespace_alias;
  char **selected_names;   // non-NULL when import { a, b } from "mod"
  size_t selected_count;
  char *platform_guard;    // "windows"/"linux" for `import ... if <platform>`;
                           // NULL means the import is unconditional
} ImportDeclaration;

typedef struct {
  char *file_path;
} ImportStrExpression;

/* Source-level storage intent. These names are frontend semantics: lowering
 * maps them to backend-neutral IR address spaces, and no target dialect enters
 * the AST. */
typedef enum {
  AST_ADDRESS_SPACE_DEFAULT = 0,
  AST_ADDRESS_SPACE_WORKGROUP,
  AST_ADDRESS_SPACE_PRIVATE
} AstAddressSpace;

typedef struct {
  char *name;
  char *type_name;
  ASTNode *initializer;
  int is_extern;
  int is_exported;
  int is_const; // declared with `const`: immutable binding
  char *link_name;
  // Set on compiler-synthesized bindings whose type is determined structurally
  // (e.g. a range-`for` loop counter takes the type of its bound), which are
  // exempt from the "explicit type required on var/const" rule. User-written
  // `var`/`const` declarations always leave this 0.
  int structural_type;
  AstAddressSpace address_space;
  /* `ident("prefix", f.name)` in the name position: the parts to join, held
   * until the expander resolves them and writes the answer into `name`. NULL
   * on every declaration whose name was spelled out. */
  ASTNode *composed_name;
} VarDeclaration;

typedef struct {
  char *name;
  char **parameter_names;
  char **parameter_types;
  size_t parameter_count;
  char *return_type;
  char **return_types;
  size_t return_type_count;
  ASTNode *body;
  int is_exported;
  int is_extern;
  int is_kernel;          // `kernel`: GPU entry point (not an ordinary function)
  // `kernel(block = N)` / `kernel(block = (x, y, z))`: the launch block shape
  // this kernel requires. All zero when undeclared.
  int kernel_block[3];
  // `kernel(block = N, per = warp)`: how many threads one work item costs,
  // which is what `dispatch k[work: n]` divides by. 0 or 1 is one thread per
  // item; 32 is one subgroup per item, the shape of a warp-per-row matvec.
  int kernel_threads_per_item;
  char *link_name;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
  // Function decorators (`@inline[!]` / `@noinline` / `@pure` / `@noalloc` /
  // `@simd[!]`):
  int is_inline;          // `@inline`  : force past the inliner's heuristics
  int is_inline_contract; // `@inline!` : every call inlines or compile error
  int is_noinline;        // `@noinline`: never inline this function
  int is_pure;            // `@pure`    : side-effect-free; enables call LICM
  int is_noalloc;         // `@noalloc` : proven allocation-free or compile error
  int is_test;            // `@test`    : compile-time unit test; compiled out
                          //              of normal builds, run by `mettle test`
  // `@swappable`: this function may be replaced in a running process at a
  // `quiesce` point. Opting in is what buys the call binding that makes a
  // swap possible, so a function without it pays nothing and can be proven
  // to have paid nothing.
  int is_swappable;
  int is_naked;
  int is_interrupt;
  int is_rule;
  int rewrite_role;
  char **effects_with;
  size_t effects_with_count;
  char **effects_forbids;
  size_t effects_forbids_count;
  char **effects_requires;
  size_t effects_requires_count;
  char **effects_provides;
  size_t effects_provides_count;
  /* The last parameter was written `T[..]`, so a call gathers whatever follows
   * the fixed parameters into it. Inside the body it is an ordinary `T[]`. */
  int is_variadic;
  int simd_mode;          // SimdAttr applied as the default to every body loop
  // Closure conversion metadata (set on AST_LAMBDA_EXPRESSION nodes only). A
  // capturing lambda records the variables it captures by value, their types,
  // and the synthesized environment struct; `name` then holds the constructor
  // function the lambda value is produced by.
  char **captured_names;
  char **captured_types;
  size_t captured_count;
  char *env_struct_name;
  /* See VarDeclaration::composed_name. */
  ASTNode *composed_name;
} FunctionDeclaration;

// A thin function value (`&func`, or a non-capturing lambda) implicitly wrapped
// to satisfy an `Fn(...)->R` closure-typed boundary (parameter, return, or var
// declaration). Synthesized by the closure-adapt pass; `ctor_name` is the
// generated adapter constructor to call, `inner` is the original thin
// expression, and `param_types`/`return_type` are the wrapped signature (used
// by the type checker to build the resulting closure type).
typedef struct {
  ASTNode *inner;
  char *ctor_name;
  char **param_types;
  size_t param_count;
  char *return_type;
} ClosureAdapt;

typedef struct {
  char *name;
  char **field_names;
  char **field_types;
  size_t field_count;
  ASTNode **methods;
  size_t method_count;
  int is_exported;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
  /* See VarDeclaration::composed_name. */
  ASTNode *composed_name;
} StructDeclaration;

typedef struct {
  char *name;
  ASTNode *value;       // Initializer expression (for plain integer enums)
  char *payload_type;   // Associated data type name, e.g. "T" or "int32"
                        // NULL means this variant carries no payload
} EnumVariant;

typedef struct {
  char *name;
  char *base_type;
  /* The name the predicate binds the value under. NULL means `value`, the
   * default, which is what almost every declaration wants. */
  char *binding;
  ASTNode *predicate;
  int is_exported;
  ASTNode *composed_name;
} TypeDeclaration;

typedef struct {
  char *name;
  int is_exported;
} EffectDeclaration;

typedef struct {
  char *name;
  EnumVariant *variants;
  size_t variant_count;
  int is_exported;
  // Generic type parameters e.g. enum Option<T> { Some(T), None }
  char **type_params;
  size_t type_param_count;
} EnumDeclaration;

// Match arm: case Some(v): body  or  case None: body
typedef struct {
  char *variant_name;   // "Some", "None", "Ok", "Err"
  char *binding_name;   // variable bound to payload, NULL if no binding/payload
  ASTNode *body;
  int is_default;       // 1 for a wildcard default arm
} MatchArm;

typedef struct {
  ASTNode *expression;  // Value being matched
  MatchArm *arms;
  size_t arm_count;
  int is_expression;    // 1 if used in expression position (arm bodies are
                        // value-yielding expressions, exhaustiveness required)
} MatchStatement;

typedef struct {
  char *name;
  int is_exported;
  ASTNode **methods;
  size_t method_count;
} TraitDeclaration;

typedef struct {
  char *trait_name;
  char *for_type_name;
  ASTNode **methods;
  size_t method_count;
} ImplDeclaration;

typedef struct {
  char *assembly_code;
} InlineAsm;

typedef struct {
  ASTNode **declarations;
  size_t declaration_count;
} Program;

typedef struct {
  char *function_name;
  ASTNode **arguments;
  /* Optional names parallel to arguments. The reference grammar accepts these
   * for compiler-native tensor and atomic operations. */
  char **argument_names;
  size_t argument_count;
  ASTNode *object; // Non-null for method calls (obj.method(args))
  char **type_args;
  size_t type_arg_count;
  /* Monomorphization overwrites function_name with the mangled instance name
   * and drops type_args, so diagnostics have nothing left to quote. This keeps
   * the callee as written. */
  char *written_name;
  int is_indirect_call; // 1 if callee is a variable with function pointer type
  struct Type *callee_closure_env; // non-NULL if the callee is a capturing
                                   // closure; set by the type checker
  const char *effect_signature;
  int is_gpu_index; /* parser-recognized thread/block/dimension member access */
  int is_gpu_atomic;
  MtlcAddressSpace atomic_address_space;
  MtlcMemoryOrder atomic_memory_order;
  MtlcMemoryOrder atomic_failure_order;
  MtlcMemoryScope atomic_memory_scope;
  int is_gpu_async_copy;
  uint32_t async_copy_element_count;
  uint32_t async_copy_transaction_bytes;
  uint32_t async_copy_pending_groups;
  MtlcAsyncCache async_copy_cache;
  int is_tensor_transfer;
  MtlcTensorTransferDesc tensor_transfer_desc;
  size_t tensor_transfer_view_argument;
  size_t tensor_transfer_coordinate_arguments[MTLC_TENSOR_MAX_RANK];
  int is_tensor_mma;
  /* Whole-matrix bounded region operation. It reuses the neutral tensor
   * descriptor but is distinct from one exact tile in shared IR. */
  int is_tensor_matmul;
  MtlcTensorMmaDesc tensor_mma_desc;
  size_t tensor_metadata_argument;
  size_t tensor_a_scale_argument;
  size_t tensor_b_scale_argument;
  size_t tensor_a_stride_argument;
  size_t tensor_b_stride_argument;
  size_t tensor_c_stride_argument;
  size_t tensor_d_stride_argument;
  int is_tensor_epilogue;
  MtlcTensorEpilogueDesc tensor_epilogue_desc;
  size_t tensor_epilogue_bias_argument;
  size_t tensor_epilogue_alpha_argument;
  size_t tensor_epilogue_beta_argument;
  size_t tensor_epilogue_clamp_min_argument;
  size_t tensor_epilogue_clamp_max_argument;
  size_t tensor_epilogue_stride_argument;
  size_t tensor_epilogue_bias_stride_argument;
} CallExpression;

typedef struct {
  ASTNode *function;
  ASTNode **arguments;
  size_t argument_count;
  const char *effect_signature;
} FuncPtrCall;

/* Semantic GPU launch statement. Compact source launches synthesize the unused
 * dimensions/shared/stream defaults; named source launches can populate the
 * complete provider-neutral contract. `kernel` is a runtime launch handle,
 * not a source function declaration. */
typedef struct {
  ASTNode *kernel;
  ASTNode *grid[3];
  ASTNode *block[3];
  ASTNode *dynamic_shared_bytes;
  ASTNode *stream;
  ASTNode **arguments;
  size_t argument_count;
  /* Set by the type checker when `kernel` named a declared `extern kernel`:
   * the arguments were checked against its signature, and the launch handle is
   * resolved from that name at lowering instead of read from a host variable. */
  int typed_kernel;
  /* `work: N` -- launch enough blocks to cover N work items at the kernel's
   * declared block shape. grid[] is synthesized from it. */
  ASTNode *work;
  /* The declared kernel's `kernel(block = ...)`, copied by the type checker so
   * lowering can size a `work:` grid. All zero when undeclared. */
  int kernel_block[3];
  /* Threads one work item costs, from the declaration's `per`. */
  int kernel_threads_per_item;
} GpuLaunchStatement;

typedef enum {
  AST_MEMORY_REGION_WORKGROUP = 1u << 0,
  AST_MEMORY_REGION_GLOBAL = 1u << 1
} AstMemoryRegion;

typedef enum {
  AST_MEMORY_ORDER_ACQUIRE = 1,
  AST_MEMORY_ORDER_RELEASE,
  AST_MEMORY_ORDER_ACQ_REL,
  AST_MEMORY_ORDER_SEQ_CST
} AstMemoryOrder;

typedef struct {
  unsigned memory_regions;
  AstMemoryOrder memory_order;
} BarrierStatement;

typedef struct {
  char *variable_name;
  ASTNode *value;
  ASTNode *target; // Non-null for struct field assignment (obj.field = expr)
  ASTNode **targets;
  size_t target_count;
} Assignment;

typedef struct {
  char *name;
  ASTScopeId scope_id;
} Identifier;

typedef struct {
  union {
    long long int_value;
    double float_value;
  };
  int is_float;
  /* Written `'a'`. The lexer folds a character literal to its code point and
   * hands back a number, so this is what tells the two apart afterwards: it
   * is what types the literal `char` rather than an integer. */
  int is_char;
  /* TOKEN_NUMBER source radix for default integer type (2, 10, 16); 10 for
   * synthesized literals. */
  unsigned char int_radix;
} NumberLiteral;

typedef struct {
  char *value;
  /* Byte length, which is not strlen: `\0` is a legal escape, so a literal may
   * carry an interior NUL. `value` still ends in a NUL so the passes that only
   * want a name (an import path, a diagnostic) can keep reading it as a C
   * string; the ones that build the {chars, length} record read this. */
  size_t length;
} StringLiteral;

typedef struct {
  char *type_name; // The target struct or type name
  /* `new T[n]`: how many elements to allocate. The value is a slice, `T[]`,
   * so the count travels with the pointer. NULL for `new T`, which allocates
   * one and yields `T*`. */
  ASTNode *count;
  ASTNode **extents;
  size_t extent_count;
} NewExpression;

typedef struct {
  char *type_name;  // Target type string
  ASTNode *operand; // Expression being cast
} CastExpression;

typedef struct {
  ASTNode *left;
  ASTNode *right;
  char *operator;
} BinaryExpression;

typedef struct {
  ASTNode *operand;
  char *operator;
} UnaryExpression;

typedef struct {
  ASTNode *object;
  char *member;
} MemberAccess;

typedef struct {
  ASTNode *array;
  ASTNode *index;
} ArrayIndexExpression;

/* One link-time address inside a folded aggregate image. A pointer, function
 * pointer, or string element has no value until the linker places what it
 * refers to, so the image leaves a pointer-sized hole and records what fills
 * it. Exactly one of `symbol` and `string` is set. */
typedef struct {
  size_t offset;  // byte offset into the image
  char *symbol;   // module symbol whose address goes here (`&f`, `&g`)
  char *string;   // string-literal bytes to emit and point at
  size_t string_length;
  /* A `string` value is a pointer to a { chars, length } record, so the slot
   * points at a record the backend builds; a `cstring` points straight at the
   * characters. Only meaningful when `string` is set. */
  int string_wants_record;
} AggregateReloc;

/* One element of an aggregate literal that is not a compile-time constant. The
 * image holds zero at its offset and lowering stores the value there after
 * copying the image in, so a literal may mix the two freely: what is known
 * while compiling stays in the image, and what is not is computed at the point
 * the literal is written. */
typedef struct {
  size_t offset;            // byte offset into the image
  ASTNode *element;         // borrowed; the node is a child of the literal
  struct Type *element_type; // what to store, and how wide
} AggregateRuntimeStore;

/* An aggregate literal: `[a, b, c]` or `[value; count]` for an array, and
 * `{ field: value, ... }` for a struct. The literal has no type of its own -
 * it takes the type of whatever it initializes, which is always spelled out in
 * Mettle (every `var` and `const` states its type). Elements are also children
 * of the node, so the node's destructor frees them; only the arrays here are
 * owned by this struct. */
typedef struct {
  int is_struct;      // 1: `{ field: value }` form; 0: `[ element ]` form
  ASTNode **elements; // borrowed; the nodes are children
  char **field_names; // struct form only: one name per element
  size_t element_count;
  /* Array repeat form `[value; count]`: `elements[0]` is the repeated value and
   * this is the count expression. NULL for the comma-separated form. */
  ASTNode *repeat_count;
  /* The folded value, filled in by the type checker. Aggregate literals are
   * compile-time constants, so the whole thing collapses to a byte image plus
   * the relocations that finish it at link time. Lowering copies these onto the
   * IR module symbol; codegen blits them into the object file. Only the
   * outermost literal of a nested group carries an image. */
  unsigned char *image;
  size_t image_size;
  AggregateReloc *relocs;
  size_t reloc_count;
  /* The elements that are not constants, in the order they were written. */
  AggregateRuntimeStore *runtime_stores;
  size_t runtime_store_count;
} AggregateLiteral;

typedef struct {
  ASTNode *condition;
  ASTNode *body;
} ElseIfClause;

typedef struct {
  ASTNode *condition;
  ASTNode *then_branch;
  ElseIfClause *else_ifs;
  size_t else_if_count;
  ASTNode *else_branch;
} IfStatement;

// SIMD vectorization attribute on a loop (`@simd` / `@simd!`).
/* SimdAttr moved to ../simd_attr.h so the backend IR/optimizer can share it
 * without depending on this AST header. */

typedef struct {
  ASTNode *condition;
  ASTNode *body;
  char *label; // Optional label for labeled break/continue; NULL if unlabeled
  int simd_mode; // SimdAttr: vectorization attribute requested on this loop
  int unroll_factor; // `@unroll(n)` requested on this loop; 0 if absent
} WhileStatement;

typedef struct {
  ASTNode *initializer;
  ASTNode *condition;
  ASTNode *increment;
  ASTNode *body;
  char *label; // Optional label
  int simd_mode; // SimdAttr: vectorization attribute requested on this loop
  int unroll_factor; // `@unroll(n)` requested on this loop; 0 if absent
} ForStatement;

/* `comptime for <binding> in <sequence> { <body> }`.
 *
 * The sequence is a compile-time expression, not a runtime one: today the only
 * form is `<type-expression>.fields`. The expander evaluates it, clones the
 * body once per element, and splices the clones into the enclosing block. */
typedef struct {
  char *binding_name;
  ASTNode *sequence;
  ASTNode *body; // AST_PROGRAM block
  /* Span of the `comptime for` keyword itself, so an expansion note points at
   * the line the programmer wrote rather than at generated code. */
  SourceLocation keyword_location;
} ComptimeForStatement;

typedef struct {
  ASTNode *value;
  ASTNode *value_high; // non-NULL for a range case `lo..hi`; `value` holds lo
  ASTNode *body;
  int is_default;
} CaseClause;

typedef struct {
  ASTNode *expression;
  ASTNode **cases;
  size_t case_count;
} SwitchStatement;

typedef struct {
  ASTNode *value;
  ASTNode **values;
  size_t value_count;
} ReturnStatement;

typedef struct {
  char *target_label; // Optional label name; NULL for unlabeled break/continue
} LoopControlStatement;

typedef struct {
  ASTNode *statement;
} DeferStatement;

// Function declarations
ASTNode *ast_create_node(ASTNodeType type, SourceLocation location);
ASTNode *ast_clone_node(ASTNode *node);
void ast_destroy_node(ASTNode *node);
void ast_add_child(ASTNode *parent, ASTNode *child);

// Specific node creation functions
ASTNode *ast_create_program();
ASTNode *ast_create_import_declaration(const char *module_name,
                                       const char *namespace_alias,
                                       const char **selected_names,
                                       size_t selected_count,
                                       SourceLocation location);
ASTNode *ast_create_import_str(const char *file_path, SourceLocation location);
ASTNode *ast_create_var_declaration(const char *name, const char *type_name,
                                    ASTNode *initializer,
                                    SourceLocation location);
ASTNode *ast_create_function_declaration(const char *name, char **param_names,
                                         char **param_types, size_t param_count,
                                         const char *return_type, ASTNode *body,
                                         SourceLocation location);
ASTNode *ast_create_struct_declaration(const char *name, char **field_names,
                                       char **field_types, size_t field_count,
                                       ASTNode **methods, size_t method_count,
                                       SourceLocation location);
ASTNode *ast_create_type_declaration(const char *name, const char *base_type,
                                     const char *binding, ASTNode *predicate,
                                     SourceLocation location);
ASTNode *ast_create_enum_declaration(const char *name, EnumVariant *variants,
                                     size_t variant_count,
                                     SourceLocation location);
ASTNode *ast_create_trait_declaration(const char *name,
                                      SourceLocation location);
ASTNode *ast_create_effect_declaration(const char *name,
                                       SourceLocation location);
int ast_function_set_effects(FunctionDeclaration *decl, int clause,
                             char **names, size_t count);
enum {
  AST_EFFECT_CLAUSE_WITH = 0,
  AST_EFFECT_CLAUSE_FORBIDS = 1,
  AST_EFFECT_CLAUSE_REQUIRES = 2,
  AST_EFFECT_CLAUSE_PROVIDES = 3
};
ASTNode *ast_create_impl_declaration(const char *trait_name,
                                     const char *for_type_name,
                                     SourceLocation location);
ASTNode *ast_create_call_expression(const char *function_name,
                                    ASTNode **arguments, size_t argument_count,
                                    SourceLocation location);
ASTNode *ast_create_func_ptr_call(ASTNode *function, ASTNode **arguments,
                                  size_t argument_count,
                                  SourceLocation location);
ASTNode *ast_create_gpu_launch(ASTNode *kernel, ASTNode **grid,
                               ASTNode **block,
                               ASTNode *dynamic_shared_bytes, ASTNode *stream,
                               ASTNode **arguments, size_t argument_count,
                               SourceLocation location);
ASTNode *ast_create_barrier_statement(unsigned memory_regions,
                                      AstMemoryOrder memory_order,
                                      SourceLocation location);
ASTNode *ast_create_assignment(const char *variable_name, ASTNode *value,
                               SourceLocation location);
ASTNode *ast_create_multi_assignment(ASTNode **targets, size_t target_count,
                                     ASTNode *value, SourceLocation location);
ASTNode *ast_create_inline_asm(const char *assembly_code,
                               SourceLocation location);
ASTNode *ast_create_identifier(const char *name, SourceLocation location);
ASTNode *ast_create_identifier_with_scope(const char *name,
                                          ASTScopeId scope_id,
                                          SourceLocation location);
ASTNode *ast_create_number_literal(long long int_value,
                                   SourceLocation location,
                                   unsigned char int_radix);
ASTNode *ast_create_float_literal(double float_value, SourceLocation location);
ASTNode *ast_create_string_literal(const char *value, size_t length,
                                  SourceLocation location);
ASTNode *ast_create_binary_expression(ASTNode *left, const char *op,
                                      ASTNode *right, SourceLocation location);
ASTNode *ast_create_unary_expression(const char *op, ASTNode *operand,
                                     SourceLocation location);
ASTNode *ast_create_member_access(ASTNode *object, const char *member,
                                  SourceLocation location);
ASTNode *ast_create_array_index_expression(ASTNode *array, ASTNode *index,
                                           SourceLocation location);
/* Takes ownership of `elements` and `field_names` (and of the name strings);
 * `field_names` is NULL for the array form. Returns NULL on allocation
 * failure, in which case the caller still owns its arrays. */
ASTNode *ast_create_aggregate_literal(int is_struct, ASTNode **elements,
                                      char **field_names, size_t element_count,
                                      ASTNode *repeat_count,
                                      SourceLocation location);
ASTNode *ast_create_method_call(ASTNode *object, const char *method_name,
                                ASTNode **arguments, size_t argument_count,
                                SourceLocation location);
ASTNode *ast_create_new_expression(const char *type_name,
                                   SourceLocation location);
/* Drop a node's claim on its children without freeing them, for a synthesized
   node that borrows expressions another node owns. */
void ast_release_children(ASTNode *node);

/* `new T[count]`: a heap array whose length travels with it, as `T[]`. */
int ast_new_expression_add_extent(ASTNode *node, ASTNode *extent);
ASTNode *ast_create_new_array_expression(const char *type_name, ASTNode *count,
                                         SourceLocation location);
ASTNode *ast_create_field_assignment(ASTNode *target, ASTNode *value,
                                     SourceLocation location);
ASTNode *ast_create_cast_expression(const char *type_name, ASTNode *operand,
                                    SourceLocation location);
ASTNode *ast_create_closure_adapt(ASTNode *inner, const char *ctor_name,
                                  char **param_types, size_t param_count,
                                  const char *return_type,
                                  SourceLocation location);
ASTNode *ast_create_for_statement(ASTNode *initializer, ASTNode *condition,
                                  ASTNode *increment, ASTNode *body,
                                  SourceLocation location);
/* Takes ownership of `sequence` and `body`; copies `binding_name`. */
ASTNode *ast_create_comptime_for(const char *binding_name, ASTNode *sequence,
                                 ASTNode *body, SourceLocation location);
/* Replace a member access with the integer const eval folded it to. */
int ast_fold_member_access_to_int(ASTNode *node, long long value);
/* Same, for a query that folded to a string (`.name`). */
int ast_fold_member_access_to_string(ASTNode *node, const char *value);
/* Same, for a float column of a compile-time table. */
int ast_fold_member_access_to_float(ASTNode *node, double value);
/* Replace an `ident(...)` call node with the identifier it composed. */
int ast_fold_call_to_identifier(ASTNode *node, const char *name);
ASTNode *ast_create_case_clause(ASTNode *value, ASTNode *body, int is_default,
                                SourceLocation location);
ASTNode *ast_create_switch_statement(ASTNode *expression, ASTNode **cases,
                                     size_t case_count,
                                     SourceLocation location);
ASTNode *ast_create_quiesce_statement(SourceLocation location);
ASTNode *ast_create_fallthrough_statement(SourceLocation location);
ASTNode *ast_create_break_statement(SourceLocation location);
ASTNode *ast_create_continue_statement(SourceLocation location);
ASTNode *ast_create_labeled_break_statement(const char *label,
                                            SourceLocation location);
ASTNode *ast_create_labeled_continue_statement(const char *label,
                                               SourceLocation location);
ASTNode *ast_create_defer_statement(ASTNode *statement,
                                    SourceLocation location);
ASTNode *ast_create_errdefer_statement(ASTNode *statement,
                                       SourceLocation location);
ASTNode *ast_create_match_statement(ASTNode *expression, MatchArm *arms,
                                    size_t arm_count, SourceLocation location);
ASTNode *ast_create_match_expression(ASTNode *expression, MatchArm *arms,
                                     size_t arm_count, SourceLocation location);

#endif // AST_H
