/* AST -> Mettle source.
 *
 * This exists for `mettle expand`, which is the reason it prints source rather
 * than a debug dump: generated code that cannot be read cannot be reviewed or
 * diffed, and every bug inside it is a bug in a program nobody has seen. The
 * output is meant to be pasted back into a file and compiled.
 *
 * Where a node has no faithful source spelling, the printer says so inline
 * rather than guessing. A printer that silently misrepresents generated code is
 * worse than no printer, because it is believed. */
#include "ast_print.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE *out;
  int depth;
  size_t unprintable;
  AstPrintAnnotator annotate;
  void *context;
} AstPrinter;

static void print_statement(AstPrinter *printer, const ASTNode *node);
static void print_expression(AstPrinter *printer, const ASTNode *node);

static void print_indent(AstPrinter *printer) {
  for (int i = 0; i < printer->depth; i++) {
    fputs("    ", printer->out);
  }
}

/* Generated code says where it came from. Without this a reader sees a bare
 * block using a name that is declared nowhere in the output, because an
 * expansion's binding lives in a scope rather than in a node. */
static void print_provenance(AstPrinter *printer, const ASTNode *block) {
  const char *note =
      printer->annotate ? printer->annotate(printer->context, block) : NULL;
  if (!note) {
    return;
  }
  print_indent(printer);
  fprintf(printer->out, "// %s\n", note);
}

/* Naming a gap costs one line and keeps the output honest. */
static void print_unprintable(AstPrinter *printer, const char *what) {
  fprintf(printer->out, "/* <mettle expand: no source form for %s> */", what);
  printer->unprintable++;
}

static void print_string_literal(AstPrinter *printer, const char *value) {
  fputc('"', printer->out);
  for (const char *p = value ? value : ""; *p; p++) {
    switch (*p) {
    case '"':
      fputs("\\\"", printer->out);
      break;
    case '\\':
      fputs("\\\\", printer->out);
      break;
    case '\n':
      fputs("\\n", printer->out);
      break;
    case '\t':
      fputs("\\t", printer->out);
      break;
    case '\r':
      fputs("\\r", printer->out);
      break;
    default:
      fputc(*p, printer->out);
      break;
    }
  }
  fputc('"', printer->out);
}

static void print_expression(AstPrinter *printer, const ASTNode *node) {
  if (!node) {
    fputs("/* <null> */", printer->out);
    return;
  }

  switch (node->type) {
  case AST_IDENTIFIER: {
    const Identifier *id = (const Identifier *)node->data;
    fputs(id && id->name ? id->name : "<anonymous>", printer->out);
    break;
  }
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *literal = (const NumberLiteral *)node->data;
    if (!literal) {
      fputs("0", printer->out);
    } else if (literal->is_float) {
      fprintf(printer->out, "%g", literal->float_value);
    } else if (literal->int_radix == 16) {
      fprintf(printer->out, "0x%llx", (unsigned long long)literal->int_value);
    } else {
      fprintf(printer->out, "%lld", literal->int_value);
    }
    break;
  }
  case AST_STRING_LITERAL: {
    const StringLiteral *literal = (const StringLiteral *)node->data;
    print_string_literal(printer, literal ? literal->value : "");
    break;
  }
  case AST_BINARY_EXPRESSION: {
    const BinaryExpression *binary = (const BinaryExpression *)node->data;
    if (!binary) {
      print_unprintable(printer, "binary expression");
      break;
    }
    /* Fully parenthesized: the printer does not track precedence, and a
     * wrong-but-plausible reassociation in generated code would be exactly the
     * kind of silent misreport this file refuses to produce. */
    fputc('(', printer->out);
    print_expression(printer, binary->left);
    fprintf(printer->out, " %s ", binary->operator ? binary->operator : "?");
    print_expression(printer, binary->right);
    fputc(')', printer->out);
    break;
  }
  case AST_UNARY_EXPRESSION: {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    if (!unary) {
      print_unprintable(printer, "unary expression");
      break;
    }
    fputs(unary->operator ? unary->operator : "?", printer->out);
    fputc('(', printer->out);
    print_expression(printer, unary->operand);
    fputc(')', printer->out);
    break;
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *member = (const MemberAccess *)node->data;
    if (!member) {
      print_unprintable(printer, "member access");
      break;
    }
    print_expression(printer, member->object);
    fprintf(printer->out, ".%s", member->member ? member->member : "<field>");
    break;
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index =
        (const ArrayIndexExpression *)node->data;
    if (!index) {
      print_unprintable(printer, "index expression");
      break;
    }
    print_expression(printer, index->array);
    fputc('[', printer->out);
    print_expression(printer, index->index);
    fputc(']', printer->out);
    break;
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)node->data;
    if (!cast) {
      print_unprintable(printer, "cast");
      break;
    }
    fprintf(printer->out, "(%s)",
            cast->type_name ? cast->type_name : "<type>");
    print_expression(printer, cast->operand);
    break;
  }
  case AST_NEW_EXPRESSION: {
    const NewExpression *expr = (const NewExpression *)node->data;
    fprintf(printer->out, "new %s",
            expr && expr->type_name ? expr->type_name : "<type>");
    break;
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)node->data;
    if (!call) {
      print_unprintable(printer, "call");
      break;
    }
    if (call->object) {
      print_expression(printer, call->object);
      fputc('.', printer->out);
    }
    fputs(call->function_name ? call->function_name : "<callee>",
          printer->out);
    fputc('(', printer->out);
    for (size_t i = 0; i < call->argument_count; i++) {
      if (i) {
        fputs(", ", printer->out);
      }
      if (call->argument_names && call->argument_names[i]) {
        fprintf(printer->out, "%s: ", call->argument_names[i]);
      }
      print_expression(printer, call->arguments[i]);
    }
    fputc(')', printer->out);
    break;
  }
  case AST_AGGREGATE_LITERAL: {
    const AggregateLiteral *literal = (const AggregateLiteral *)node->data;
    if (!literal) {
      print_unprintable(printer, "aggregate literal");
      break;
    }
    fputs(literal->is_struct ? "{ " : "[", printer->out);
    for (size_t i = 0; i < literal->element_count; i++) {
      if (i) {
        fputs(", ", printer->out);
      }
      if (literal->is_struct && literal->field_names &&
          literal->field_names[i]) {
        fprintf(printer->out, "%s: ", literal->field_names[i]);
      }
      print_expression(printer, literal->elements[i]);
    }
    if (literal->repeat_count) {
      fputs("; ", printer->out);
      print_expression(printer, literal->repeat_count);
    }
    fputs(literal->is_struct ? " }" : "]", printer->out);
    break;
  }
  default:
    print_unprintable(printer, "this expression");
    break;
  }
}

static void print_block(AstPrinter *printer, const ASTNode *node) {
  if (!node || node->type != AST_PROGRAM) {
    fputs(" {\n", printer->out);
    printer->depth++;
    print_statement(printer, node);
    printer->depth--;
    print_indent(printer);
    fputs("}", printer->out);
    return;
  }
  fputs(" {\n", printer->out);
  printer->depth++;
  for (size_t i = 0; i < node->child_count; i++) {
    print_statement(printer, node->children[i]);
  }
  printer->depth--;
  print_indent(printer);
  fputs("}", printer->out);
}

static void print_loop_statement(AstPrinter *printer,
                                 const ASTNode *node) {
  switch (node->type) {
case AST_WHILE_STATEMENT: {
  const WhileStatement *loop = (const WhileStatement *)node->data;
  if (!loop) {
    break;
  }
  print_indent(printer);
  fputs("while (", printer->out);
  print_expression(printer, loop->condition);
  fputc(')', printer->out);
  print_block(printer, loop->body);
  fputc('\n', printer->out);
  break;
}

case AST_FOR_STATEMENT: {
  const ForStatement *loop = (const ForStatement *)node->data;
  if (!loop) {
    break;
  }
  print_indent(printer);
  fputs("for (", printer->out);
  if (loop->initializer) {
    fputs("/* init */ ", printer->out);
  }
  print_expression(printer, loop->condition);
  fputc(')', printer->out);
  print_block(printer, loop->body);
  fputc('\n', printer->out);
  break;
}

case AST_COMPTIME_FOR: {
  /* Only reachable when expansion did not run (a parse-only dump), since a
   * successful expand replaces the directive with its iterations. */
  const ComptimeForStatement *directive =
      (const ComptimeForStatement *)node->data;
  print_indent(printer);
  fprintf(printer->out, "comptime for %s in ",
          directive && directive->binding_name ? directive->binding_name
                                               : "<binding>");
  print_expression(printer, directive ? directive->sequence : NULL);
  print_block(printer, directive ? directive->body : NULL);
  fputc('\n', printer->out);
  break;
}
  default:
    break;
  }
}

static void print_statement(AstPrinter *printer, const ASTNode *node) {
  if (!node) {
    return;
  }

  switch (node->type) {
  case AST_PROGRAM:
    print_provenance(printer, node);
    print_indent(printer);
    fputs("{\n", printer->out);
    printer->depth++;
    for (size_t i = 0; i < node->child_count; i++) {
      print_statement(printer, node->children[i]);
    }
    printer->depth--;
    print_indent(printer);
    fputs("}\n", printer->out);
    break;

  case AST_VAR_DECLARATION: {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    if (!decl) {
      break;
    }
    print_indent(printer);
    fprintf(printer->out, "%s %s", decl->is_const ? "const" : "var",
            decl->name ? decl->name : "<name>");
    if (decl->type_name) {
      fprintf(printer->out, ": %s", decl->type_name);
    }
    if (decl->initializer) {
      fputs(" = ", printer->out);
      print_expression(printer, decl->initializer);
    }
    fputs(";\n", printer->out);
    break;
  }

  case AST_ASSIGNMENT: {
    const Assignment *assign = (const Assignment *)node->data;
    if (!assign) {
      break;
    }
    print_indent(printer);
    if (assign->target) {
      print_expression(printer, assign->target);
    } else {
      fputs(assign->variable_name ? assign->variable_name : "<target>",
            printer->out);
    }
    fputs(" = ", printer->out);
    print_expression(printer, assign->value);
    fputs(";\n", printer->out);
    break;
  }

  case AST_RETURN_STATEMENT: {
    const ReturnStatement *ret = (const ReturnStatement *)node->data;
    print_indent(printer);
    fputs("return", printer->out);
    if (ret && ret->value) {
      fputc(' ', printer->out);
      print_expression(printer, ret->value);
    }
    fputs(";\n", printer->out);
    break;
  }

  case AST_IF_STATEMENT: {
    const IfStatement *branch = (const IfStatement *)node->data;
    if (!branch) {
      break;
    }
    print_indent(printer);
    fputs("if (", printer->out);
    print_expression(printer, branch->condition);
    fputc(')', printer->out);
    print_block(printer, branch->then_branch);
    for (size_t i = 0; i < branch->else_if_count; i++) {
      fputs(" else if (", printer->out);
      print_expression(printer, branch->else_ifs[i].condition);
      fputc(')', printer->out);
      print_block(printer, branch->else_ifs[i].body);
    }
    if (branch->else_branch) {
      fputs(" else", printer->out);
      print_block(printer, branch->else_branch);
    }
    fputc('\n', printer->out);
    break;
  }

  case AST_WHILE_STATEMENT:
  case AST_FOR_STATEMENT:
  case AST_COMPTIME_FOR:
    print_loop_statement(printer, node);
    break;

  case AST_BREAK_STATEMENT:
    print_indent(printer);
    fputs("break;\n", printer->out);
    break;

  case AST_CONTINUE_STATEMENT:
    print_indent(printer);
    fputs("continue;\n", printer->out);
    break;

  case AST_FUNCTION_CALL:
    print_indent(printer);
    print_expression(printer, node);
    fputs(";\n", printer->out);
    break;

  case AST_QUIESCE_STATEMENT:
    print_indent(printer);
    fputs("quiesce;\n", printer->out);
    break;

  case AST_FALLTHROUGH_STATEMENT:
    print_indent(printer);
    fputs("fallthrough;\n", printer->out);
    break;

  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    const DeferStatement *defer = (const DeferStatement *)node->data;
    print_indent(printer);
    fputs(node->type == AST_DEFER_STATEMENT ? "defer " : "errdefer ",
          printer->out);
    if (defer && defer->statement) {
      print_expression(printer, defer->statement);
    }
    fputs(";\n", printer->out);
    break;
  }

  default:
    print_indent(printer);
    print_unprintable(printer, "this statement");
    fputc('\n', printer->out);
    break;
  }
}

static void print_declaration(AstPrinter *printer, const ASTNode *node) {
  if (!node) {
    return;
  }

  /* A module-scope expansion generates declarations rather than blocks, so the
   * provenance line belongs here too, and reads the same either way. */
  print_provenance(printer, node);

  switch (node->type) {
  case AST_STRUCT_DECLARATION: {
    const StructDeclaration *decl = (const StructDeclaration *)node->data;
    if (!decl) {
      break;
    }
    fprintf(printer->out, "struct %s {\n",
            decl->name ? decl->name : "<name>");
    for (size_t i = 0; i < decl->field_count; i++) {
      fprintf(printer->out, "    %s: %s;\n",
              decl->field_names ? decl->field_names[i] : "<field>",
              decl->field_types ? decl->field_types[i] : "<type>");
    }
    fputs("}\n\n", printer->out);
    break;
  }

  case AST_TYPE_DECLARATION: {
    const TypeDeclaration *decl = (const TypeDeclaration *)node->data;
    if (!decl) {
      break;
    }
    if (decl->is_exported) {
      fputs("export ", printer->out);
    }
    fprintf(printer->out, "type %s = %s", decl->name ? decl->name : "<name>",
            decl->base_type ? decl->base_type : "<base>");
    if (decl->predicate) {
      fputs(" where ", printer->out);
      print_expression(printer, decl->predicate);
    }
    fputs(";\n\n", printer->out);
    break;
  }
  case AST_ENUM_DECLARATION: {
    const EnumDeclaration *decl = (const EnumDeclaration *)node->data;
    if (!decl) {
      break;
    }
    fprintf(printer->out, "enum %s {\n", decl->name ? decl->name : "<name>");
    for (size_t i = 0; i < decl->variant_count; i++) {
      fprintf(printer->out, "    %s%s\n", decl->variants[i].name,
              i + 1 < decl->variant_count ? "," : "");
    }
    fputs("}\n\n", printer->out);
    break;
  }

  case AST_FUNCTION_DECLARATION: {
    const FunctionDeclaration *decl = (const FunctionDeclaration *)node->data;
    if (!decl) {
      break;
    }
    if (decl->is_inline) {
      fputs(decl->is_inline_contract ? "@inline! " : "@inline ", printer->out);
    }
    if (decl->is_noinline) {
      fputs("@noinline ", printer->out);
    }
    if (decl->is_pure) {
      fputs("@pure ", printer->out);
    }
    if (decl->is_noalloc) {
      fputs("@noalloc ", printer->out);
    }
    if (decl->is_test) {
      fputs("@test ", printer->out);
    }
    if (decl->is_rule) {
      fputs("@rule ", printer->out);
    }
    if (decl->is_swappable) {
      fputs("@swappable ", printer->out);
    }
    if (decl->is_naked) {
      fputs("@naked ", printer->out);
    }
    if (decl->is_interrupt) {
      fputs("@interrupt ", printer->out);
    }
    if (decl->simd_mode == SIMD_ATTR_HINT) {
      fputs("@simd ", printer->out);
    } else if (decl->simd_mode == SIMD_ATTR_CONTRACT) {
      fputs("@simd! ", printer->out);
    }
    if (decl->is_exported) {
      fputs("export ", printer->out);
    }
    fprintf(printer->out, "fn %s(", decl->name ? decl->name : "<name>");
    for (size_t i = 0; i < decl->parameter_count; i++) {
      fprintf(printer->out, "%s%s: %s", i ? ", " : "",
              decl->parameter_names[i], decl->parameter_types[i]);
    }
    fputc(')', printer->out);
    if (decl->return_type) {
      fprintf(printer->out, " -> %s", decl->return_type);
    }
    if (!decl->body) {
      fputs(";\n\n", printer->out);
      break;
    }
    fputs(" {\n", printer->out);
    printer->depth++;
    for (size_t i = 0; i < decl->body->child_count; i++) {
      print_statement(printer, decl->body->children[i]);
    }
    printer->depth--;
    fputs("}\n\n", printer->out);
    break;
  }

  case AST_IMPORT:
  case AST_IMPORT_STR:
    /* Imports are resolved and inlined before expansion runs, so reprinting
     * them would describe a program that no longer exists. */
    break;

  case AST_VAR_DECLARATION:
    print_statement(printer, node);
    break;

  /* A call at module scope is `static_assert(...)`, which the statement
   * printer already writes. Reaching the default here would report a
   * declaration the programmer wrote as having no source form. */
  case AST_FUNCTION_CALL:
    print_statement(printer, node);
    break;

  default:
    print_unprintable(printer, "this declaration");
    fputc('\n', printer->out);
    break;
  }
}

size_t ast_print_program(FILE *out, const ASTNode *program,
                         AstPrintAnnotator annotate, void *context) {
  if (!out || !program) {
    return 0;
  }
  AstPrinter printer = {out, 0, 0, annotate, context};
  for (size_t i = 0; i < program->child_count; i++) {
    print_declaration(&printer, program->children[i]);
  }
  return printer.unprintable;
}
