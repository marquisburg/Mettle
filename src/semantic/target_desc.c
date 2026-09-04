#include "target_desc.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *string_of(const ASTNode *node) {
  if (!node || node->type != AST_STRING_LITERAL || !node->data) {
    return NULL;
  }
  return ((const StringLiteral *)node->data)->value;
}

static int int_of(const ASTNode *node, long long *out) {
  if (!node) {
    return 0;
  }
  if (node->type == AST_NUMBER_LITERAL && node->data) {
    const NumberLiteral *literal = (const NumberLiteral *)node->data;
    if (literal->is_float) {
      return 0;
    }
    *out = literal->int_value;
    return 1;
  }
  if (node->type == AST_UNARY_EXPRESSION && node->data) {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    long long inner = 0;
    if (unary->operator && strcmp(unary->operator, "-") == 0 &&
        int_of(unary->operand, &inner)) {
      *out = -inner;
      return 1;
    }
  }
  return 0;
}

static int bool_of(const ASTNode *node, int *out) {
  const char *name;
  long long number = 0;
  if (int_of(node, &number)) {
    *out = number != 0;
    return 1;
  }
  if (!node || node->type != AST_IDENTIFIER || !node->data) {
    return 0;
  }
  name = ((const Identifier *)node->data)->name;
  if (name && strcmp(name, "true") == 0) {
    *out = 1;
    return 1;
  }
  if (name && strcmp(name, "false") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static int copy_string(char *slot, size_t capacity, const ASTNode *node,
                       const char *field, char *error, size_t error_size) {
  const char *text = string_of(node);
  if (!text) {
    snprintf(error, error_size, "`%s` must be a string literal", field);
    return 0;
  }
  if (strlen(text) >= capacity) {
    snprintf(error, error_size, "`%s` is longer than %zu characters", field,
             capacity - 1);
    return 0;
  }
  snprintf(slot, capacity, "%s", text);
  return 1;
}

static const AggregateLiteral *array_of(const ASTNode *node) {
  if (!node || node->type != AST_AGGREGATE_LITERAL || !node->data ||
      ((const AggregateLiteral *)node->data)->is_struct) {
    return NULL;
  }
  return (const AggregateLiteral *)node->data;
}

static int copy_names(char (*slots)[8], size_t *count, size_t capacity,
                      const ASTNode *node, const char *field, char *error,
                      size_t error_size) {
  const AggregateLiteral *literal = array_of(node);
  size_t i;
  *count = 0;
  if (!literal) {
    snprintf(error, error_size, "`%s` must be an array of register names",
             field);
    return 0;
  }
  if (literal->element_count > capacity) {
    snprintf(error, error_size, "`%s` lists more than %zu registers", field,
             capacity);
    return 0;
  }
  for (i = 0; i < literal->element_count; i++) {
    if (!copy_string(slots[i], 8, literal->elements[i], field, error,
                     error_size)) {
      return 0;
    }
    (*count)++;
  }
  return 1;
}

static int copy_wide_names(char (*slots)[16], size_t *count, size_t capacity,
                           const ASTNode *node, const char *field, char *error,
                           size_t error_size) {
  const AggregateLiteral *literal = array_of(node);
  size_t i;
  *count = 0;
  if (!literal) {
    snprintf(error, error_size, "`%s` must be an array of names", field);
    return 0;
  }
  if (literal->element_count > capacity) {
    snprintf(error, error_size, "`%s` lists more than %zu names", field,
             capacity);
    return 0;
  }
  for (i = 0; i < literal->element_count; i++) {
    if (!copy_string(slots[i], 16, literal->elements[i], field, error,
                     error_size)) {
      return 0;
    }
    (*count)++;
  }
  return 1;
}

static int read_fields(const AggregateLiteral *literal,
                       MtlcTargetDescription *out, char *error,
                       size_t error_size) {
  size_t i;
  int seen_name = 0;
  int seen_arch = 0;
  for (i = 0; i < literal->element_count; i++) {
    const char *field =
        literal->field_names ? literal->field_names[i] : NULL;
    ASTNode *value = literal->elements[i];
    long long number = 0;
    if (!field) {
      snprintf(error, error_size, "every element of a TargetDesc names a field");
      return 0;
    }
    if (strcmp(field, "name") == 0) {
      if (!copy_string(out->name, sizeof(out->name), value, field, error,
                       error_size)) {
        return 0;
      }
      seen_name = 1;
    } else if (strcmp(field, "arch") == 0) {
      if (!copy_string(out->arch, sizeof(out->arch), value, field, error,
                       error_size)) {
        return 0;
      }
      seen_arch = 1;
    } else if (strcmp(field, "os") == 0) {
      if (!copy_string(out->os, sizeof(out->os), value, field, error,
                       error_size)) {
        return 0;
      }
    } else if (strcmp(field, "format") == 0) {
      if (!copy_string(out->format, sizeof(out->format), value, field, error,
                       error_size)) {
        return 0;
      }
    } else if (strcmp(field, "indirect_return") == 0) {
      if (!copy_string(out->indirect_return, sizeof(out->indirect_return),
                       value, field, error, error_size)) {
        return 0;
      }
    } else if (strcmp(field, "int_args") == 0) {
      if (!copy_names(out->int_args, &out->int_arg_count,
                      MTLC_TARGET_DESC_MAX_REGS, value, field, error,
                      error_size)) {
        return 0;
      }
    } else if (strcmp(field, "float_args") == 0) {
      if (!copy_names(out->float_args, &out->float_arg_count,
                      MTLC_TARGET_DESC_MAX_REGS, value, field, error,
                      error_size)) {
        return 0;
      }
    } else if (strcmp(field, "address_spaces") == 0) {
      if (!copy_wide_names(out->address_spaces, &out->address_space_count,
                           MTLC_TARGET_DESC_MAX_NAMES, value, field, error,
                           error_size)) {
        return 0;
      }
    } else if (strcmp(field, "separate_classes") == 0) {
      if (!bool_of(value, &out->separate_classes)) {
        snprintf(error, error_size, "`separate_classes` must be true or false");
        return 0;
      }
    } else if (strcmp(field, "widths") == 0) {
      const AggregateLiteral *widths = array_of(value);
      size_t w;
      out->width_count = 0;
      if (!widths) {
        snprintf(error, error_size, "`widths` must be an array of integers");
        return 0;
      }
      if (widths->element_count > MTLC_TARGET_DESC_MAX_NAMES) {
        snprintf(error, error_size, "`widths` lists more than %d widths",
                 MTLC_TARGET_DESC_MAX_NAMES);
        return 0;
      }
      for (w = 0; w < widths->element_count; w++) {
        if (!int_of(widths->elements[w], &number)) {
          snprintf(error, error_size, "every width must be an integer literal");
          return 0;
        }
        out->widths[out->width_count++] = (int)number;
      }
    } else if (strncmp(field, "cost_", 5) == 0) {
      static const struct {
        const char *name;
        size_t offset;
      } costs[] = {
          {"cost_op", offsetof(MtlcTargetDescription, cost_op)},
          {"cost_load", offsetof(MtlcTargetDescription, cost_load)},
          {"cost_store", offsetof(MtlcTargetDescription, cost_store)},
          {"cost_branch", offsetof(MtlcTargetDescription, cost_branch)},
          {"cost_multiply", offsetof(MtlcTargetDescription, cost_multiply)},
          {"cost_multiply_float",
           offsetof(MtlcTargetDescription, cost_multiply_float)},
          {"cost_divide", offsetof(MtlcTargetDescription, cost_divide)},
          {"cost_divide_float",
           offsetof(MtlcTargetDescription, cost_divide_float)},
          {"cost_call", offsetof(MtlcTargetDescription, cost_call)},
          {"cost_allocate", offsetof(MtlcTargetDescription, cost_allocate)},
      };
      size_t c;
      if (!int_of(value, &number)) {
        snprintf(error, error_size, "`%s` must be an integer literal", field);
        return 0;
      }
      for (c = 0; c < sizeof(costs) / sizeof(costs[0]); c++) {
        if (strcmp(field, costs[c].name) == 0) {
          *(int *)((char *)out + costs[c].offset) = (int)number;
          break;
        }
      }
      if (c == sizeof(costs) / sizeof(costs[0])) {
        snprintf(error, error_size,
                 "`%s` is not a cost this machine has; the costs are cost_op, "
                 "cost_load, cost_store, cost_branch, cost_multiply, "
                 "cost_multiply_float, cost_divide, cost_divide_float, "
                 "cost_call and cost_allocate",
                 field);
        return 0;
      }
    } else if (strcmp(field, "pointer_bits") == 0 ||
               strcmp(field, "stack_alignment") == 0 ||
               strcmp(field, "shadow_space") == 0 ||
               strcmp(field, "red_zone") == 0 ||
               strcmp(field, "vector_width") == 0) {
      if (!int_of(value, &number)) {
        snprintf(error, error_size, "`%s` must be an integer literal", field);
        return 0;
      }
      if (strcmp(field, "pointer_bits") == 0) {
        out->pointer_bits = (int)number;
      } else if (strcmp(field, "stack_alignment") == 0) {
        out->stack_alignment = (int)number;
      } else if (strcmp(field, "shadow_space") == 0) {
        out->shadow_space = (int)number;
      } else if (strcmp(field, "red_zone") == 0) {
        out->red_zone = (int)number;
      } else {
        out->vector_width = (int)number;
      }
    } else {
      snprintf(error, error_size, "TargetDesc has no field `%s`", field);
      return 0;
    }
  }
  if (!seen_name || !seen_arch) {
    snprintf(error, error_size, "a target description names at least `name` "
                                "and `arch`");
    return 0;
  }
  return 1;
}

static int names_target_desc(const char *type_name) {
  size_t length = type_name ? strlen(type_name) : 0;
  const char *tail;
  if (length < 10) {
    return 0;
  }
  tail = type_name + length - 10;
  return strcmp(tail, "TargetDesc") == 0 &&
         (length == 10 || tail[-1] == '.');
}

int target_desc_read(ASTNode *program, MtlcTargetDescription *out,
                     char *error, size_t error_size) {
  Program *prog;
  size_t i;
  int imports_target = 0;
  if (!program || program->type != AST_PROGRAM || !out || !error) {
    return 0;
  }
  prog = (Program *)program->data;
  if (!prog) {
    return 0;
  }
  for (i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_IMPORT && decl->data) {
      const ImportDeclaration *import = (const ImportDeclaration *)decl->data;
      if (import->module_name &&
          strcmp(import->module_name, "std/target") == 0) {
        imports_target = 1;
      }
    }
  }
  for (i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    VarDeclaration *var;
    if (!decl || decl->type != AST_VAR_DECLARATION || !decl->data) {
      continue;
    }
    var = (VarDeclaration *)decl->data;
    if (!var->is_const || !names_target_desc(var->type_name)) {
      continue;
    }
    if (!imports_target) {
      snprintf(error, error_size,
               "the description declares a TargetDesc without importing "
               "\"std/target\", where the record is declared");
      return 0;
    }
    if (!var->initializer || var->initializer->type != AST_AGGREGATE_LITERAL ||
        !var->initializer->data ||
        !((AggregateLiteral *)var->initializer->data)->is_struct) {
      snprintf(error, error_size,
               "`%s` must be initialized with a `{ field: value }` literal",
               var->name ? var->name : "?");
      return 0;
    }
    memset(out, 0, sizeof(*out));
    return read_fields((AggregateLiteral *)var->initializer->data, out, error,
                       error_size);
  }
  snprintf(error, error_size,
           "no `const NAME: TargetDesc = { ... }` in the description; import "
           "\"std/target\" and declare one");
  return 0;
}
