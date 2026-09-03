#include "ast_dump.h"
#include <stdio.h>

static const char *const node_names[] = {
    [AST_PROGRAM] = "AST_PROGRAM",
    [AST_IMPORT] = "AST_IMPORT",
    [AST_IMPORT_STR] = "AST_IMPORT_STR",
    [AST_VAR_DECLARATION] = "AST_VAR_DECLARATION",
    [AST_FUNCTION_DECLARATION] = "AST_FUNCTION_DECLARATION",
    [AST_STRUCT_DECLARATION] = "AST_STRUCT_DECLARATION",
    [AST_ENUM_DECLARATION] = "AST_ENUM_DECLARATION",
    [AST_TYPE_DECLARATION] = "AST_TYPE_DECLARATION",
    [AST_TRAIT_DECLARATION] = "AST_TRAIT_DECLARATION",
    [AST_EFFECT_DECLARATION] = "AST_EFFECT_DECLARATION",
    [AST_IMPL_DECLARATION] = "AST_IMPL_DECLARATION",
    [AST_METHOD_DECLARATION] = "AST_METHOD_DECLARATION",
    [AST_ASSIGNMENT] = "AST_ASSIGNMENT",
    [AST_FUNCTION_CALL] = "AST_FUNCTION_CALL",
    [AST_FUNC_PTR_CALL] = "AST_FUNC_PTR_CALL",
    [AST_GPU_LAUNCH] = "AST_GPU_LAUNCH",
    [AST_RETURN_STATEMENT] = "AST_RETURN_STATEMENT",
    [AST_IF_STATEMENT] = "AST_IF_STATEMENT",
    [AST_WHILE_STATEMENT] = "AST_WHILE_STATEMENT",
    [AST_FOR_STATEMENT] = "AST_FOR_STATEMENT",
    [AST_SWITCH_STATEMENT] = "AST_SWITCH_STATEMENT",
    [AST_CASE_CLAUSE] = "AST_CASE_CLAUSE",
    [AST_MATCH_STATEMENT] = "AST_MATCH_STATEMENT",
    [AST_BREAK_STATEMENT] = "AST_BREAK_STATEMENT",
    [AST_CONTINUE_STATEMENT] = "AST_CONTINUE_STATEMENT",
    [AST_QUIESCE_STATEMENT] = "AST_QUIESCE_STATEMENT",
    [AST_FALLTHROUGH_STATEMENT] = "AST_FALLTHROUGH_STATEMENT",
    [AST_DEFER_STATEMENT] = "AST_DEFER_STATEMENT",
    [AST_ERRDEFER_STATEMENT] = "AST_ERRDEFER_STATEMENT",
    [AST_INLINE_ASM] = "AST_INLINE_ASM",
    [AST_IDENTIFIER] = "AST_IDENTIFIER",
    [AST_NUMBER_LITERAL] = "AST_NUMBER_LITERAL",
    [AST_STRING_LITERAL] = "AST_STRING_LITERAL",
    [AST_BINARY_EXPRESSION] = "AST_BINARY_EXPRESSION",
    [AST_UNARY_EXPRESSION] = "AST_UNARY_EXPRESSION",
    [AST_MEMBER_ACCESS] = "AST_MEMBER_ACCESS",
    [AST_INDEX_EXPRESSION] = "AST_INDEX_EXPRESSION",
    [AST_NEW_EXPRESSION] = "AST_NEW_EXPRESSION",
    [AST_CAST_EXPRESSION] = "AST_CAST_EXPRESSION",
    [AST_LAMBDA_EXPRESSION] = "AST_LAMBDA_EXPRESSION",
    [AST_CLOSURE_ADAPT_EXPRESSION] = "AST_CLOSURE_ADAPT_EXPRESSION",
    [AST_BARRIER_STATEMENT] = "AST_BARRIER_STATEMENT",
    [AST_AGGREGATE_LITERAL] = "AST_AGGREGATE_LITERAL",
    [AST_COMPTIME_FOR] = "AST_COMPTIME_FOR",
};

static void dump_quoted(FILE *out, const char *value) {
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)(value ? value : "");
       *p; p++) {
    switch (*p) {
    case '"':
      fputs("\\\"", out);
      break;
    case '\\':
      fputs("\\\\", out);
      break;
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    default:
      if (*p >= 0x20 && *p <= 0x7e) {
        fputc(*p, out);
      } else {
        fprintf(out, "\\x%02x", *p);
      }
    }
  }
  fputc('"', out);
}

static void dump_text(FILE *out, const char *key, const char *value) {
  if (value) {
    fprintf(out, " %s=", key);
    dump_quoted(out, value);
  }
}

static void dump_pairs(FILE *out, const char *key, char *const *names,
                       char *const *types, size_t count) {
  fprintf(out, " %s=[", key);
  for (size_t i = 0; i < count; i++) {
    if (i) {
      fputs(", ", out);
    }
    fputc('[', out);
    dump_quoted(out, names && names[i] ? names[i] : "");
    fputs(", ", out);
    dump_quoted(out, types && types[i] ? types[i] : "");
    fputc(']', out);
  }
  fputc(']', out);
}

static void dump_function_data(FILE *out, const FunctionDeclaration *decl) {
  if (!decl) {
    return;
  }
  dump_text(out, "name", decl->name);
  dump_pairs(out, "parameters", decl->parameter_names, decl->parameter_types,
             decl->parameter_count);
  dump_text(out, "return_type", decl->return_type);
}

static void dump_node_data(FILE *out, const ASTNode *node) {
  switch (node->type) {
  case AST_PROGRAM: {
    const Program *program = node->data;
    fprintf(out, " declarations=%zu",
            program ? program->declaration_count : node->child_count);
    break;
  }
  case AST_IMPORT: {
    const ImportDeclaration *decl = node->data;
    dump_text(out, "module", decl ? decl->module_name : NULL);
    dump_text(out, "alias", decl ? decl->namespace_alias : NULL);
    break;
  }
  case AST_IMPORT_STR: {
    const ImportStrExpression *expr = node->data;
    dump_text(out, "path", expr ? expr->file_path : NULL);
    break;
  }
  case AST_VAR_DECLARATION: {
    const VarDeclaration *decl = node->data;
    dump_text(out, "name", decl ? decl->name : NULL);
    dump_text(out, "type", decl ? decl->type_name : NULL);
    break;
  }
  case AST_FUNCTION_DECLARATION:
  case AST_METHOD_DECLARATION:
  case AST_LAMBDA_EXPRESSION:
    dump_function_data(out, node->data);
    break;
  case AST_STRUCT_DECLARATION: {
    const StructDeclaration *decl = node->data;
    dump_text(out, "name", decl ? decl->name : NULL);
    if (decl) {
      dump_pairs(out, "fields", decl->field_names, decl->field_types,
                 decl->field_count);
    }
    break;
  }
  case AST_TYPE_DECLARATION: {
    const TypeDeclaration *decl = node->data;
    dump_text(out, "name", decl ? decl->name : NULL);
    dump_text(out, "base", decl ? decl->base_type : NULL);
    break;
  }
  case AST_ENUM_DECLARATION: {
    const EnumDeclaration *decl = node->data;
    dump_text(out, "name", decl ? decl->name : NULL);
    if (decl) {
      fprintf(out, " variants=%zu", decl->variant_count);
    }
    break;
  }
  case AST_TRAIT_DECLARATION: {
    const TraitDeclaration *decl = node->data;
    dump_text(out, "name", decl ? decl->name : NULL);
    break;
  }
  case AST_EFFECT_DECLARATION: {
    const EffectDeclaration *decl = node->data;
    dump_text(out, "name", decl ? decl->name : NULL);
    break;
  }
  case AST_IMPL_DECLARATION: {
    const ImplDeclaration *decl = node->data;
    dump_text(out, "trait", decl ? decl->trait_name : NULL);
    dump_text(out, "type", decl ? decl->for_type_name : NULL);
    break;
  }
  case AST_ASSIGNMENT: {
    const Assignment *assignment = node->data;
    dump_text(out, "name", assignment ? assignment->variable_name : NULL);
    break;
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = node->data;
    dump_text(out, "function", call ? call->function_name : NULL);
    break;
  }
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT: {
    const LoopControlStatement *control = node->data;
    dump_text(out, "target", control ? control->target_label : NULL);
    break;
  }
  case AST_INLINE_ASM: {
    const InlineAsm *statement = node->data;
    dump_text(out, "code", statement ? statement->assembly_code : NULL);
    break;
  }
  case AST_IDENTIFIER: {
    const Identifier *identifier = node->data;
    dump_text(out, "name", identifier ? identifier->name : NULL);
    break;
  }
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *literal = node->data;
    if (literal && literal->is_float) {
      fprintf(out, " value=%.17g", literal->float_value);
    } else if (literal) {
      fprintf(out, " value=%lld radix=%u", literal->int_value,
              (unsigned)literal->int_radix);
    }
    break;
  }
  case AST_STRING_LITERAL: {
    const StringLiteral *literal = node->data;
    dump_text(out, "value", literal ? literal->value : NULL);
    break;
  }
  case AST_BINARY_EXPRESSION: {
    const BinaryExpression *expr = node->data;
    dump_text(out, "operator", expr ? expr->operator : NULL);
    break;
  }
  case AST_UNARY_EXPRESSION: {
    const UnaryExpression *expr = node->data;
    dump_text(out, "operator", expr ? expr->operator : NULL);
    break;
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *expr = node->data;
    dump_text(out, "member", expr ? expr->member : NULL);
    break;
  }
  case AST_NEW_EXPRESSION: {
    const NewExpression *expr = node->data;
    dump_text(out, "type", expr ? expr->type_name : NULL);
    break;
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *expr = node->data;
    dump_text(out, "type", expr ? expr->type_name : NULL);
    break;
  }
  case AST_CLOSURE_ADAPT_EXPRESSION: {
    const ClosureAdapt *expr = node->data;
    dump_text(out, "constructor", expr ? expr->ctor_name : NULL);
    break;
  }
  case AST_AGGREGATE_LITERAL: {
    const AggregateLiteral *literal = node->data;
    if (literal) {
      fprintf(out, " kind=%s elements=%zu",
              literal->is_struct ? "struct" : "array",
              literal->element_count);
    }
    break;
  }
  case AST_COMPTIME_FOR: {
    const ComptimeForStatement *statement = node->data;
    dump_text(out, "binding", statement ? statement->binding_name : NULL);
    break;
  }
  default:
    break;
  }
}

static void dump_node(FILE *out, const ASTNode *current, size_t depth) {
  if (!current) {
    return;
  }
  for (size_t i = 0; i < depth; i++) {
    fputs("  ", out);
  }
  size_t name_count = sizeof(node_names) / sizeof(node_names[0]);
  const char *name = (size_t)current->type < name_count
                         ? node_names[current->type]
                         : NULL;
  fputs(name ? name : "AST_UNKNOWN", out);
  dump_node_data(out, current);
  dump_text(out, "file", current->location.filename);
  fprintf(out, " line=%zu column=%zu children=%zu\n", current->location.line,
          current->location.column, current->child_count);
  for (size_t i = 0; i < current->child_count; i++) {
    dump_node(out, current->children[i], depth + 1);
  }
}

int ast_dump_program(FILE *out, const ASTNode *program) {
  if (!out || !program) {
    return 0;
  }
  dump_node(out, program, 0);
  return !ferror(out);
}
