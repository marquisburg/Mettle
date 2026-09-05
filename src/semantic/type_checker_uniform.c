/* Uniformity: which values are the same for every work item of a group.
 *
 * A group collective is only meaningful where every work item of the group
 * reaches it, and that is decided by the values the control flow tests. So the
 * question "is this value the same in every lane" has to be answerable in the
 * frontend, before any of it reaches IR: a declared type says a value is
 * uniform, and this is what discharges the claim.
 *
 * A value is uniform when it depends on no work-item index, on no load from
 * memory, and on no value that is not itself uniform. A kernel's parameters
 * are uniform because the launch gave every work item the same ones; `block.x`
 * and the launch geometry are uniform; `thread.x` and the subgroup lane are
 * not. The answer is computed to a fixed point through calls, which is what
 * lets a helper stay reusable rather than being uniform by decree.
 */
#include "type_checker_internal.h"
#include <string.h>

typedef struct {
  TypeChecker *checker;
  const char *why;          /* the term that made the answer no */
  const char *visiting[16]; /* locals already on the path, to stop a cycle */
  size_t visiting_count;
  int depth;
} UniformContext;

static int uniform_expression(UniformContext *context, ASTNode *expression);

static ASTNode *uniform_function_body(TypeChecker *checker, const char *name);

/* A function body that reads a work-item index, or memory, cannot promise its
   result is the same in every lane whatever it was handed. Calls are followed:
   a helper that hides the index one level down hides nothing. The device call
   graph is a DAG of direct calls, and the walk carries the names already on
   its path so a shared helper is not a cycle. */
static int uniform_body_reads_a_varying_thing(UniformContext *context,
                                              ASTNode *node, int depth) {
  if (!node || depth > 64) {
    return depth > 64;
  }
  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && call->function_name) {
      if (strncmp(call->function_name, "gpu_tid_", 8) == 0 ||
          strcmp(call->function_name, "subgroup_local_id") == 0) {
        return 1;
      }
      if (!call->is_gpu_index) {
        ASTNode *callee =
            uniform_function_body(context->checker, call->function_name);
        int already = 0;
        for (size_t i = 0; i < context->visiting_count; i++) {
          if (context->visiting[i] == call->function_name) {
            already = 1;
          }
        }
        if (!callee) {
          /* Nothing to read: an extern or a name from another module. It is
             not known to be the same in every work item. */
          return 1;
        }
        if (!already && context->visiting_count < 16) {
          int varying;
          context->visiting[context->visiting_count++] = call->function_name;
          varying = uniform_body_reads_a_varying_thing(context, callee,
                                                       depth + 1);
          context->visiting_count--;
          if (varying) {
            return 1;
          }
        }
      }
    }
  }
  if (node->type == AST_INDEX_EXPRESSION) {
    return 1;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (uniform_body_reads_a_varying_thing(context, node->children[i],
                                           depth + 1)) {
      return 1;
    }
  }
  return 0;
}

/* The body of a function by name, out of the module being checked. A device
   call graph is a DAG of direct calls, so a name is enough to find it. */
static ASTNode *uniform_function_body(TypeChecker *checker, const char *name) {
  ASTNode *module = checker->module_program;
  Program *program = module && module->data ? (Program *)module->data : NULL;
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    FunctionDeclaration *function =
        declaration && declaration->type == AST_FUNCTION_DECLARATION
            ? (FunctionDeclaration *)declaration->data
            : NULL;
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function->body;
    }
  }
  return NULL;
}

/* Every write to a local in the function being checked. A local is uniform
   only when its declaration and every assignment to it are. */
static int uniform_local_writes(UniformContext *context, ASTNode *node,
                                const char *name, int depth) {
  if (!node || depth > 128) {
    return depth <= 128;
  }
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *declaration = (VarDeclaration *)node->data;
    if (declaration && declaration->name && name &&
        strcmp(declaration->name, name) == 0 && declaration->initializer &&
        !uniform_expression(context, declaration->initializer)) {
      return 0;
    }
  }
  if (node->type == AST_ASSIGNMENT) {
    Assignment *assignment = (Assignment *)node->data;
    if (assignment && assignment->variable_name && name &&
        strcmp(assignment->variable_name, name) == 0 && assignment->value &&
        !uniform_expression(context, assignment->value)) {
      return 0;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!uniform_local_writes(context, node->children[i], name, depth + 1)) {
      return 0;
    }
  }
  return 1;
}

static int uniform_identifier(UniformContext *context, ASTNode *expression) {
  Identifier *identifier = (Identifier *)expression->data;
  TypeChecker *checker = context->checker;
  Symbol *symbol = identifier && identifier->name
                       ? symbol_table_lookup(checker->symbol_table,
                                             identifier->name)
                       : NULL;
  ASTNode *owner_node = checker->current_function_decl;
  FunctionDeclaration *owner =
      owner_node && owner_node->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)owner_node->data
          : NULL;
  if (!symbol) {
    context->why = identifier && identifier->name ? identifier->name
                                                  : "an unknown name";
    return 0;
  }
  /* A declared type may already say it. */
  if (symbol->type && symbol->type->refine_uniform) {
    return 1;
  }
  if (symbol->kind == SYMBOL_PARAMETER) {
    /* A kernel's parameters come from the launch, so every work item has the
       same ones. A helper's do not, unless their type says so. */
    if (owner && owner->is_kernel) {
      return 1;
    }
    context->why = identifier->name;
    return 0;
  }
  if (symbol->kind == SYMBOL_CONSTANT) {
    return 1;
  }
  if (symbol->kind == SYMBOL_VARIABLE && symbol->scope &&
      symbol->scope->type == SCOPE_GLOBAL) {
    /* A module-scope binding is device memory, and reading one is a load like
       any other. Only a `const`, which is laid out before anything runs, is
       the same in every work item by construction. */
    context->why = identifier->name;
    return 0;
  }
  if (symbol->kind == SYMBOL_VARIABLE || symbol->kind == SYMBOL_CONSTANT) {
    for (size_t i = 0; i < context->visiting_count; i++) {
      if (context->visiting[i] == identifier->name) {
        return 1; /* already on the path: it constrains nothing new */
      }
    }
    if (!owner || !owner->body || context->visiting_count >= 16) {
      context->why = identifier->name;
      return 0;
    }
    context->visiting[context->visiting_count++] = identifier->name;
    {
      int uniform =
          uniform_local_writes(context, owner->body, identifier->name, 0);
      context->visiting_count--;
      if (!uniform && !context->why) {
        context->why = identifier->name;
      }
      return uniform;
    }
  }
  context->why = identifier->name;
  return 0;
}

static int uniform_call(UniformContext *context, ASTNode *expression) {
  CallExpression *call = (CallExpression *)expression->data;
  ASTNode *body;
  if (!call || !call->function_name) {
    context->why = "an indirect call";
    return 0;
  }
  if (call->is_gpu_index) {
    if (strncmp(call->function_name, "gpu_tid_", 8) == 0) {
      static const char *const axis[3] = {"thread.x", "thread.y", "thread.z"};
      char last = call->function_name[8];
      context->why = last == 'y' ? axis[1] : last == 'z' ? axis[2] : axis[0];
      return 0;
    }
    return 1;
  }
  if (strcmp(call->function_name, "subgroup_local_id") == 0) {
    context->why = "subgroup_local_id()";
    return 0;
  }
  if (strcmp(call->function_name, "subgroup_size") == 0) {
    return 1;
  }
  for (size_t i = 0; i < call->argument_count; i++) {
    if (!uniform_expression(context, call->arguments[i])) {
      return 0;
    }
  }
  body = uniform_function_body(context->checker, call->function_name);
  if (!body) {
    context->why = call->function_name;
    return 0;
  }
  if (uniform_body_reads_a_varying_thing(context, body, 0)) {
    context->why = call->function_name;
    return 0;
  }
  return 1;
}

static int uniform_expression(UniformContext *context, ASTNode *expression) {
  if (!expression || context->depth > 64) {
    return expression == NULL;
  }
  context->depth++;
  switch (expression->type) {
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
    context->depth--;
    return 1;
  case AST_IDENTIFIER: {
    int uniform = uniform_identifier(context, expression);
    context->depth--;
    return uniform;
  }
  case AST_FUNCTION_CALL: {
    int uniform = uniform_call(context, expression);
    context->depth--;
    return uniform;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    int uniform = binary && uniform_expression(context, binary->left) &&
                  uniform_expression(context, binary->right);
    context->depth--;
    return uniform;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    int uniform = unary && uniform_expression(context, unary->operand);
    context->depth--;
    return uniform;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    int uniform = cast && uniform_expression(context, cast->operand);
    context->depth--;
    return uniform;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expression->data;
    int uniform = member && uniform_expression(context, member->object);
    context->depth--;
    return uniform;
  }
  case AST_INDEX_EXPRESSION:
    context->why = "a load from memory";
    context->depth--;
    return 0;
  default:
    context->why = "an expression whose value the compiler cannot follow";
    context->depth--;
    return 0;
  }
}

int type_checker_expression_is_uniform(TypeChecker *checker,
                                       ASTNode *expression,
                                       const char **why) {
  UniformContext context;
  int uniform;
  if (why) {
    *why = NULL;
  }
  if (!checker || !expression) {
    return 0;
  }
  memset(&context, 0, sizeof(context));
  context.checker = checker;
  uniform = uniform_expression(&context, expression);
  if (!uniform && why) {
    *why = context.why ? context.why : "a value that varies by work item";
  }
  return uniform;
}
