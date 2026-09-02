#include "ast.h"
#include "string_intern.h"
#include <stdlib.h>
#include <string.h>

static char *ast_copy_string(const char *value) {
  return value ? strdup(value) : NULL;
}

static char *ast_copy_bytes(const char *value, size_t length) {
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

static char *ast_intern_string(const char *value) {
  return value ? (char *)string_intern(value) : NULL;
}

static void ast_free_string(char *value) {
  if (!value) {
    return;
  }
  if (!string_is_interned(value)) {
    free(value);
  }
}

static char **ast_copy_string_array(char **values, size_t count) {
  if (!values || count == 0) {
    return NULL;
  }

  char **copy = malloc(count * sizeof(char *));
  if (!copy) {
    return NULL;
  }

  for (size_t i = 0; i < count; i++) {
    copy[i] = ast_intern_string(values[i]);
  }

  return copy;
}

ASTNode *ast_create_node(ASTNodeType type, SourceLocation location) {
  ASTNode *node = malloc(sizeof(ASTNode));
  if (!node)
    return NULL;

  node->type = type;
  node->location = location;
  node->children = NULL;
  node->child_count = 0;
  node->data = NULL;
  node->resolved_type = NULL;

  return node;
}

/* Cloning, one function per node kind.
 *
 * These were a single switch with a case per kind, 950 lines of it, past
 * what a reader can hold and past what the complexity gate allows. A handler
 * fills `clone` from `node` and hands back the node to use: normally `clone`
 * itself, but a kind whose constructor builds the node outright returns that
 * instead, having released `clone`. NULL means the clone failed, and the
 * handler has released `clone` by then - it owns it either way.
 *
 * A kind with no payload to copy has no handler: the table holds NULL there
 * and the bare node stands, which is what the switch's default did. */

static ASTNode *ast_clone_program(ASTNode *clone, const ASTNode *node) {
  Program *src = (Program *)node->data;
  Program *dst = malloc(sizeof(Program));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->declaration_count = src ? src->declaration_count : 0;
  dst->declarations = NULL;
  if (dst->declaration_count > 0) {
    dst->declarations = malloc(dst->declaration_count * sizeof(ASTNode *));
    for (size_t i = 0; i < dst->declaration_count; i++) {
      dst->declarations[i] = ast_clone_node(src->declarations[i]);
      ast_add_child(clone, dst->declarations[i]);
    }
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_var_declaration(ASTNode *clone, const ASTNode *node) {
  VarDeclaration *src = (VarDeclaration *)node->data;
  VarDeclaration *dst = malloc(sizeof(VarDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->name = ast_intern_string(src->name);
  dst->type_name = ast_intern_string(src->type_name);
  dst->is_extern = src->is_extern;
  dst->is_exported = src->is_exported;
  dst->is_const = src->is_const;
  dst->structural_type = src->structural_type;
  dst->address_space = src->address_space;
  dst->link_name = ast_copy_string(src->link_name);
  dst->initializer =
      src->initializer ? ast_clone_node(src->initializer) : NULL;
  if (dst->initializer)
    ast_add_child(clone, dst->initializer);
  /* Not a child: a composed name is compile-time material that the expander
   * consumes, and every pass that walks children runs after it is gone. */
  dst->composed_name =
      src->composed_name ? ast_clone_node(src->composed_name) : NULL;
  clone->data = dst;
  return clone;
}

/* A method carries a FunctionDeclaration like the other two. It used to fall
* to the default case and clone to a node with NULL data, which every later
* pass then skipped as malformed -- that is how a monomorphized generic
* struct lost its methods. */
static ASTNode *ast_clone_method_declaration(ASTNode *clone, const ASTNode *node) {
  FunctionDeclaration *src = (FunctionDeclaration *)node->data;
  FunctionDeclaration *dst = malloc(sizeof(FunctionDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->name = ast_intern_string(src->name);
  dst->return_type = ast_intern_string(src->return_type);
  dst->return_type_count = src->return_type_count;
  dst->return_types = ast_copy_string_array(src->return_types,
                                             src->return_type_count);
  dst->parameter_count = src->parameter_count;
  dst->is_exported = src->is_exported;
  dst->is_extern = src->is_extern;
  dst->is_kernel = src->is_kernel;
  dst->kernel_block[0] = src->kernel_block[0];
  dst->kernel_block[1] = src->kernel_block[1];
  dst->kernel_block[2] = src->kernel_block[2];
  dst->kernel_threads_per_item = src->kernel_threads_per_item;
  dst->link_name = ast_copy_string(src->link_name);
  dst->type_param_count = src->type_param_count;
  dst->type_params = ast_copy_string_array(src->type_params, src->type_param_count);
  dst->type_param_traits =
      ast_copy_string_array(src->type_param_traits, src->type_param_count);
  /* Decorator flags. These were never copied (the struct is malloc'd, so
   * clones -- notably monomorphized generics -- carried UNINITIALIZED
   * decorator flags). Mostly latent until `@inline!`/`@noalloc` made a
   * garbage flag a hard compile error. */
  dst->is_inline = src->is_inline;
  dst->is_inline_contract = src->is_inline_contract;
  dst->is_noinline = src->is_noinline;
  dst->is_pure = src->is_pure;
  dst->is_noalloc = src->is_noalloc;
  dst->is_test = src->is_test;
  dst->is_swappable = src->is_swappable;
  dst->is_naked = src->is_naked;
  dst->is_interrupt = src->is_interrupt;
  dst->rewrite_role = src->rewrite_role;
  dst->is_variadic = src->is_variadic;
  dst->simd_mode = src->simd_mode;
  dst->captured_count = src->captured_count;
  dst->captured_names =
      ast_copy_string_array(src->captured_names, src->captured_count);
  dst->captured_types =
      ast_copy_string_array(src->captured_types, src->captured_count);
  dst->env_struct_name = ast_intern_string(src->env_struct_name);
  if (src->parameter_count > 0) {
    dst->parameter_names = malloc(src->parameter_count * sizeof(char *));
    dst->parameter_types = malloc(src->parameter_count * sizeof(char *));
    for (size_t i = 0; i < src->parameter_count; i++) {
      dst->parameter_names[i] = ast_intern_string(src->parameter_names[i]);
      dst->parameter_types[i] = ast_intern_string(src->parameter_types[i]);
    }
  } else {
    dst->parameter_names = NULL;
    dst->parameter_types = NULL;
  }
  dst->body = src->body ? ast_clone_node(src->body) : NULL;
  if (dst->body)
    ast_add_child(clone, dst->body);
  dst->composed_name =
      src->composed_name ? ast_clone_node(src->composed_name) : NULL;
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_struct_declaration(ASTNode *clone, const ASTNode *node) {
  StructDeclaration *src = (StructDeclaration *)node->data;
  StructDeclaration *dst = malloc(sizeof(StructDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->name = ast_intern_string(src->name);
  dst->field_count = src->field_count;
  dst->method_count = src->method_count;
  dst->is_exported = src->is_exported;
  dst->type_param_count = src->type_param_count;
  dst->type_params = ast_copy_string_array(src->type_params, src->type_param_count);
  dst->type_param_traits =
      ast_copy_string_array(src->type_param_traits, src->type_param_count);
  if (src->field_count > 0) {
    dst->field_names = malloc(src->field_count * sizeof(char *));
    dst->field_types = malloc(src->field_count * sizeof(char *));
    for (size_t i = 0; i < src->field_count; i++) {
      dst->field_names[i] = ast_intern_string(src->field_names[i]);
      dst->field_types[i] = ast_intern_string(src->field_types[i]);
    }
  } else {
    dst->field_names = NULL;
    dst->field_types = NULL;
  }
  if (src->method_count > 0) {
    dst->methods = malloc(src->method_count * sizeof(ASTNode *));
    for (size_t i = 0; i < src->method_count; i++) {
      dst->methods[i] = ast_clone_node(src->methods[i]);
      if (dst->methods[i])
        ast_add_child(clone, dst->methods[i]);
    }
  } else {
    dst->methods = NULL;
  }
  dst->composed_name =
      src->composed_name ? ast_clone_node(src->composed_name) : NULL;
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_function_call(ASTNode *clone, const ASTNode *node) {
  CallExpression *src = (CallExpression *)node->data;
  CallExpression *dst = malloc(sizeof(CallExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->function_name = ast_intern_string(src->function_name);
  dst->argument_count = src->argument_count;
  dst->is_gpu_index = src->is_gpu_index;
  dst->is_gpu_atomic = src->is_gpu_atomic;
  dst->atomic_address_space = src->atomic_address_space;
  dst->atomic_memory_order = src->atomic_memory_order;
  dst->atomic_failure_order = src->atomic_failure_order;
  dst->atomic_memory_scope = src->atomic_memory_scope;
  dst->is_gpu_async_copy = src->is_gpu_async_copy;
  dst->async_copy_element_count = src->async_copy_element_count;
  dst->async_copy_transaction_bytes = src->async_copy_transaction_bytes;
  dst->async_copy_pending_groups = src->async_copy_pending_groups;
  dst->async_copy_cache = src->async_copy_cache;
  dst->is_tensor_transfer = src->is_tensor_transfer;
  dst->tensor_transfer_desc = src->tensor_transfer_desc;
  dst->tensor_transfer_view_argument =
      src->tensor_transfer_view_argument;
  memcpy(dst->tensor_transfer_coordinate_arguments,
         src->tensor_transfer_coordinate_arguments,
         sizeof(dst->tensor_transfer_coordinate_arguments));
  dst->is_tensor_mma = src->is_tensor_mma;
  dst->is_tensor_matmul = src->is_tensor_matmul;
  dst->tensor_mma_desc = src->tensor_mma_desc;
  dst->tensor_metadata_argument = src->tensor_metadata_argument;
  dst->tensor_a_scale_argument = src->tensor_a_scale_argument;
  dst->tensor_b_scale_argument = src->tensor_b_scale_argument;
  dst->tensor_a_stride_argument = src->tensor_a_stride_argument;
  dst->tensor_b_stride_argument = src->tensor_b_stride_argument;
  dst->tensor_c_stride_argument = src->tensor_c_stride_argument;
  dst->tensor_d_stride_argument = src->tensor_d_stride_argument;
  dst->is_tensor_epilogue = src->is_tensor_epilogue;
  dst->tensor_epilogue_desc = src->tensor_epilogue_desc;
  dst->tensor_epilogue_bias_argument =
      src->tensor_epilogue_bias_argument;
  dst->tensor_epilogue_alpha_argument =
      src->tensor_epilogue_alpha_argument;
  dst->tensor_epilogue_beta_argument = src->tensor_epilogue_beta_argument;
  dst->tensor_epilogue_clamp_min_argument =
      src->tensor_epilogue_clamp_min_argument;
  dst->tensor_epilogue_clamp_max_argument =
      src->tensor_epilogue_clamp_max_argument;
  dst->tensor_epilogue_stride_argument =
      src->tensor_epilogue_stride_argument;
  dst->tensor_epilogue_bias_stride_argument =
      src->tensor_epilogue_bias_stride_argument;
  dst->type_arg_count = src->type_arg_count;
  dst->written_name = ast_intern_string(src->written_name);
  dst->is_indirect_call = src->is_indirect_call;
  dst->callee_closure_env = src->callee_closure_env;
  dst->object = src->object ? ast_clone_node(src->object) : NULL;
  if (dst->object)
    ast_add_child(clone, dst->object);
  if (src->argument_count > 0) {
    dst->arguments = malloc(src->argument_count * sizeof(ASTNode *));
    dst->argument_names = calloc(src->argument_count, sizeof(char *));
    for (size_t i = 0; i < src->argument_count; i++) {
      dst->arguments[i] = ast_clone_node(src->arguments[i]);
      dst->argument_names[i] =
          src->argument_names && src->argument_names[i]
              ? ast_intern_string(src->argument_names[i])
              : NULL;
      if (dst->arguments[i])
        ast_add_child(clone, dst->arguments[i]);
    }
  } else {
    dst->arguments = NULL;
    dst->argument_names = NULL;
  }
  if (src->type_arg_count > 0 && src->type_args) {
    dst->type_args = malloc(src->type_arg_count * sizeof(char *));
    for (size_t i = 0; i < src->type_arg_count; i++) {
      dst->type_args[i] = ast_intern_string(src->type_args[i]);
    }
  } else {
    dst->type_args = NULL;
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_trait_declaration(ASTNode *clone, const ASTNode *node) {
  TraitDeclaration *src = (TraitDeclaration *)node->data;
  TraitDeclaration *dst = malloc(sizeof(TraitDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->name = ast_intern_string(src->name);
  dst->is_exported = src->is_exported;
  dst->method_count = src->method_count;
  if (src->method_count > 0) {
    dst->methods = malloc(src->method_count * sizeof(ASTNode *));
    if (!dst->methods) {
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < src->method_count; i++) {
      dst->methods[i] = ast_clone_node(src->methods[i]);
      if (dst->methods[i])
        ast_add_child(clone, dst->methods[i]);
    }
  } else {
    dst->methods = NULL;
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_impl_declaration(ASTNode *clone, const ASTNode *node) {
  ImplDeclaration *src = (ImplDeclaration *)node->data;
  ImplDeclaration *dst = malloc(sizeof(ImplDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->trait_name = ast_intern_string(src->trait_name);
  dst->for_type_name = ast_intern_string(src->for_type_name);
  dst->method_count = src->method_count;
  if (src->method_count > 0) {
    dst->methods = malloc(src->method_count * sizeof(ASTNode *));
    if (!dst->methods) {
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < src->method_count; i++) {
      dst->methods[i] = ast_clone_node(src->methods[i]);
      if (dst->methods[i])
        ast_add_child(clone, dst->methods[i]);
    }
  } else {
    dst->methods = NULL;
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_func_ptr_call(ASTNode *clone, const ASTNode *node) {
  FuncPtrCall *src = (FuncPtrCall *)node->data;
  FuncPtrCall *dst = malloc(sizeof(FuncPtrCall));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->function = src->function ? ast_clone_node(src->function) : NULL;
  if (dst->function)
    ast_add_child(clone, dst->function);
  dst->argument_count = src->argument_count;
  if (src->argument_count > 0) {
    dst->arguments = malloc(src->argument_count * sizeof(ASTNode *));
    for (size_t i = 0; i < src->argument_count; i++) {
      dst->arguments[i] = ast_clone_node(src->arguments[i]);
      if (dst->arguments[i])
        ast_add_child(clone, dst->arguments[i]);
    }
  } else {
    dst->arguments = NULL;
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_gpu_launch(ASTNode *clone, const ASTNode *node) {
  GpuLaunchStatement *src = (GpuLaunchStatement *)node->data;
  ASTNode *grid[3] = {NULL, NULL, NULL};
  ASTNode *block[3] = {NULL, NULL, NULL};
  ASTNode **args = NULL;
  if (!src) {
    free(clone);
    return NULL;
  }
  ASTNode *kernel = ast_clone_node(src->kernel);
  ASTNode *shared = ast_clone_node(src->dynamic_shared_bytes);
  ASTNode *stream = ast_clone_node(src->stream);
  for (size_t i = 0; i < 3; i++) {
    grid[i] = ast_clone_node(src->grid[i]);
    block[i] = ast_clone_node(src->block[i]);
  }
  if (src->argument_count > 0) {
    args = malloc(src->argument_count * sizeof(*args));
    if (!args) {
      ast_destroy_node(kernel);
      ast_destroy_node(shared);
      ast_destroy_node(stream);
      for (size_t i = 0; i < 3; i++) {
        ast_destroy_node(grid[i]);
        ast_destroy_node(block[i]);
      }
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < src->argument_count; i++) {
      args[i] = ast_clone_node(src->arguments[i]);
    }
  }
  ASTNode *built = ast_create_gpu_launch(
      kernel, grid, block, shared, stream, args, src->argument_count,
      node->location);
  if (built && built->data) {
    GpuLaunchStatement *dst = (GpuLaunchStatement *)built->data;
    dst->typed_kernel = src->typed_kernel;
    dst->kernel_block[0] = src->kernel_block[0];
    dst->kernel_block[1] = src->kernel_block[1];
    dst->kernel_block[2] = src->kernel_block[2];
    dst->kernel_threads_per_item = src->kernel_threads_per_item;
    dst->work = ast_clone_node(src->work);
    ast_add_child(built, dst->work);
  }
  free(args);
  free(clone);
  return built;
}

static ASTNode *ast_clone_barrier_statement(ASTNode *clone, const ASTNode *node) {
  BarrierStatement *src = (BarrierStatement *)node->data;
  ASTNode *built = src ? ast_create_barrier_statement(
                             src->memory_regions, src->memory_order,
                             node->location)
                       : NULL;
  free(clone);
  return built;
}

static ASTNode *ast_clone_assignment(ASTNode *clone, const ASTNode *node) {
  Assignment *src = (Assignment *)node->data;
  Assignment *dst = malloc(sizeof(Assignment));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->variable_name = ast_intern_string(src->variable_name);
  dst->value = src->value ? ast_clone_node(src->value) : NULL;
  dst->target = src->target ? ast_clone_node(src->target) : NULL;
  dst->targets = NULL;
  dst->target_count = src->target_count;
  if (src->target_count > 0) {
    dst->targets = calloc(src->target_count, sizeof(ASTNode *));
    if (!dst->targets) {
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < src->target_count; i++) {
      dst->targets[i] = ast_clone_node(src->targets[i]);
      if (!dst->targets[i]) {
        free(dst->targets);
        free(dst);
        free(clone);
        return NULL;
      }
      ast_add_child(clone, dst->targets[i]);
    }
  }
  if (dst->target)
    ast_add_child(clone, dst->target);
  if (dst->value)
    ast_add_child(clone, dst->value);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_identifier(ASTNode *clone, const ASTNode *node) {
  Identifier *src = (Identifier *)node->data;
  Identifier *dst = malloc(sizeof(Identifier));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->name = ast_intern_string(src->name);
  dst->scope_id = src->scope_id;
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_number_literal(ASTNode *clone, const ASTNode *node) {
  NumberLiteral *src = (NumberLiteral *)node->data;
  NumberLiteral *dst = malloc(sizeof(NumberLiteral));
  if (!dst) {
    free(clone);
    return NULL;
  }
  *dst = *src;
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_string_literal(ASTNode *clone, const ASTNode *node) {
  StringLiteral *src = (StringLiteral *)node->data;
  StringLiteral *dst = malloc(sizeof(StringLiteral));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->value = ast_copy_bytes(src->value, src->length);
  dst->length = src->length;
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_binary_expression(ASTNode *clone, const ASTNode *node) {
  BinaryExpression *src = (BinaryExpression *)node->data;
  BinaryExpression *dst = malloc(sizeof(BinaryExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->operator = ast_copy_string(src->operator);
  dst->left = src->left ? ast_clone_node(src->left) : NULL;
  dst->right = src->right ? ast_clone_node(src->right) : NULL;
  if (dst->left)
    ast_add_child(clone, dst->left);
  if (dst->right)
    ast_add_child(clone, dst->right);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_unary_expression(ASTNode *clone, const ASTNode *node) {
  UnaryExpression *src = (UnaryExpression *)node->data;
  UnaryExpression *dst = malloc(sizeof(UnaryExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->operator = ast_copy_string(src->operator);
  dst->operand = src->operand ? ast_clone_node(src->operand) : NULL;
  if (dst->operand)
    ast_add_child(clone, dst->operand);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_member_access(ASTNode *clone, const ASTNode *node) {
  MemberAccess *src = (MemberAccess *)node->data;
  MemberAccess *dst = malloc(sizeof(MemberAccess));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->member = ast_intern_string(src->member);
  dst->object = src->object ? ast_clone_node(src->object) : NULL;
  if (dst->object)
    ast_add_child(clone, dst->object);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_index_expression(ASTNode *clone, const ASTNode *node) {
  ArrayIndexExpression *src = (ArrayIndexExpression *)node->data;
  ArrayIndexExpression *dst = malloc(sizeof(ArrayIndexExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->array = src->array ? ast_clone_node(src->array) : NULL;
  dst->index = src->index ? ast_clone_node(src->index) : NULL;
  if (dst->array)
    ast_add_child(clone, dst->array);
  if (dst->index)
    ast_add_child(clone, dst->index);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_aggregate_literal(ASTNode *clone, const ASTNode *node) {
  AggregateLiteral *src = (AggregateLiteral *)node->data;
  AggregateLiteral *dst = malloc(sizeof(AggregateLiteral));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->is_struct = src->is_struct;
  dst->element_count = src->element_count;
  dst->elements = NULL;
  dst->field_names = ast_copy_string_array(src->field_names,
                                           src->element_count);
  if (src->element_count > 0) {
    dst->elements = malloc(src->element_count * sizeof(ASTNode *));
    if (!dst->elements) {
      free(dst->field_names);
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < src->element_count; i++) {
      dst->elements[i] =
          src->elements[i] ? ast_clone_node(src->elements[i]) : NULL;
      if (dst->elements[i]) {
        ast_add_child(clone, dst->elements[i]);
      }
    }
  }
  dst->repeat_count =
      src->repeat_count ? ast_clone_node(src->repeat_count) : NULL;
  if (dst->repeat_count) {
    ast_add_child(clone, dst->repeat_count);
  }
  /* The folded image is re-derived when the clone is checked; a clone made
   * before checking (monomorphization) has nothing to copy anyway. The runtime
   * elements go with it: they name nodes in the tree that was cloned from, so
   * carrying them over would point the clone's stores at another tree's
   * expressions. */
  dst->image = NULL;
  dst->image_size = 0;
  dst->relocs = NULL;
  dst->reloc_count = 0;
  dst->runtime_stores = NULL;
  dst->runtime_store_count = 0;
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_new_expression(ASTNode *clone, const ASTNode *node) {
  NewExpression *src = (NewExpression *)node->data;
  NewExpression *dst = malloc(sizeof(NewExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->type_name = ast_intern_string(src->type_name);
  dst->count = src->count ? ast_clone_node(src->count) : NULL;
  dst->extents = NULL;
  dst->extent_count = 0;
  if (dst->count) {
    ast_add_child(clone, dst->count);
  }
  clone->data = dst;
  for (size_t i = 0; i < src->extent_count; i++) {
    ASTNode *extent = ast_clone_node(src->extents[i]);
    if (!extent || !ast_new_expression_add_extent(clone, extent)) {
      ast_destroy_node(extent);
      ast_destroy_node(clone);
      return NULL;
    }
  }
  return clone;
}

static ASTNode *ast_clone_cast_expression(ASTNode *clone, const ASTNode *node) {
  CastExpression *src = (CastExpression *)node->data;
  CastExpression *dst = malloc(sizeof(CastExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->type_name = ast_intern_string(src->type_name);
  dst->operand = src->operand ? ast_clone_node(src->operand) : NULL;
  if (dst->operand)
    ast_add_child(clone, dst->operand);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_closure_adapt_expression(ASTNode *clone, const ASTNode *node) {
  ClosureAdapt *src = (ClosureAdapt *)node->data;
  ClosureAdapt *dst = malloc(sizeof(ClosureAdapt));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->ctor_name = ast_intern_string(src->ctor_name);
  dst->return_type = ast_intern_string(src->return_type);
  dst->param_count = src->param_count;
  dst->param_types =
      ast_copy_string_array(src->param_types, src->param_count);
  dst->inner = src->inner ? ast_clone_node(src->inner) : NULL;
  if (dst->inner)
    ast_add_child(clone, dst->inner);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_return_statement(ASTNode *clone, const ASTNode *node) {
  ReturnStatement *src = (ReturnStatement *)node->data;
  if (src) {
    ReturnStatement *dst = malloc(sizeof(ReturnStatement));
    if (!dst) {
      free(clone);
      return NULL;
    }
    dst->value = NULL;
    dst->values = NULL;
    dst->value_count = src->value_count ? src->value_count
                                        : (src->value ? 1 : 0);
    if (dst->value_count > 0) {
      dst->values = calloc(dst->value_count, sizeof(ASTNode *));
      if (!dst->values) {
        free(dst);
        free(clone);
        return NULL;
      }
      for (size_t i = 0; i < dst->value_count; i++) {
        ASTNode *source_value = src->values
                                    ? src->values[i]
                                    : (i == 0 ? src->value : NULL);
        dst->values[i] = source_value ? ast_clone_node(source_value) : NULL;
        if (!dst->values[i]) {
          free(dst->values);
          free(dst);
          free(clone);
          return NULL;
        }
        ast_add_child(clone, dst->values[i]);
      }
      dst->value = dst->values[0];
    }
    clone->data = dst;
  }
  return clone;
}

static ASTNode *ast_clone_if_statement(ASTNode *clone, const ASTNode *node) {
  IfStatement *src = (IfStatement *)node->data;
  IfStatement *dst = malloc(sizeof(IfStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->condition = src->condition ? ast_clone_node(src->condition) : NULL;
  dst->then_branch =
      src->then_branch ? ast_clone_node(src->then_branch) : NULL;
  dst->else_branch =
      src->else_branch ? ast_clone_node(src->else_branch) : NULL;
  dst->else_if_count = src->else_if_count;
  if (src->else_if_count > 0 && src->else_ifs) {
    dst->else_ifs = malloc(src->else_if_count * sizeof(ElseIfClause));
    for (size_t i = 0; i < src->else_if_count; i++) {
      dst->else_ifs[i].condition = ast_clone_node(src->else_ifs[i].condition);
      dst->else_ifs[i].body = ast_clone_node(src->else_ifs[i].body);
    }
  } else {
    dst->else_ifs = NULL;
  }
  if (dst->condition)
    ast_add_child(clone, dst->condition);
  if (dst->then_branch)
    ast_add_child(clone, dst->then_branch);
  for (size_t i = 0; i < dst->else_if_count; i++) {
    if (dst->else_ifs[i].condition)
      ast_add_child(clone, dst->else_ifs[i].condition);
    if (dst->else_ifs[i].body)
      ast_add_child(clone, dst->else_ifs[i].body);
  }
  if (dst->else_branch)
    ast_add_child(clone, dst->else_branch);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_while_statement(ASTNode *clone, const ASTNode *node) {
  WhileStatement *src = (WhileStatement *)node->data;
  WhileStatement *dst = malloc(sizeof(WhileStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->condition = src->condition ? ast_clone_node(src->condition) : NULL;
  dst->body = src->body ? ast_clone_node(src->body) : NULL;
  dst->label = ast_copy_string(src->label);
  dst->simd_mode = src->simd_mode;
  dst->unroll_factor = src->unroll_factor;
  if (dst->condition)
    ast_add_child(clone, dst->condition);
  if (dst->body)
    ast_add_child(clone, dst->body);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_for_statement(ASTNode *clone, const ASTNode *node) {
  ForStatement *src = (ForStatement *)node->data;
  ForStatement *dst = malloc(sizeof(ForStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->initializer =
      src->initializer ? ast_clone_node(src->initializer) : NULL;
  dst->condition = src->condition ? ast_clone_node(src->condition) : NULL;
  dst->increment = src->increment ? ast_clone_node(src->increment) : NULL;
  dst->body = src->body ? ast_clone_node(src->body) : NULL;
  dst->label = ast_copy_string(src->label);
  dst->simd_mode = src->simd_mode;
  dst->unroll_factor = src->unroll_factor;
  if (dst->initializer)
    ast_add_child(clone, dst->initializer);
  if (dst->condition)
    ast_add_child(clone, dst->condition);
  if (dst->increment)
    ast_add_child(clone, dst->increment);
  if (dst->body)
    ast_add_child(clone, dst->body);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_comptime_for(ASTNode *clone, const ASTNode *node) {
  ComptimeForStatement *src = (ComptimeForStatement *)node->data;
  ComptimeForStatement *dst = malloc(sizeof(ComptimeForStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->binding_name = ast_intern_string(src->binding_name);
  dst->sequence = src->sequence ? ast_clone_node(src->sequence) : NULL;
  dst->body = src->body ? ast_clone_node(src->body) : NULL;
  dst->keyword_location = src->keyword_location;
  if (dst->sequence)
    ast_add_child(clone, dst->sequence);
  if (dst->body)
    ast_add_child(clone, dst->body);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_switch_statement(ASTNode *clone, const ASTNode *node) {
  SwitchStatement *src = (SwitchStatement *)node->data;
  SwitchStatement *dst = malloc(sizeof(SwitchStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->expression = src->expression ? ast_clone_node(src->expression) : NULL;
  dst->case_count = src->case_count;
  if (src->case_count > 0 && src->cases) {
    dst->cases = malloc(src->case_count * sizeof(ASTNode *));
    for (size_t i = 0; i < src->case_count; i++) {
      dst->cases[i] = ast_clone_node(src->cases[i]);
      if (dst->cases[i])
        ast_add_child(clone, dst->cases[i]);
    }
  } else {
    dst->cases = NULL;
  }
  if (dst->expression)
    ast_add_child(clone, dst->expression);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_case_clause(ASTNode *clone, const ASTNode *node) {
  CaseClause *src = (CaseClause *)node->data;
  CaseClause *dst = malloc(sizeof(CaseClause));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->value = src->value ? ast_clone_node(src->value) : NULL;
  dst->value_high = src->value_high ? ast_clone_node(src->value_high) : NULL;
  dst->body = src->body ? ast_clone_node(src->body) : NULL;
  dst->is_default = src->is_default;
  if (dst->value)
    ast_add_child(clone, dst->value);
  if (dst->value_high)
    ast_add_child(clone, dst->value_high);
  if (dst->body)
    ast_add_child(clone, dst->body);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_defer_statement(ASTNode *clone, const ASTNode *node) {
  DeferStatement *src = (DeferStatement *)node->data;
  DeferStatement *dst = malloc(sizeof(DeferStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->statement = src->statement ? ast_clone_node(src->statement) : NULL;
  if (dst->statement)
    ast_add_child(clone, dst->statement);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_inline_asm(ASTNode *clone, const ASTNode *node) {
  InlineAsm *src = (InlineAsm *)node->data;
  InlineAsm *dst = malloc(sizeof(InlineAsm));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->assembly_code = ast_copy_string(src->assembly_code);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_import(ASTNode *clone, const ASTNode *node) {
  ImportDeclaration *src = (ImportDeclaration *)node->data;
  ImportDeclaration *dst = malloc(sizeof(ImportDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->module_name = ast_copy_string(src->module_name);
  dst->namespace_alias = ast_copy_string(src->namespace_alias);
  dst->platform_guard = ast_copy_string(src->platform_guard);
  dst->selected_names = NULL;
  dst->selected_count = 0;
  if (src->selected_names && src->selected_count > 0) {
    dst->selected_names = malloc(src->selected_count * sizeof(char *));
    if (dst->selected_names) {
      for (size_t i = 0; i < src->selected_count; i++) {
        dst->selected_names[i] = ast_copy_string(src->selected_names[i]);
      }
      dst->selected_count = src->selected_count;
    }
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_import_str(ASTNode *clone, const ASTNode *node) {
  ImportStrExpression *src = (ImportStrExpression *)node->data;
  ImportStrExpression *dst = malloc(sizeof(ImportStrExpression));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->file_path = ast_copy_string(src->file_path);
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_break_statement(ASTNode *clone, const ASTNode *node) {
  LoopControlStatement *src = (LoopControlStatement *)node->data;
  if (src) {
    LoopControlStatement *dst = malloc(sizeof(LoopControlStatement));
    if (!dst) {
      free(clone);
      return NULL;
    }
    dst->target_label = ast_copy_string(src->target_label);
    clone->data = dst;
  }
  return clone;
}

static ASTNode *ast_clone_enum_declaration(ASTNode *clone, const ASTNode *node) {
  EnumDeclaration *src = (EnumDeclaration *)node->data;
  EnumDeclaration *dst = malloc(sizeof(EnumDeclaration));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->name = ast_intern_string(src ? src->name : NULL);
  dst->is_exported = src ? src->is_exported : 0;
  dst->variant_count = src ? src->variant_count : 0;
  dst->variants = NULL;
  dst->type_param_count = src ? src->type_param_count : 0;
  dst->type_params = NULL;
  if (dst->variant_count > 0) {
    dst->variants = malloc(dst->variant_count * sizeof(EnumVariant));
    if (!dst->variants) {
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < dst->variant_count; i++) {
      dst->variants[i].name = ast_intern_string(src->variants[i].name);
      dst->variants[i].payload_type =
          ast_intern_string(src->variants[i].payload_type);
      dst->variants[i].value = src->variants[i].value
                                   ? ast_clone_node(src->variants[i].value)
                                   : NULL;
      if (dst->variants[i].value) {
        ast_add_child(clone, dst->variants[i].value);
      }
    }
  }
  if (dst->type_param_count > 0 && src->type_params) {
    dst->type_params = malloc(dst->type_param_count * sizeof(char *));
    if (!dst->type_params) {
      free(dst->variants);
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < dst->type_param_count; i++) {
      dst->type_params[i] = ast_intern_string(src->type_params[i]);
    }
  } else {
    dst->type_param_count = 0;
  }
  clone->data = dst;
  return clone;
}

static ASTNode *ast_clone_match_statement(ASTNode *clone, const ASTNode *node) {
  MatchStatement *src = (MatchStatement *)node->data;
  MatchStatement *dst = malloc(sizeof(MatchStatement));
  if (!dst) {
    free(clone);
    return NULL;
  }
  dst->is_expression = src ? src->is_expression : 0;
  dst->arm_count = src ? src->arm_count : 0;
  dst->arms = NULL;
  dst->expression =
      src && src->expression ? ast_clone_node(src->expression) : NULL;
  if (dst->expression) {
    ast_add_child(clone, dst->expression);
  }
  if (dst->arm_count > 0) {
    dst->arms = malloc(dst->arm_count * sizeof(MatchArm));
    if (!dst->arms) {
      free(dst);
      free(clone);
      return NULL;
    }
    for (size_t i = 0; i < dst->arm_count; i++) {
      dst->arms[i].variant_name =
          ast_intern_string(src->arms[i].variant_name);
      dst->arms[i].binding_name =
          ast_intern_string(src->arms[i].binding_name);
      dst->arms[i].is_default = src->arms[i].is_default;
      dst->arms[i].body =
          src->arms[i].body ? ast_clone_node(src->arms[i].body) : NULL;
      if (dst->arms[i].body) {
        ast_add_child(clone, dst->arms[i].body);
      }
    }
  }
  clone->data = dst;
  return clone;
}

typedef ASTNode *(*AstCloneHandler)(ASTNode *clone, const ASTNode *node);

static const AstCloneHandler AST_CLONE_HANDLERS[AST_NODE_TYPE_COUNT] = {
    [AST_PROGRAM] = ast_clone_program,
    [AST_VAR_DECLARATION] = ast_clone_var_declaration,
    [AST_METHOD_DECLARATION] = ast_clone_method_declaration,
    [AST_LAMBDA_EXPRESSION] = ast_clone_method_declaration,
    [AST_FUNCTION_DECLARATION] = ast_clone_method_declaration,
    [AST_STRUCT_DECLARATION] = ast_clone_struct_declaration,
    [AST_FUNCTION_CALL] = ast_clone_function_call,
    [AST_TRAIT_DECLARATION] = ast_clone_trait_declaration,
    [AST_IMPL_DECLARATION] = ast_clone_impl_declaration,
    [AST_FUNC_PTR_CALL] = ast_clone_func_ptr_call,
    [AST_GPU_LAUNCH] = ast_clone_gpu_launch,
    [AST_BARRIER_STATEMENT] = ast_clone_barrier_statement,
    [AST_ASSIGNMENT] = ast_clone_assignment,
    [AST_IDENTIFIER] = ast_clone_identifier,
    [AST_NUMBER_LITERAL] = ast_clone_number_literal,
    [AST_STRING_LITERAL] = ast_clone_string_literal,
    [AST_BINARY_EXPRESSION] = ast_clone_binary_expression,
    [AST_UNARY_EXPRESSION] = ast_clone_unary_expression,
    [AST_MEMBER_ACCESS] = ast_clone_member_access,
    [AST_INDEX_EXPRESSION] = ast_clone_index_expression,
    [AST_AGGREGATE_LITERAL] = ast_clone_aggregate_literal,
    [AST_NEW_EXPRESSION] = ast_clone_new_expression,
    [AST_CAST_EXPRESSION] = ast_clone_cast_expression,
    [AST_CLOSURE_ADAPT_EXPRESSION] = ast_clone_closure_adapt_expression,
    [AST_RETURN_STATEMENT] = ast_clone_return_statement,
    [AST_IF_STATEMENT] = ast_clone_if_statement,
    [AST_WHILE_STATEMENT] = ast_clone_while_statement,
    [AST_FOR_STATEMENT] = ast_clone_for_statement,
    [AST_COMPTIME_FOR] = ast_clone_comptime_for,
    [AST_SWITCH_STATEMENT] = ast_clone_switch_statement,
    [AST_CASE_CLAUSE] = ast_clone_case_clause,
    [AST_DEFER_STATEMENT] = ast_clone_defer_statement,
    [AST_ERRDEFER_STATEMENT] = ast_clone_defer_statement,
    [AST_INLINE_ASM] = ast_clone_inline_asm,
    [AST_IMPORT] = ast_clone_import,
    [AST_IMPORT_STR] = ast_clone_import_str,
    [AST_BREAK_STATEMENT] = ast_clone_break_statement,
    [AST_CONTINUE_STATEMENT] = ast_clone_break_statement,
    [AST_ENUM_DECLARATION] = ast_clone_enum_declaration,
    [AST_MATCH_STATEMENT] = ast_clone_match_statement,
};

ASTNode *ast_clone_node(ASTNode *node) {
  if (!node) {
    return NULL;
  }
  ASTNode *clone = ast_create_node(node->type, node->location);
  if (!clone) {
    return NULL;
  }
  if (node->type < 0 || node->type >= AST_NODE_TYPE_COUNT) {
    return clone;
  }
  AstCloneHandler handler = AST_CLONE_HANDLERS[node->type];
  if (!handler) {
    return clone;
  }
  return handler(clone, node);
}

ASTNode *ast_create_errdefer_statement(ASTNode *statement,
                                       SourceLocation location) {
  if (!statement) {
    return NULL;
  }

  ASTNode *node = ast_create_node(AST_ERRDEFER_STATEMENT, location);
  if (!node) {
    return NULL;
  }

  DeferStatement *defer_statement = malloc(sizeof(DeferStatement));
  if (!defer_statement) {
    free(node);
    return NULL;
  }

  defer_statement->statement = statement;
  node->data = defer_statement;
  ast_add_child(node, statement);

  return node;
}

void ast_destroy_node(ASTNode *node) {
  if (!node)
    return;

  // Free children
  for (size_t i = 0; i < node->child_count; i++) {
    ast_destroy_node(node->children[i]);
  }
  free(node->children);

  // Free node-specific data
  switch (node->type) {
  case AST_PROGRAM: {
    Program *program = (Program *)node->data;
    if (program) {
      free(program->declarations);
      free(program);
    }
    break;
  }
  case AST_IMPORT: {
    ImportDeclaration *import_decl = (ImportDeclaration *)node->data;
    if (import_decl) {
      ast_free_string(import_decl->module_name);
      ast_free_string(import_decl->namespace_alias);
      ast_free_string(import_decl->platform_guard);
      for (size_t i = 0; i < import_decl->selected_count; i++) {
        ast_free_string(import_decl->selected_names[i]);
      }
      free(import_decl->selected_names);
      free(import_decl);
    }
    break;
  }
  case AST_IMPORT_STR: {
    ImportStrExpression *import_str = (ImportStrExpression *)node->data;
    if (import_str) {
      ast_free_string(import_str->file_path);
      free(import_str);
    }
    break;
  }
  case AST_VAR_DECLARATION: {
    VarDeclaration *var_decl = (VarDeclaration *)node->data;
    if (var_decl) {
      ast_free_string(var_decl->name);
      ast_free_string(var_decl->type_name);
      ast_free_string(var_decl->link_name);
      ast_destroy_node(var_decl->composed_name);
      free(var_decl);
    }
    break;
  }
  case AST_LAMBDA_EXPRESSION:
  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *func_decl = (FunctionDeclaration *)node->data;
    if (func_decl) {
      ast_free_string(func_decl->name);
      ast_free_string(func_decl->return_type);
      for (size_t i = 0; i < func_decl->return_type_count; i++) {
        ast_free_string(func_decl->return_types[i]);
      }
      free(func_decl->return_types);
      ast_free_string(func_decl->link_name);
      for (size_t i = 0; i < func_decl->parameter_count; i++) {
        ast_free_string(func_decl->parameter_names[i]);
        ast_free_string(func_decl->parameter_types[i]);
      }
      free(func_decl->parameter_names);
      free(func_decl->parameter_types);
      for (size_t i = 0; i < func_decl->type_param_count; i++) {
        ast_free_string(func_decl->type_params[i]);
        ast_free_string(func_decl->type_param_traits[i]);
      }
      free(func_decl->type_params);
      free(func_decl->type_param_traits);
      for (size_t i = 0; i < func_decl->captured_count; i++) {
        ast_free_string(func_decl->captured_names[i]);
        ast_free_string(func_decl->captured_types[i]);
      }
      free(func_decl->captured_names);
      free(func_decl->captured_types);
      ast_free_string(func_decl->env_struct_name);
      ast_destroy_node(func_decl->composed_name);
      free(func_decl);
    }
    break;
  }
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *struct_decl = (StructDeclaration *)node->data;
    if (struct_decl) {
      ast_free_string(struct_decl->name);
      for (size_t i = 0; i < struct_decl->field_count; i++) {
        ast_free_string(struct_decl->field_names[i]);
        ast_free_string(struct_decl->field_types[i]);
      }
      free(struct_decl->field_names);
      free(struct_decl->field_types);
      free(struct_decl->methods);
      for (size_t i = 0; i < struct_decl->type_param_count; i++) {
        ast_free_string(struct_decl->type_params[i]);
        ast_free_string(struct_decl->type_param_traits[i]);
      }
      free(struct_decl->type_params);
      free(struct_decl->type_param_traits);
      ast_destroy_node(struct_decl->composed_name);
      free(struct_decl);
    }
    break;
  }
  case AST_ENUM_DECLARATION: {
    EnumDeclaration *enum_decl = (EnumDeclaration *)node->data;
    if (enum_decl) {
      ast_free_string(enum_decl->name);
      if (enum_decl->variants) {
        for (size_t i = 0; i < enum_decl->variant_count; i++) {
          ast_free_string(enum_decl->variants[i].name);
          ast_free_string(enum_decl->variants[i].payload_type);
          // the 'value' node is a child of the enum decl node, freed auto
        }
        free(enum_decl->variants);
      }
      for (size_t i = 0; i < enum_decl->type_param_count; i++) {
        ast_free_string(enum_decl->type_params[i]);
      }
      free(enum_decl->type_params);
      free(enum_decl);
    }
    break;
  }
  case AST_MATCH_STATEMENT: {
    MatchStatement *match = (MatchStatement *)node->data;
    if (match) {
      // expression and arm bodies are children, freed automatically
      if (match->arms) {
        for (size_t i = 0; i < match->arm_count; i++) {
          ast_free_string(match->arms[i].variant_name);
          ast_free_string(match->arms[i].binding_name);
          // body node is a child, freed automatically
        }
        free(match->arms);
      }
      free(match);
    }
    break;
  }
  case AST_TRAIT_DECLARATION: {
    TraitDeclaration *trait_decl = (TraitDeclaration *)node->data;
    if (trait_decl) {
      ast_free_string(trait_decl->name);
      free(trait_decl->methods);
      free(trait_decl);
    }
    break;
  }
  case AST_IMPL_DECLARATION: {
    ImplDeclaration *impl_decl = (ImplDeclaration *)node->data;
    if (impl_decl) {
      ast_free_string(impl_decl->trait_name);
      ast_free_string(impl_decl->for_type_name);
      free(impl_decl->methods);
      free(impl_decl);
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call_expr = (CallExpression *)node->data;
    if (call_expr) {
      ast_free_string(call_expr->function_name);
      free(call_expr->arguments);
      for (size_t i = 0; i < call_expr->argument_count; i++) {
        ast_free_string(call_expr->argument_names
                            ? call_expr->argument_names[i]
                            : NULL);
      }
      free(call_expr->argument_names);
      for (size_t i = 0; i < call_expr->type_arg_count; i++) {
        ast_free_string(call_expr->type_args[i]);
      }
      free(call_expr->type_args);
      ast_free_string(call_expr->written_name);
      free(call_expr);
    }
    break;
  }
  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *fp_call = (FuncPtrCall *)node->data;
    if (fp_call) {
      free(fp_call->arguments);
      free(fp_call);
    }
    break;
  }
  case AST_GPU_LAUNCH: {
    GpuLaunchStatement *launch = (GpuLaunchStatement *)node->data;
    if (launch) {
      free(launch->arguments);
      free(launch);
    }
    break;
  }
  case AST_BARRIER_STATEMENT:
    free(node->data);
    break;

  case AST_DEFER_STATEMENT: {
    DeferStatement *defer_stmt = (DeferStatement *)node->data;
    if (defer_stmt) {
      free(defer_stmt);
    }
    break;
  }

  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *defer_stmt = (DeferStatement *)node->data;
    if (defer_stmt) {
      free(defer_stmt);
    }
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)node->data;
    if (assignment) {
      ast_free_string(assignment->variable_name);
      free(assignment->targets);
      free(assignment);
    }
    break;
  }
  case AST_INLINE_ASM: {
    InlineAsm *inline_asm = (InlineAsm *)node->data;
    if (inline_asm) {
      ast_free_string(inline_asm->assembly_code);
      free(inline_asm);
    }
    break;
  }
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)node->data;
    if (identifier) {
      ast_free_string(identifier->name);
      free(identifier);
    }
    break;
  }
  case AST_STRING_LITERAL: {
    StringLiteral *string_literal = (StringLiteral *)node->data;
    if (string_literal) {
      ast_free_string(string_literal->value);
      free(string_literal);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary_expr = (BinaryExpression *)node->data;
    if (binary_expr) {
      ast_free_string(binary_expr->operator);
      free(binary_expr);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary_expr = (UnaryExpression *)node->data;
    if (unary_expr) {
      ast_free_string(unary_expr->operator);
      free(unary_expr);
    }
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member_access = (MemberAccess *)node->data;
    if (member_access) {
      ast_free_string(member_access->member);
      free(member_access);
    }
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index_expr = (ArrayIndexExpression *)node->data;
    if (index_expr) {
      free(index_expr);
    }
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *new_expr = (NewExpression *)node->data;
    if (new_expr) {
      ast_free_string(new_expr->type_name);
      free(new_expr->extents);
      free(new_expr);
    }
    break;
  }
  case AST_AGGREGATE_LITERAL: {
    AggregateLiteral *literal = (AggregateLiteral *)node->data;
    if (literal) {
      /* The element nodes are children and were already freed above; only the
       * arrays and the field-name strings belong to this struct. */
      for (size_t i = 0; i < literal->element_count; i++) {
        ast_free_string(literal->field_names ? literal->field_names[i] : NULL);
      }
      free(literal->field_names);
      free(literal->elements);
      for (size_t i = 0; i < literal->reloc_count; i++) {
        free(literal->relocs[i].symbol);
        free(literal->relocs[i].string);
      }
      free(literal->relocs);
      free(literal->image);
      free(literal->runtime_stores);
      free(literal);
    }
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast_expr = (CastExpression *)node->data;
    if (cast_expr) {
      ast_free_string(cast_expr->type_name);
      free(cast_expr);
    }
    break;
  }
  case AST_CLOSURE_ADAPT_EXPRESSION: {
    ClosureAdapt *adapt = (ClosureAdapt *)node->data;
    if (adapt) {
      ast_free_string(adapt->ctor_name);
      ast_free_string(adapt->return_type);
      for (size_t i = 0; i < adapt->param_count; i++) {
        ast_free_string(adapt->param_types[i]);
      }
      free(adapt->param_types);
      free(adapt);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *for_stmt = (ForStatement *)node->data;
    if (for_stmt) {
      ast_free_string(for_stmt->label);
      free(for_stmt);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *while_stmt = (WhileStatement *)node->data;
    if (while_stmt) {
      ast_free_string(while_stmt->label);
      free(while_stmt);
    }
    break;
  }
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT: {
    LoopControlStatement *ctrl = (LoopControlStatement *)node->data;
    if (ctrl) {
      ast_free_string(ctrl->target_label);
      free(ctrl);
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *case_clause = (CaseClause *)node->data;
    if (case_clause) {
      free(case_clause);
    }
    break;
  }
  case AST_COMPTIME_FOR: {
    ComptimeForStatement *comptime_for = (ComptimeForStatement *)node->data;
    if (comptime_for) {
      /* `sequence` and `body` are children; the child walk frees them. */
      ast_free_string(comptime_for->binding_name);
      free(comptime_for);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *switch_stmt = (SwitchStatement *)node->data;
    if (switch_stmt) {
      free(switch_stmt->cases);
      free(switch_stmt);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)node->data;
    if (if_stmt) {
      if (if_stmt->else_ifs) {
        free(if_stmt->else_ifs);
      }
      free(if_stmt);
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret_stmt = (ReturnStatement *)node->data;
    if (ret_stmt) {
      free(ret_stmt->values);
      free(ret_stmt);
    }
    break;
  }
  case AST_NUMBER_LITERAL: {
    NumberLiteral *num_lit = (NumberLiteral *)node->data;
    if (num_lit) {
      free(num_lit);
    }
    break;
  }
  case AST_METHOD_DECLARATION: {
    FunctionDeclaration *method_decl = (FunctionDeclaration *)node->data;
    if (method_decl) {
      ast_free_string(method_decl->name);
      ast_free_string(method_decl->return_type);
      ast_free_string(method_decl->link_name);
      for (size_t i = 0; i < method_decl->parameter_count; i++) {
        ast_free_string(method_decl->parameter_names[i]);
        ast_free_string(method_decl->parameter_types[i]);
      }
      free(method_decl->parameter_names);
      free(method_decl->parameter_types);
      for (size_t i = 0; i < method_decl->type_param_count; i++) {
        ast_free_string(method_decl->type_params[i]);
        ast_free_string(method_decl->type_param_traits[i]);
      }
      free(method_decl->type_params);
      free(method_decl->type_param_traits);
      free(method_decl);
    }
    break;
  }
  default:
    // For other node types, assume data is managed by children or is NULL
    break;
  }

  free(node);
}

void ast_add_child(ASTNode *parent, ASTNode *child) {
  if (!parent || !child)
    return;

  parent->children =
      realloc(parent->children, (parent->child_count + 1) * sizeof(ASTNode *));
  if (!parent->children)
    return;

  parent->children[parent->child_count] = child;
  parent->child_count++;
}

// Specific node creation functions
ASTNode *ast_create_program() {
  SourceLocation location = {0, 0, NULL};
  ASTNode *node = ast_create_node(AST_PROGRAM, location);
  if (!node)
    return NULL;

  Program *program = malloc(sizeof(Program));
  if (!program) {
    free(node);
    return NULL;
  }

  program->declarations = NULL;
  program->declaration_count = 0;
  node->data = program;

  return node;
}

ASTNode *ast_create_import_declaration(const char *module_name,
                                       const char *namespace_alias,
                                       const char **selected_names,
                                       size_t selected_count,
                                       SourceLocation location) {
  ASTNode *node = ast_create_node(AST_IMPORT, location);
  if (!node)
    return NULL;

  ImportDeclaration *import_decl = malloc(sizeof(ImportDeclaration));
  if (!import_decl) {
    free(node);
    return NULL;
  }

  import_decl->module_name = ast_copy_string(module_name);
  import_decl->namespace_alias = ast_copy_string(namespace_alias);
  import_decl->selected_names = NULL;
  import_decl->selected_count = 0;
  import_decl->platform_guard = NULL;

  if (selected_names && selected_count > 0) {
    import_decl->selected_names = malloc(selected_count * sizeof(char *));
    if (!import_decl->selected_names) {
      ast_free_string(import_decl->module_name);
      ast_free_string(import_decl->namespace_alias);
      free(import_decl);
      free(node);
      return NULL;
    }
    for (size_t i = 0; i < selected_count; i++) {
      import_decl->selected_names[i] = ast_copy_string(selected_names[i]);
    }
    import_decl->selected_count = selected_count;
  }

  node->data = import_decl;
  return node;
}

ASTNode *ast_create_import_str(const char *file_path, SourceLocation location) {
  ASTNode *node = ast_create_node(AST_IMPORT_STR, location);
  if (!node)
    return NULL;

  ImportStrExpression *import_str = malloc(sizeof(ImportStrExpression));
  if (!import_str) {
    free(node);
    return NULL;
  }

  import_str->file_path = ast_copy_string(file_path);
  node->data = import_str;

  return node;
}

ASTNode *ast_create_var_declaration(const char *name, const char *type_name,
                                    ASTNode *initializer,
                                    SourceLocation location) {
  ASTNode *node = ast_create_node(AST_VAR_DECLARATION, location);
  if (!node)
    return NULL;

  VarDeclaration *var_decl = malloc(sizeof(VarDeclaration));
  if (!var_decl) {
    free(node);
    return NULL;
  }

  var_decl->name = ast_intern_string(name);
  var_decl->type_name = ast_intern_string(type_name);
  var_decl->initializer = initializer;
  var_decl->is_extern = 0;
  var_decl->is_exported = 0;
  var_decl->is_const = 0;
  var_decl->structural_type = 0;
  var_decl->address_space = AST_ADDRESS_SPACE_DEFAULT;
  var_decl->link_name = NULL;
  var_decl->composed_name = NULL;
  node->data = var_decl;

  if (initializer) {
    ast_add_child(node, initializer);
  }

  return node;
}

ASTNode *ast_create_function_declaration(const char *name, char **param_names,
                                         char **param_types, size_t param_count,
                                         const char *return_type, ASTNode *body,
                                         SourceLocation location) {
  ASTNode *node = ast_create_node(AST_FUNCTION_DECLARATION, location);
  if (!node)
    return NULL;

  FunctionDeclaration *func_decl = malloc(sizeof(FunctionDeclaration));
  if (!func_decl) {
    free(node);
    return NULL;
  }

  func_decl->name = ast_intern_string(name);
  func_decl->return_type = ast_intern_string(return_type);
  func_decl->return_types = NULL;
  func_decl->return_type_count = 0;
  func_decl->parameter_count = param_count;
  func_decl->body = body;
  func_decl->is_exported = 0;
  func_decl->is_extern = 0;
  func_decl->is_kernel = 0;
  func_decl->kernel_block[0] = 0;
  func_decl->kernel_block[1] = 0;
  func_decl->kernel_block[2] = 0;
  func_decl->kernel_threads_per_item = 0;
  func_decl->link_name = NULL;
  func_decl->type_params = NULL;
  func_decl->type_param_traits = NULL;
  func_decl->type_param_count = 0;
  func_decl->is_inline = 0;
  func_decl->is_inline_contract = 0;
  func_decl->is_noinline = 0;
  func_decl->is_pure = 0;
  func_decl->is_noalloc = 0;
  func_decl->is_test = 0;
  func_decl->is_swappable = 0;
  func_decl->is_naked = 0;
  func_decl->is_interrupt = 0;
  func_decl->rewrite_role = 0;
  func_decl->is_variadic = 0;
  func_decl->simd_mode = SIMD_ATTR_NONE;
  func_decl->captured_names = NULL;
  func_decl->captured_types = NULL;
  func_decl->captured_count = 0;
  func_decl->env_struct_name = NULL;
  func_decl->composed_name = NULL;

  if (param_count > 0) {
    func_decl->parameter_names = malloc(param_count * sizeof(char *));
    func_decl->parameter_types = malloc(param_count * sizeof(char *));
    if (!func_decl->parameter_names || !func_decl->parameter_types) {
      free(func_decl->parameter_names);
      free(func_decl->parameter_types);
      free(func_decl);
      free(node);
      return NULL;
    }

    for (size_t i = 0; i < param_count; i++) {
      func_decl->parameter_names[i] = ast_intern_string(param_names[i]);
      func_decl->parameter_types[i] = ast_intern_string(param_types[i]);
    }
  } else {
    func_decl->parameter_names = NULL;
    func_decl->parameter_types = NULL;
  }

  node->data = func_decl;

  if (body) {
    ast_add_child(node, body);
  }

  return node;
}

ASTNode *ast_create_struct_declaration(const char *name, char **field_names,
                                       char **field_types, size_t field_count,
                                       ASTNode **methods, size_t method_count,
                                       SourceLocation location) {
  ASTNode *node = ast_create_node(AST_STRUCT_DECLARATION, location);
  if (!node)
    return NULL;

  StructDeclaration *struct_decl = malloc(sizeof(StructDeclaration));
  if (!struct_decl) {
    free(node);
    return NULL;
  }

  struct_decl->name = ast_intern_string(name);
  struct_decl->field_count = field_count;
  struct_decl->method_count = method_count;
  struct_decl->is_exported = 0;
  struct_decl->type_params = NULL;
  struct_decl->type_param_traits = NULL;
  struct_decl->type_param_count = 0;
  struct_decl->composed_name = NULL;

  if (field_count > 0) {
    struct_decl->field_names = malloc(field_count * sizeof(char *));
    struct_decl->field_types = malloc(field_count * sizeof(char *));

    for (size_t i = 0; i < field_count; i++) {
      struct_decl->field_names[i] = ast_intern_string(field_names[i]);
      struct_decl->field_types[i] = ast_intern_string(field_types[i]);
    }
  } else {
    struct_decl->field_names = NULL;
    struct_decl->field_types = NULL;
  }

  if (method_count > 0) {
    struct_decl->methods = malloc(method_count * sizeof(ASTNode *));
    for (size_t i = 0; i < method_count; i++) {
      struct_decl->methods[i] = methods[i];
      if (methods[i]) {
        ast_add_child(node, methods[i]);
      }
    }
  } else {
    struct_decl->methods = NULL;
  }

  node->data = struct_decl;

  return node;
}

ASTNode *ast_create_enum_declaration(const char *name, EnumVariant *variants,
                                     size_t variant_count,
                                     SourceLocation location) {
  ASTNode *node = ast_create_node(AST_ENUM_DECLARATION, location);
  if (!node)
    return NULL;

  EnumDeclaration *enum_decl = malloc(sizeof(EnumDeclaration));
  if (!enum_decl) {
    free(node);
    return NULL;
  }

  enum_decl->name = ast_intern_string(name);
  enum_decl->is_exported = 0;
  enum_decl->variant_count = variant_count;

  if (variant_count > 0 && variants) {
    enum_decl->variants = malloc(variant_count * sizeof(EnumVariant));
    if (!enum_decl->variants) {
      if (enum_decl->name)
        ast_free_string(enum_decl->name);
      free(enum_decl);
      free(node);
      return NULL;
    }
    for (size_t i = 0; i < variant_count; i++) {
      enum_decl->variants[i].name = ast_intern_string(variants[i].name);
      enum_decl->variants[i].payload_type =
          ast_intern_string(variants[i].payload_type);
      enum_decl->variants[i].value = variants[i].value;
      if (variants[i].value) {
        ast_add_child(node, variants[i].value);
      }
    }
  } else {
    enum_decl->variants = NULL;
  }

  enum_decl->type_params = NULL;
  enum_decl->type_param_count = 0;

  node->data = enum_decl;
  return node;
}

static ASTNode *ast_create_match_node(ASTNode *expression, MatchArm *arms,
                                      size_t arm_count, int is_expression,
                                      SourceLocation location) {
  ASTNode *node = ast_create_node(AST_MATCH_STATEMENT, location);
  if (!node)
    return NULL;

  MatchStatement *match = malloc(sizeof(MatchStatement));
  if (!match) {
    free(node);
    return NULL;
  }

  match->is_expression = is_expression;
  match->expression = expression;
  if (expression)
    ast_add_child(node, expression);

  match->arm_count = arm_count;
  if (arm_count > 0 && arms) {
    match->arms = malloc(arm_count * sizeof(MatchArm));
    if (!match->arms) {
      free(match);
      free(node);
      return NULL;
    }
    for (size_t i = 0; i < arm_count; i++) {
      match->arms[i].variant_name = ast_intern_string(arms[i].variant_name);
      match->arms[i].binding_name = ast_intern_string(arms[i].binding_name);
      match->arms[i].body = arms[i].body;
      match->arms[i].is_default = arms[i].is_default;
      if (arms[i].body)
        ast_add_child(node, arms[i].body);
    }
  } else {
    match->arms = NULL;
  }

  node->data = match;
  return node;
}

ASTNode *ast_create_match_statement(ASTNode *expression, MatchArm *arms,
                                    size_t arm_count,
                                    SourceLocation location) {
  return ast_create_match_node(expression, arms, arm_count, 0, location);
}

ASTNode *ast_create_match_expression(ASTNode *expression, MatchArm *arms,
                                     size_t arm_count,
                                     SourceLocation location) {
  return ast_create_match_node(expression, arms, arm_count, 1, location);
}

ASTNode *ast_create_trait_declaration(const char *name,
                                      SourceLocation location) {
  ASTNode *node = ast_create_node(AST_TRAIT_DECLARATION, location);
  if (!node) {
    return NULL;
  }

  TraitDeclaration *trait_decl = malloc(sizeof(TraitDeclaration));
  if (!trait_decl) {
    free(node);
    return NULL;
  }

  trait_decl->name = ast_intern_string(name);
  trait_decl->is_exported = 0;
  trait_decl->methods = NULL;
  trait_decl->method_count = 0;
  node->data = trait_decl;
  return node;
}

ASTNode *ast_create_impl_declaration(const char *trait_name,
                                     const char *for_type_name,
                                     SourceLocation location) {
  ASTNode *node = ast_create_node(AST_IMPL_DECLARATION, location);
  if (!node) {
    return NULL;
  }

  ImplDeclaration *impl_decl = malloc(sizeof(ImplDeclaration));
  if (!impl_decl) {
    free(node);
    return NULL;
  }

  impl_decl->trait_name = ast_intern_string(trait_name);
  impl_decl->for_type_name = ast_intern_string(for_type_name);
  impl_decl->methods = NULL;
  impl_decl->method_count = 0;
  node->data = impl_decl;
  return node;
}

ASTNode *ast_create_call_expression(const char *function_name,
                                    ASTNode **arguments, size_t argument_count,
                                    SourceLocation location) {
  ASTNode *node = ast_create_node(AST_FUNCTION_CALL, location);
  if (!node)
    return NULL;

  CallExpression *call_expr = malloc(sizeof(CallExpression));
  if (!call_expr) {
    free(node);
    return NULL;
  }

  call_expr->function_name = ast_intern_string(function_name);
  call_expr->argument_count = argument_count;
  call_expr->argument_names = NULL;
  call_expr->object = NULL;
  call_expr->type_args = NULL;
  call_expr->type_arg_count = 0;
  call_expr->written_name = NULL;
  call_expr->is_indirect_call = 0;
  call_expr->callee_closure_env = NULL;
  call_expr->is_gpu_index = 0;
  call_expr->is_gpu_atomic = 0;
  call_expr->atomic_address_space = MTLC_ADDRESS_SPACE_DEFAULT;
  call_expr->atomic_memory_order = MTLC_MEMORY_ORDER_DEFAULT;
  call_expr->atomic_failure_order = MTLC_MEMORY_ORDER_DEFAULT;
  call_expr->atomic_memory_scope = MTLC_MEMORY_SCOPE_DEFAULT;
  call_expr->is_tensor_mma = 0;
  call_expr->is_tensor_matmul = 0;
  call_expr->is_gpu_async_copy = 0;
  call_expr->async_copy_element_count = 0;
  call_expr->async_copy_transaction_bytes = 0;
  call_expr->async_copy_pending_groups = 0;
  call_expr->async_copy_cache = MTLC_ASYNC_CACHE_DEFAULT;
  call_expr->is_tensor_transfer = 0;
  call_expr->tensor_transfer_desc = (MtlcTensorTransferDesc){0};
  call_expr->tensor_transfer_view_argument = SIZE_MAX;
  for (size_t dimension = 0; dimension < MTLC_TENSOR_MAX_RANK; dimension++)
    call_expr->tensor_transfer_coordinate_arguments[dimension] = SIZE_MAX;
  call_expr->tensor_mma_desc = (MtlcTensorMmaDesc){0};
  call_expr->tensor_metadata_argument = SIZE_MAX;
  call_expr->tensor_a_scale_argument = SIZE_MAX;
  call_expr->tensor_b_scale_argument = SIZE_MAX;
  call_expr->tensor_a_stride_argument = SIZE_MAX;
  call_expr->tensor_b_stride_argument = SIZE_MAX;
  call_expr->tensor_c_stride_argument = SIZE_MAX;
  call_expr->tensor_d_stride_argument = SIZE_MAX;
  call_expr->is_tensor_epilogue = 0;
  call_expr->tensor_epilogue_desc = (MtlcTensorEpilogueDesc){0};
  call_expr->tensor_epilogue_bias_argument = SIZE_MAX;
  call_expr->tensor_epilogue_alpha_argument = SIZE_MAX;
  call_expr->tensor_epilogue_beta_argument = SIZE_MAX;
  call_expr->tensor_epilogue_clamp_min_argument = SIZE_MAX;
  call_expr->tensor_epilogue_clamp_max_argument = SIZE_MAX;
  call_expr->tensor_epilogue_stride_argument = SIZE_MAX;
  call_expr->tensor_epilogue_bias_stride_argument = SIZE_MAX;

  if (argument_count > 0) {
    call_expr->arguments = malloc(argument_count * sizeof(ASTNode *));
    for (size_t i = 0; i < argument_count; i++) {
      call_expr->arguments[i] = arguments[i];
      if (arguments[i]) {
        ast_add_child(node, arguments[i]);
      }
    }
  } else {
    call_expr->arguments = NULL;
  }

  node->data = call_expr;

  return node;
}

ASTNode *ast_create_func_ptr_call(ASTNode *function, ASTNode **arguments,
                                  size_t argument_count,
                                  SourceLocation location) {
  ASTNode *node = ast_create_node(AST_FUNC_PTR_CALL, location);
  if (!node)
    return NULL;

  FuncPtrCall *fp_call = malloc(sizeof(FuncPtrCall));
  if (!fp_call) {
    free(node);
    return NULL;
  }

  fp_call->function = function;
  fp_call->argument_count = argument_count;

  if (function) {
    ast_add_child(node, function);
  }

  if (argument_count > 0) {
    fp_call->arguments = malloc(argument_count * sizeof(ASTNode *));
    for (size_t i = 0; i < argument_count; i++) {
      fp_call->arguments[i] = arguments[i];
      if (arguments[i]) {
        ast_add_child(node, arguments[i]);
      }
    }
  } else {
    fp_call->arguments = NULL;
  }

  node->data = fp_call;

  return node;
}

ASTNode *ast_create_gpu_launch(ASTNode *kernel, ASTNode **grid,
                               ASTNode **block,
                               ASTNode *dynamic_shared_bytes, ASTNode *stream,
                               ASTNode **arguments, size_t argument_count,
                               SourceLocation location) {
  ASTNode *node = ast_create_node(AST_GPU_LAUNCH, location);
  GpuLaunchStatement *launch = NULL;
  if (!node) {
    return NULL;
  }
  launch = calloc(1, sizeof(*launch));
  if (!launch) {
    free(node);
    return NULL;
  }
  launch->kernel = kernel;
  launch->dynamic_shared_bytes = dynamic_shared_bytes;
  launch->stream = stream;
  launch->argument_count = argument_count;
  if (argument_count > 0) {
    launch->arguments = malloc(argument_count * sizeof(*launch->arguments));
    if (!launch->arguments) {
      free(launch);
      free(node);
      return NULL;
    }
    memcpy(launch->arguments, arguments,
           argument_count * sizeof(*launch->arguments));
  }
  ast_add_child(node, kernel);
  for (size_t i = 0; i < 3; i++) {
    launch->grid[i] = grid ? grid[i] : NULL;
    launch->block[i] = block ? block[i] : NULL;
    ast_add_child(node, launch->grid[i]);
    ast_add_child(node, launch->block[i]);
  }
  ast_add_child(node, dynamic_shared_bytes);
  ast_add_child(node, stream);
  for (size_t i = 0; i < argument_count; i++) {
    ast_add_child(node, launch->arguments[i]);
  }
  node->data = launch;
  return node;
}

ASTNode *ast_create_barrier_statement(unsigned memory_regions,
                                      AstMemoryOrder memory_order,
                                      SourceLocation location) {
  ASTNode *node = ast_create_node(AST_BARRIER_STATEMENT, location);
  if (!node) return NULL;
  BarrierStatement *barrier = malloc(sizeof(*barrier));
  if (!barrier) {
    free(node);
    return NULL;
  }
  barrier->memory_regions = memory_regions;
  barrier->memory_order = memory_order;
  node->data = barrier;
  return node;
}

ASTNode *ast_create_assignment(const char *variable_name, ASTNode *value,
                               SourceLocation location) {
  ASTNode *node = ast_create_node(AST_ASSIGNMENT, location);
  if (!node)
    return NULL;

  Assignment *assignment = malloc(sizeof(Assignment));
  if (!assignment) {
    free(node);
    return NULL;
  }

  assignment->variable_name = ast_intern_string(variable_name);
  assignment->value = value;
  assignment->target = NULL;
  assignment->targets = NULL;
  assignment->target_count = 0;
  node->data = assignment;

  if (value) {
    ast_add_child(node, value);
  }

  return node;
}

ASTNode *ast_create_multi_assignment(ASTNode **targets, size_t target_count,
                                     ASTNode *value, SourceLocation location) {
  if (!targets || target_count < 2 || !value) {
    return NULL;
  }

  ASTNode *node = ast_create_node(AST_ASSIGNMENT, location);
  Assignment *assignment = node ? malloc(sizeof(Assignment)) : NULL;
  if (!node || !assignment) {
    free(assignment);
    free(node);
    return NULL;
  }

  assignment->variable_name = NULL;
  assignment->value = value;
  assignment->target = NULL;
  assignment->targets = targets;
  assignment->target_count = target_count;
  node->data = assignment;
  for (size_t i = 0; i < target_count; i++) {
    ast_add_child(node, targets[i]);
  }
  ast_add_child(node, value);
  return node;
}

ASTNode *ast_create_inline_asm(const char *assembly_code,
                               SourceLocation location) {
  ASTNode *node = ast_create_node(AST_INLINE_ASM, location);
  if (!node)
    return NULL;

  InlineAsm *inline_asm = malloc(sizeof(InlineAsm));
  if (!inline_asm) {
    free(node);
    return NULL;
  }

  inline_asm->assembly_code = ast_copy_string(assembly_code);
  node->data = inline_asm;

  return node;
}

ASTNode *ast_create_identifier_with_scope(const char *name,
                                          ASTScopeId scope_id,
                                          SourceLocation location) {
  ASTNode *node = ast_create_node(AST_IDENTIFIER, location);
  if (!node)
    return NULL;

  Identifier *identifier = malloc(sizeof(Identifier));
  if (!identifier) {
    free(node);
    return NULL;
  }

  identifier->name = ast_intern_string(name);
  identifier->scope_id = scope_id;
  node->data = identifier;

  return node;
}

ASTNode *ast_create_identifier(const char *name, SourceLocation location) {
  return ast_create_identifier_with_scope(name, AST_SCOPE_ID_UNRESOLVED,
                                          location);
}

ASTNode *ast_create_number_literal(long long int_value,
                                   SourceLocation location,
                                   unsigned char int_radix) {
  ASTNode *node = ast_create_node(AST_NUMBER_LITERAL, location);
  if (!node)
    return NULL;

  NumberLiteral *number_literal = malloc(sizeof(NumberLiteral));
  if (!number_literal) {
    free(node);
    return NULL;
  }

  number_literal->int_value = int_value;
  number_literal->is_float = 0;
  number_literal->is_char = 0;
  number_literal->int_radix =
      (int_radix == 2u || int_radix == 16u) ? int_radix : 10u;
  node->data = number_literal;

  return node;
}

ASTNode *ast_create_float_literal(double float_value, SourceLocation location) {
  ASTNode *node = ast_create_node(AST_NUMBER_LITERAL, location);
  if (!node)
    return NULL;

  NumberLiteral *number_literal = malloc(sizeof(NumberLiteral));
  if (!number_literal) {
    free(node);
    return NULL;
  }

  number_literal->float_value = float_value;
  number_literal->is_float = 1;
  number_literal->is_char = 0;
  number_literal->int_radix = 10;
  node->data = number_literal;

  return node;
}

ASTNode *ast_create_string_literal(const char *value, size_t length,
                                   SourceLocation location) {
  ASTNode *node = ast_create_node(AST_STRING_LITERAL, location);
  if (!node)
    return NULL;

  StringLiteral *string_literal = malloc(sizeof(StringLiteral));
  if (!string_literal) {
    free(node);
    return NULL;
  }

  string_literal->value = ast_copy_bytes(value, length);
  string_literal->length = length;
  node->data = string_literal;

  return node;
}

ASTNode *ast_create_binary_expression(ASTNode *left, const char *operator,
                                      ASTNode *right, SourceLocation location) {
  ASTNode *node = ast_create_node(AST_BINARY_EXPRESSION, location);
  if (!node)
    return NULL;

  BinaryExpression *binary_expr = malloc(sizeof(BinaryExpression));
  if (!binary_expr) {
    free(node);
    return NULL;
  }

  binary_expr->left = left;
  binary_expr->right = right;
  binary_expr->operator = ast_copy_string(operator);
  node->data = binary_expr;

  if (left) {
    ast_add_child(node, left);
  }
  if (right) {
    ast_add_child(node, right);
  }

  return node;
}

ASTNode *ast_create_unary_expression(const char *operator, ASTNode *operand,
                                     SourceLocation location) {
  ASTNode *node = ast_create_node(AST_UNARY_EXPRESSION, location);
  if (!node)
    return NULL;

  UnaryExpression *unary_expr = malloc(sizeof(UnaryExpression));
  if (!unary_expr) {
    free(node);
    return NULL;
  }

  unary_expr->operator = ast_copy_string(operator);
  unary_expr->operand = operand;
  node->data = unary_expr;

  if (operand) {
    ast_add_child(node, operand);
  }

  return node;
}

ASTNode *ast_create_member_access(ASTNode *object, const char *member,
                                  SourceLocation location) {
  ASTNode *node = ast_create_node(AST_MEMBER_ACCESS, location);
  if (!node)
    return NULL;

  MemberAccess *member_access = malloc(sizeof(MemberAccess));
  if (!member_access) {
    free(node);
    return NULL;
  }

  member_access->object = object;
  member_access->member = ast_intern_string(member);
  node->data = member_access;

  if (object) {
    ast_add_child(node, object);
  }

  return node;
}

ASTNode *ast_create_array_index_expression(ASTNode *array, ASTNode *index,
                                           SourceLocation location) {
  ASTNode *node = ast_create_node(AST_INDEX_EXPRESSION, location);
  if (!node)
    return NULL;

  ArrayIndexExpression *index_expr = malloc(sizeof(ArrayIndexExpression));
  if (!index_expr) {
    free(node);
    return NULL;
  }

  index_expr->array = array;
  index_expr->index = index;
  node->data = index_expr;

  if (array) {
    ast_add_child(node, array);
  }
  if (index) {
    ast_add_child(node, index);
  }

  return node;
}

ASTNode *ast_create_aggregate_literal(int is_struct, ASTNode **elements,
                                      char **field_names, size_t element_count,
                                      ASTNode *repeat_count,
                                      SourceLocation location) {
  ASTNode *node = ast_create_node(AST_AGGREGATE_LITERAL, location);
  if (!node)
    return NULL;

  AggregateLiteral *literal = malloc(sizeof(AggregateLiteral));
  if (!literal) {
    free(node);
    return NULL;
  }

  literal->is_struct = is_struct ? 1 : 0;
  literal->elements = elements;
  literal->field_names = field_names;
  literal->element_count = element_count;
  literal->repeat_count = repeat_count;
  literal->image = NULL;
  literal->image_size = 0;
  literal->relocs = NULL;
  literal->reloc_count = 0;
  literal->runtime_stores = NULL;
  literal->runtime_store_count = 0;
  node->data = literal;

  for (size_t i = 0; i < element_count; i++) {
    if (elements && elements[i]) {
      ast_add_child(node, elements[i]);
    }
  }
  if (repeat_count) {
    ast_add_child(node, repeat_count);
  }

  return node;
}

ASTNode *ast_create_method_call(ASTNode *object, const char *method_name,
                                ASTNode **arguments, size_t argument_count,
                                SourceLocation location) {
  ASTNode *node = ast_create_node(AST_FUNCTION_CALL, location);
  if (!node)
    return NULL;

  CallExpression *call_expr = malloc(sizeof(CallExpression));
  if (!call_expr) {
    free(node);
    return NULL;
  }

  call_expr->function_name = ast_intern_string(method_name);
  call_expr->argument_count = argument_count;
  call_expr->argument_names = NULL;
  call_expr->object = object;
  call_expr->type_args = NULL;
  call_expr->type_arg_count = 0;
  call_expr->written_name = NULL;
  call_expr->is_indirect_call = 0;
  call_expr->callee_closure_env = NULL;
  call_expr->is_gpu_index = 0;
  call_expr->is_gpu_atomic = 0;
  call_expr->atomic_address_space = MTLC_ADDRESS_SPACE_DEFAULT;
  call_expr->atomic_memory_order = MTLC_MEMORY_ORDER_DEFAULT;
  call_expr->atomic_failure_order = MTLC_MEMORY_ORDER_DEFAULT;
  call_expr->atomic_memory_scope = MTLC_MEMORY_SCOPE_DEFAULT;
  call_expr->is_tensor_mma = 0;
  call_expr->is_tensor_matmul = 0;
  call_expr->is_gpu_async_copy = 0;
  call_expr->async_copy_element_count = 0;
  call_expr->async_copy_transaction_bytes = 0;
  call_expr->async_copy_pending_groups = 0;
  call_expr->async_copy_cache = MTLC_ASYNC_CACHE_DEFAULT;
  call_expr->is_tensor_transfer = 0;
  call_expr->tensor_transfer_desc = (MtlcTensorTransferDesc){0};
  call_expr->tensor_transfer_view_argument = SIZE_MAX;
  for (size_t dimension = 0; dimension < MTLC_TENSOR_MAX_RANK; dimension++)
    call_expr->tensor_transfer_coordinate_arguments[dimension] = SIZE_MAX;
  call_expr->tensor_mma_desc = (MtlcTensorMmaDesc){0};
  call_expr->tensor_metadata_argument = SIZE_MAX;
  call_expr->tensor_a_scale_argument = SIZE_MAX;
  call_expr->tensor_b_scale_argument = SIZE_MAX;
  call_expr->tensor_a_stride_argument = SIZE_MAX;
  call_expr->tensor_b_stride_argument = SIZE_MAX;
  call_expr->tensor_c_stride_argument = SIZE_MAX;
  call_expr->tensor_d_stride_argument = SIZE_MAX;
  call_expr->is_tensor_epilogue = 0;
  call_expr->tensor_epilogue_desc = (MtlcTensorEpilogueDesc){0};
  call_expr->tensor_epilogue_bias_argument = SIZE_MAX;
  call_expr->tensor_epilogue_alpha_argument = SIZE_MAX;
  call_expr->tensor_epilogue_beta_argument = SIZE_MAX;
  call_expr->tensor_epilogue_clamp_min_argument = SIZE_MAX;
  call_expr->tensor_epilogue_clamp_max_argument = SIZE_MAX;
  call_expr->tensor_epilogue_stride_argument = SIZE_MAX;
  call_expr->tensor_epilogue_bias_stride_argument = SIZE_MAX;

  if (argument_count > 0) {
    call_expr->arguments = malloc(argument_count * sizeof(ASTNode *));
    for (size_t i = 0; i < argument_count; i++) {
      call_expr->arguments[i] = arguments[i];
      if (arguments[i]) {
        ast_add_child(node, arguments[i]);
      }
    }
  } else {
    call_expr->arguments = NULL;
  }

  // The object is also a child for proper memory management
  if (object) {
    ast_add_child(node, object);
  }

  node->data = call_expr;
  return node;
}

ASTNode *ast_create_new_expression(const char *type_name,
                                   SourceLocation location) {
  ASTNode *node = ast_create_node(AST_NEW_EXPRESSION, location);
  if (!node)
    return NULL;

  NewExpression *new_expr = malloc(sizeof(NewExpression));
  if (!new_expr) {
    free(node);
    return NULL;
  }

  new_expr->type_name = ast_intern_string(type_name);
  new_expr->count = NULL;
  new_expr->extents = NULL;
  new_expr->extent_count = 0;
  node->data = new_expr;

  return node;
}

int ast_new_expression_add_extent(ASTNode *node, ASTNode *extent) {
  NewExpression *new_expr = node ? (NewExpression *)node->data : NULL;
  ASTNode **grown = NULL;
  if (!new_expr || !extent) {
    return 0;
  }
  grown = realloc(new_expr->extents,
                  (new_expr->extent_count + 1) * sizeof(ASTNode *));
  if (!grown) {
    return 0;
  }
  new_expr->extents = grown;
  new_expr->extents[new_expr->extent_count++] = extent;
  ast_add_child(node, extent);
  return 1;
}

/* Drop a node's claim on its children without freeing them. The compiler uses
 * it where a synthesized node borrows expressions another node already owns:
 * a walk still reaches them through the owner, and only one destructor does. */
void ast_release_children(ASTNode *node) {
  if (!node) {
    return;
  }
  free(node->children);
  node->children = NULL;
  node->child_count = 0;
}

ASTNode *ast_create_new_array_expression(const char *type_name, ASTNode *count,
                                         SourceLocation location) {
  ASTNode *node = ast_create_new_expression(type_name, location);
  NewExpression *new_expr = node ? (NewExpression *)node->data : NULL;
  if (!node) {
    return NULL;
  }
  new_expr->count = count;
  if (count) {
    ast_add_child(node, count);
  }
  return node;
}

ASTNode *ast_create_field_assignment(ASTNode *target, ASTNode *value,
                                     SourceLocation location) {
  ASTNode *node = ast_create_node(AST_ASSIGNMENT, location);
  if (!node)
    return NULL;

  Assignment *assignment = malloc(sizeof(Assignment));
  if (!assignment) {
    free(node);
    return NULL;
  }

  assignment->variable_name = NULL; // Not a simple variable assignment
  assignment->value = value;
  assignment->target = target;
  assignment->targets = NULL;
  assignment->target_count = 0;
  node->data = assignment;

  if (target) {
    ast_add_child(node, target);
  }
  if (value) {
    ast_add_child(node, value);
  }

  return node;
}

ASTNode *ast_create_for_statement(ASTNode *initializer, ASTNode *condition,
                                  ASTNode *increment, ASTNode *body,
                                  SourceLocation location) {
  ASTNode *node = ast_create_node(AST_FOR_STATEMENT, location);
  if (!node)
    return NULL;

  ForStatement *for_stmt = malloc(sizeof(ForStatement));
  if (!for_stmt) {
    free(node);
    return NULL;
  }

  for_stmt->initializer = initializer;
  for_stmt->condition = condition;
  for_stmt->increment = increment;
  for_stmt->body = body;
  for_stmt->label = NULL;
  for_stmt->simd_mode = SIMD_ATTR_NONE;
  for_stmt->unroll_factor = 0;
  node->data = for_stmt;

  if (initializer)
    ast_add_child(node, initializer);
  if (condition)
    ast_add_child(node, condition);
  if (increment)
    ast_add_child(node, increment);
  if (body)
    ast_add_child(node, body);

  return node;
}

/* Collapse a member access in place into an integer literal, keeping the
 * node's address and location. Const eval uses this to bake a compile-time
 * answer into the tree at the point it is known: a `comptime for` binding is
 * only in scope while its expansion is checked, so no later pass could work
 * the value out again. The node pointer is kept because parents hold it. */
int ast_fold_member_access_to_int(ASTNode *node, long long value) {
  if (!node || (node->type != AST_MEMBER_ACCESS &&
                node->type != AST_INDEX_EXPRESSION &&
                node->type != AST_IDENTIFIER)) {
    return 0;
  }
  NumberLiteral *literal = malloc(sizeof(NumberLiteral));
  if (!literal) {
    return 0;
  }
  literal->int_value = value;
  literal->is_float = 0;
  literal->is_char = 0;
  literal->int_radix = 10;

  for (size_t i = 0; i < node->child_count; i++) {
    ast_destroy_node(node->children[i]);
  }
  free(node->children);
  node->children = NULL;
  node->child_count = 0;

  if (node->type == AST_MEMBER_ACCESS) {
    MemberAccess *member_access = (MemberAccess *)node->data;
    if (member_access) {
      ast_free_string(member_access->member);
      free(member_access);
    }
  } else {
    free(node->data);
  }
  node->type = AST_NUMBER_LITERAL;
  node->data = literal;
  return 1;
}

/* Same shape as the integer fold, for a float a compile-time query answered
 * with. */
int ast_fold_member_access_to_float(ASTNode *node, double value) {
  NumberLiteral *literal = NULL;
  size_t i;
  if (!node || (node->type != AST_MEMBER_ACCESS &&
                node->type != AST_INDEX_EXPRESSION &&
                node->type != AST_IDENTIFIER)) {
    return 0;
  }
  literal = malloc(sizeof(NumberLiteral));
  if (!literal) {
    return 0;
  }
  literal->float_value = value;
  literal->is_float = 1;
  literal->is_char = 0;
  literal->int_radix = 10;

  for (i = 0; i < node->child_count; i++) {
    ast_destroy_node(node->children[i]);
  }
  free(node->children);
  node->children = NULL;
  node->child_count = 0;

  if (node->type == AST_MEMBER_ACCESS) {
    MemberAccess *member_access = (MemberAccess *)node->data;
    if (member_access) {
      ast_free_string(member_access->member);
      free(member_access);
    }
  } else {
    free(node->data);
  }
  node->type = AST_NUMBER_LITERAL;
  node->data = literal;
  return 1;
}

/* `ident("read_", f.name)` where a value goes, folded to the name it composed.
 * Same shape as the folds above: the node becomes what it stood for, so nothing
 * downstream has to know an `ident(...)` was ever there. */
int ast_fold_call_to_identifier(ASTNode *node, const char *name) {
  if (!node || !name || node->type != AST_FUNCTION_CALL) {
    return 0;
  }
  Identifier *identifier = malloc(sizeof(Identifier));
  if (!identifier) {
    return 0;
  }
  identifier->name = ast_intern_string(name);
  identifier->scope_id = 0;
  if (!identifier->name) {
    free(identifier);
    return 0;
  }

  for (size_t i = 0; i < node->child_count; i++) {
    ast_destroy_node(node->children[i]);
  }
  free(node->children);
  node->children = NULL;
  node->child_count = 0;

  CallExpression *call = (CallExpression *)node->data;
  if (call) {
    ast_free_string(call->function_name);
    free(call->arguments);
    for (size_t i = 0; i < call->argument_count; i++) {
      ast_free_string(call->argument_names ? call->argument_names[i] : NULL);
    }
    free(call->argument_names);
    for (size_t i = 0; i < call->type_arg_count; i++) {
      ast_free_string(call->type_args[i]);
    }
    free(call->type_args);
    ast_free_string(call->written_name);
    free(call);
  }
  node->type = AST_IDENTIFIER;
  node->data = identifier;
  return 1;
}

/* Same shape as the integer fold above, for `.name`. The string is interned by
 * the caller, so the literal borrows it and the node owns nothing new. */
int ast_fold_member_access_to_string(ASTNode *node, const char *value) {
  if (!node || !value || (node->type != AST_MEMBER_ACCESS &&
                          node->type != AST_INDEX_EXPRESSION &&
                          node->type != AST_IDENTIFIER)) {
    return 0;
  }
  StringLiteral *literal = malloc(sizeof(StringLiteral));
  if (!literal) {
    return 0;
  }
  literal->value = ast_intern_string(value);
  if (!literal->value) {
    free(literal);
    return 0;
  }
  literal->length = strlen(value);

  for (size_t i = 0; i < node->child_count; i++) {
    ast_destroy_node(node->children[i]);
  }
  free(node->children);
  node->children = NULL;
  node->child_count = 0;

  if (node->type == AST_MEMBER_ACCESS) {
    MemberAccess *member_access = (MemberAccess *)node->data;
    if (member_access) {
      ast_free_string(member_access->member);
      free(member_access);
    }
  } else {
    free(node->data);
  }
  node->type = AST_STRING_LITERAL;
  node->data = literal;
  return 1;
}

ASTNode *ast_create_comptime_for(const char *binding_name, ASTNode *sequence,
                                 ASTNode *body, SourceLocation location) {
  ASTNode *node = ast_create_node(AST_COMPTIME_FOR, location);
  if (!node)
    return NULL;

  ComptimeForStatement *comptime_for = malloc(sizeof(ComptimeForStatement));
  if (!comptime_for) {
    free(node);
    return NULL;
  }

  comptime_for->binding_name = ast_copy_string(binding_name);
  comptime_for->sequence = sequence;
  comptime_for->body = body;
  comptime_for->keyword_location = location;
  node->data = comptime_for;

  if (sequence)
    ast_add_child(node, sequence);
  if (body)
    ast_add_child(node, body);

  return node;
}

ASTNode *ast_create_case_clause(ASTNode *value, ASTNode *body, int is_default,
                                SourceLocation location) {
  ASTNode *node = ast_create_node(AST_CASE_CLAUSE, location);
  if (!node)
    return NULL;

  CaseClause *case_clause = malloc(sizeof(CaseClause));
  if (!case_clause) {
    free(node);
    return NULL;
  }

  case_clause->value = value;
  case_clause->value_high = NULL;
  case_clause->body = body;
  case_clause->is_default = is_default;
  node->data = case_clause;

  if (value)
    ast_add_child(node, value);
  if (body)
    ast_add_child(node, body);

  return node;
}

ASTNode *ast_create_switch_statement(ASTNode *expression, ASTNode **cases,
                                     size_t case_count,
                                     SourceLocation location) {
  ASTNode *node = ast_create_node(AST_SWITCH_STATEMENT, location);
  if (!node)
    return NULL;

  SwitchStatement *switch_stmt = malloc(sizeof(SwitchStatement));
  if (!switch_stmt) {
    free(node);
    return NULL;
  }

  switch_stmt->expression = expression;
  switch_stmt->case_count = case_count;

  if (case_count > 0) {
    switch_stmt->cases = malloc(case_count * sizeof(ASTNode *));
    if (!switch_stmt->cases) {
      free(switch_stmt);
      free(node);
      return NULL;
    }
    for (size_t i = 0; i < case_count; i++) {
      switch_stmt->cases[i] = cases[i];
      if (cases[i])
        ast_add_child(node, cases[i]);
    }
  } else {
    switch_stmt->cases = NULL;
  }

  switch_stmt->expression = expression;
  if (expression)
    ast_add_child(node, expression);

  node->data = switch_stmt;
  return node;
}

ASTNode *ast_create_quiesce_statement(SourceLocation location) {
  return ast_create_node(AST_QUIESCE_STATEMENT, location);
}

ASTNode *ast_create_fallthrough_statement(SourceLocation location) {
  return ast_create_node(AST_FALLTHROUGH_STATEMENT, location);
}

ASTNode *ast_create_break_statement(SourceLocation location) {
  return ast_create_labeled_break_statement(NULL, location);
}

ASTNode *ast_create_continue_statement(SourceLocation location) {
  return ast_create_labeled_continue_statement(NULL, location);
}

ASTNode *ast_create_labeled_break_statement(const char *label,
                                            SourceLocation location) {
  ASTNode *node = ast_create_node(AST_BREAK_STATEMENT, location);
  if (!node)
    return NULL;
  LoopControlStatement *data = malloc(sizeof(LoopControlStatement));
  if (!data) {
    free(node);
    return NULL;
  }
  data->target_label = ast_copy_string(label);
  node->data = data;
  return node;
}

ASTNode *ast_create_labeled_continue_statement(const char *label,
                                               SourceLocation location) {
  ASTNode *node = ast_create_node(AST_CONTINUE_STATEMENT, location);
  if (!node)
    return NULL;
  LoopControlStatement *data = malloc(sizeof(LoopControlStatement));
  if (!data) {
    free(node);
    return NULL;
  }
  data->target_label = ast_copy_string(label);
  node->data = data;
  return node;
}

ASTNode *ast_create_defer_statement(ASTNode *statement,
                                    SourceLocation location) {
  ASTNode *node = ast_create_node(AST_DEFER_STATEMENT, location);
  if (!node) {
    return NULL;
  }

  DeferStatement *defer_stmt = malloc(sizeof(DeferStatement));
  if (!defer_stmt) {
    free(node);
    return NULL;
  }
  defer_stmt->statement = statement;
  node->data = defer_stmt;
  if (statement) {
    ast_add_child(node, statement);
  }
  return node;
}

ASTNode *ast_create_cast_expression(const char *type_name, ASTNode *operand,
                                    SourceLocation location) {
  ASTNode *node = ast_create_node(AST_CAST_EXPRESSION, location);
  if (!node)
    return NULL;

  CastExpression *cast_expr = malloc(sizeof(CastExpression));
  if (!cast_expr) {
    free(node);
    return NULL;
  }

  cast_expr->type_name = ast_intern_string(type_name);
  cast_expr->operand = operand;
  node->data = cast_expr;

  if (operand) {
    ast_add_child(node, operand);
  }

  return node;
}

ASTNode *ast_create_closure_adapt(ASTNode *inner, const char *ctor_name,
                                  char **param_types, size_t param_count,
                                  const char *return_type,
                                  SourceLocation location) {
  ASTNode *node = ast_create_node(AST_CLOSURE_ADAPT_EXPRESSION, location);
  if (!node)
    return NULL;

  ClosureAdapt *adapt = malloc(sizeof(ClosureAdapt));
  if (!adapt) {
    free(node);
    return NULL;
  }

  adapt->ctor_name = ast_intern_string(ctor_name);
  adapt->return_type = ast_intern_string(return_type);
  adapt->param_count = param_count;
  adapt->param_types = ast_copy_string_array(param_types, param_count);
  adapt->inner = inner;
  node->data = adapt;

  if (inner) {
    ast_add_child(node, inner);
  }

  return node;
}
