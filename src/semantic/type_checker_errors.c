// Type checker: diagnostic emission helpers.
#include "type_checker_internal.h"

void type_checker_set_error(TypeChecker *checker, const char *format, ...) {
  if (!checker || !format)
    return;

  // Free previous error message
  free(checker->error_message);

  // Calculate required buffer size
  va_list args1, args2;
  va_start(args1, format);
  va_copy(args2, args1);

  int size = vsnprintf(NULL, 0, format, args1);
  va_end(args1);

  if (size < 0) {
    checker->error_message = NULL;
    checker->has_error = 1;
    va_end(args2);
    return;
  }

  // Allocate and format the message
  checker->error_message = malloc(size + 1);
  if (checker->error_message) {
    vsnprintf(checker->error_message, size + 1, format, args2);
  }

  va_end(args2);
  checker->has_error = 1;
}

// Enhanced error reporting functions

void type_checker_set_error_at_location(TypeChecker *checker,
                                        SourceLocation location,
                                        const char *format, ...) {
  if (!checker || !format)
    return;

  checker->has_error = 1;
  free(checker->error_message);

  va_list args;
  va_start(args, format);

  // Calculate required buffer size
  va_list args_copy;
  va_copy(args_copy, args);
  int size = vsnprintf(NULL, 0, format, args_copy);
  va_end(args_copy);

  if (size > 0) {
    checker->error_message = malloc(size + 1);
    if (checker->error_message) {
      vsnprintf(checker->error_message, size + 1, format, args);
    }
  }

  // If we have an error reporter, add the error to it
  if (checker->error_reporter) {
    char *message = checker->error_message;
    SourceSpan span = source_span_from_location(location, 1);
    error_reporter_add_error_with_span(checker->error_reporter, ERROR_SEMANTIC,
                                       span, message);
  }

  va_end(args);
}

/* Width of the source text a node occupies, for full-token caret underlines.
   Conservative: falls back to 1 when the width isn't recoverable. */
size_t type_checker_node_span_length(const ASTNode *node) {
  if (!node || !node->data)
    return 1;
  switch (node->type) {
  case AST_IDENTIFIER: {
    const Identifier *id = (const Identifier *)node->data;
    return id->name ? strlen(id->name) : 1;
  }
  case AST_STRING_LITERAL: {
    const StringLiteral *lit = (const StringLiteral *)node->data;
    return lit->value ? strlen(lit->value) + 2 : 1; /* include quotes */
  }
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *num = (const NumberLiteral *)node->data;
    char buf[64];
    int n;
    if (num->is_float)
      n = snprintf(buf, sizeof(buf), "%g", num->float_value);
    else
      n = snprintf(buf, sizeof(buf), "%lld", num->int_value);
    return n > 0 ? (size_t)n : 1;
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)node->data;
    return call->function_name ? strlen(call->function_name) : 1;
  }
  default:
    return 1;
  }
}

static void type_checker_report_type_mismatch_span(TypeChecker *checker,
                                                   SourceLocation location,
                                                   size_t span_length,
                                                   const char *expected,
                                                   const char *actual) {
  if (!checker || !expected || !actual)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg),
           "Type mismatch: expected '%s', found '%s'", expected, actual);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char *suggestion =
        error_reporter_suggest_for_type_mismatch(expected, actual);
    SourceSpan span = source_span_from_location(location, span_length);
    if (suggestion) {
      error_reporter_add_error_with_span_and_suggestion(
          checker->error_reporter, ERROR_TYPE, span, error_msg, suggestion);
      free(suggestion);
    } else {
      error_reporter_add_error_with_span(checker->error_reporter, ERROR_TYPE,
                                         span, error_msg);
    }
    char label[192];
    snprintf(label, sizeof(label), "expected '%s', found '%s'", expected,
             actual);
    error_reporter_set_last_label(checker->error_reporter, label);
  }
}

void type_checker_report_type_mismatch(TypeChecker *checker,
                                       SourceLocation location,
                                       const char *expected,
                                       const char *actual) {
  type_checker_report_type_mismatch_span(checker, location, 1, expected,
                                         actual);
}

void type_checker_report_type_mismatch_node(TypeChecker *checker,
                                            const ASTNode *node,
                                            const char *expected,
                                            const char *actual) {
  if (!node) {
    return;
  }
  type_checker_report_type_mismatch_span(checker, node->location,
                                         type_checker_node_span_length(node),
                                         expected, actual);
}

int type_checker_reject_rawptr_element_use(TypeChecker *checker,
                                           SourceLocation location,
                                           const char *what) {
  char message[256];
  char help[256];
  SourceSpan span;

  if (!checker) {
    return 0;
  }
  snprintf(message, sizeof(message),
           "cannot %s a 'rawptr': it has no element type", what);
  snprintf(help, sizeof(help),
           "give the address a type first: `var p: int32* = raw;` and %s that",
           what);
  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(message);
  if (checker->error_reporter) {
    span = source_span_from_location(location, 1);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_TYPE, span, message, help);
    error_reporter_set_last_label(checker->error_reporter,
                                  "an address with no element type");
  }
  return 1;
}

/* Print an integer bound the way the source would spell it: unsigned maxima
   above INT64_MAX do not fit the signed formatter. */
static void type_checker_format_bounds(const Type *type, char *out,
                                       size_t out_size) {
  long long min = 0;
  unsigned long long max = 0;
  if (!type_checker_integer_bounds(type, &min, &max)) {
    snprintf(out, out_size, "?");
    return;
  }
  snprintf(out, out_size, "%lld..%llu", min, max);
}

void type_checker_report_assign_mismatch(TypeChecker *checker,
                                         const ASTNode *src_expr,
                                         SourceLocation location,
                                         Type *dest_type, Type *src_type) {
  const char *expected = dest_type && dest_type->name ? dest_type->name : "?";
  const char *actual = src_type && src_type->name ? src_type->name : "?";
  size_t span_length = src_expr ? type_checker_node_span_length(src_expr) : 1;
  SourceLocation where = src_expr ? src_expr->location : location;
  long long folded = 0;
  int is_integer_pair = dest_type && src_type &&
                        type_checker_is_integer_type(dest_type) &&
                        type_checker_is_integer_type(src_type) &&
                        dest_type->kind != TYPE_ENUM &&
                        src_type->kind != TYPE_ENUM;
  int is_float_narrow = dest_type && src_type &&
                        type_checker_is_floating_type(dest_type) &&
                        type_checker_is_floating_type(src_type) &&
                        (dest_type->kind == TYPE_FLOAT16 || dest_type->kind == TYPE_BFLOAT16) &&
                        (src_type->kind == TYPE_FLOAT32 || src_type->kind == TYPE_FLOAT64);
  int folds = 0;
  char message[512];
  char help[320];
  char bounds[80];
  SourceSpan span;

  if (!checker) {
    return;
  }
  if (is_float_narrow) {
    snprintf(message, sizeof(message),
             "Narrowing conversion from '%s' to '%s' needs a cast", actual,
             expected);
    snprintf(help, sizeof(help), "cast explicitly: (%s)value", expected);
    checker->has_error = 1;
    free(checker->error_message);
    checker->error_message = strdup(message);
    if (!checker->error_reporter) {
      return;
    }
    span = source_span_from_location(where, span_length);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_TYPE, span, message, help);
    error_reporter_set_last_code(checker->error_reporter, "M0119");
    {
      char label[192];
      snprintf(label, sizeof(label), "'%s' value stored as '%s'", actual,
               expected);
      error_reporter_set_last_label(checker->error_reporter, label);
    }
    return;
  }
  if (!is_integer_pair) {
    type_checker_report_type_mismatch_span(checker, where, span_length,
                                           expected, actual);
    return;
  }

  folds = src_expr && type_checker_eval_integer_constant_with_checker(
                          checker, (ASTNode *)src_expr, &folded);
  type_checker_format_bounds(dest_type, bounds, sizeof(bounds));

  if (folds) {
    /* The value is known and does not fit: naming it, and the range it missed,
     * is the whole diagnostic. Nothing here is a guess. */
    int src_unsigned = src_type->kind == TYPE_UINT8 ||
                       src_type->kind == TYPE_UINT16 ||
                       src_type->kind == TYPE_UINT32 ||
                       src_type->kind == TYPE_UINT64;
    if (src_unsigned) {
      snprintf(message, sizeof(message),
               "Integer %llu is out of range for '%s'",
               (unsigned long long)folded, expected);
    } else {
      snprintf(message, sizeof(message), "Integer %lld is out of range for '%s'",
               folded, expected);
    }
    snprintf(help, sizeof(help),
             "'%s' holds %s. Widen the type, or cast to say the wrap is meant: "
             "(%s)value",
             expected, bounds, expected);
  } else {
    snprintf(message, sizeof(message),
             "Narrowing conversion from '%s' to '%s' needs a cast", actual,
             expected);
    snprintf(help, sizeof(help),
             "cast explicitly: (%s)value. '%s' holds %s, so a value outside it "
             "wraps rather than trapping",
             expected, expected, bounds);
  }

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(message);
  if (!checker->error_reporter) {
    return;
  }
  span = source_span_from_location(where, span_length);
  error_reporter_add_error_with_span_and_suggestion(
      checker->error_reporter, ERROR_TYPE, span, message, help);
  error_reporter_set_last_code(checker->error_reporter,
                               folds ? "M0118" : "M0119");
  {
    char label[192];
    if (folds) {
      snprintf(label, sizeof(label), "does not fit in '%s' (%s)", expected,
               bounds);
    } else {
      snprintf(label, sizeof(label), "'%s' value stored as '%s'", actual,
               expected);
    }
    error_reporter_set_last_label(checker->error_reporter, label);
  }
}

/* Warn about locals declared in the current (about-to-close) scope that were
   never read. `_`-prefixed names opt out; only the main compile unit is
   checked so imported/stdlib code stays quiet. */
void type_checker_mark_captures_used(TypeChecker *checker,
                                    const FunctionDeclaration *lam) {
  if (!checker || !lam || !lam->captured_names) {
    return;
  }
  for (size_t i = 0; i < lam->captured_count; i++) {
    if (lam->captured_names[i]) {
      symbol_table_lookup(checker->symbol_table, lam->captured_names[i]);
    }
  }
}

void type_checker_warn_unused_locals(TypeChecker *checker) {
  if (!checker || !checker->error_reporter)
    return;
  Scope *scope = symbol_table_get_current_scope(checker->symbol_table);
  if (!scope || scope->type == SCOPE_GLOBAL)
    return;
  const char *main_file = checker->error_reporter->filename;
  for (size_t i = 0; i < scope->symbol_count; i++) {
    Symbol *s = scope->symbols[i];
    if (!s || s->kind != SYMBOL_VARIABLE || s->is_used)
      continue;
    if (!s->name || s->name[0] == '_' || s->name[0] == '.' || !s->decl_line)
      continue;
    if (s->decl_file && main_file && strcmp(s->decl_file, main_file) != 0)
      continue;
    char msg[256];
    snprintf(msg, sizeof(msg), "unused %s '%s'",
             s->is_immutable ? "constant" : "variable", s->name);
    char suggestion[256];
    snprintf(suggestion, sizeof(suggestion),
             "remove it, or rename it to '_%s' to keep it intentionally",
             s->name);
    SourceSpan span =
        source_span_create(s->decl_line, s->decl_column, strlen(s->name));
    span.filename = s->decl_file;
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             s->name);
    error_reporter_add_warning_span_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, msg, suggestion);
  }
}

void type_checker_note_declared_here(TypeChecker *checker,
                                     const Symbol *symbol, const char *what) {
  if (!checker || !checker->error_reporter || !symbol || !symbol->decl_line)
    return;
  char note[256];
  snprintf(note, sizeof(note), "%s '%s' %s here", what, symbol->name,
           symbol->kind == SYMBOL_FUNCTION ? "defined" : "declared");
  SourceSpan span = source_span_create(symbol->decl_line, symbol->decl_column,
                                       strlen(symbol->name));
  span.filename = symbol->decl_file;
  span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                           symbol->name);
  error_reporter_add_note_of_span(checker->error_reporter, span, note);
}

void type_checker_report_undefined_symbol(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const char *symbol_type) {
  if (!checker || !symbol_name || !symbol_type)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg), "Undefined %s '%s'", symbol_type,
           symbol_name);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char suggestion[256];
    char *closest = symbol_table_suggest_similar(checker->symbol_table,
                                                 symbol_name, NULL, 0);
    if (closest) {
      snprintf(suggestion, sizeof(suggestion),
               "did you mean '%s'? (or declare '%s' before using it)", closest,
               symbol_name);
      free(closest);
    } else {
      snprintf(suggestion, sizeof(suggestion), "declare '%s' before using it",
               symbol_name);
    }
    SourceSpan span = source_span_from_location(location, strlen(symbol_name));
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             symbol_name);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, error_msg, suggestion);
  }
}

void type_checker_report_duplicate_declaration_prev(TypeChecker *checker,
                                                    SourceLocation location,
                                                    const char *symbol_name,
                                                    const Symbol *previous) {
  type_checker_report_duplicate_declaration(checker, location, symbol_name);
  if (previous && previous->decl_line) {
    char note[256];
    snprintf(note, sizeof(note), "previous declaration of '%s' is here",
             symbol_name);
    SourceSpan span = source_span_create(
        previous->decl_line, previous->decl_column, strlen(symbol_name));
    span.filename = previous->decl_file;
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             symbol_name);
    error_reporter_add_note_of_span(checker->error_reporter, span, note);
  }
}

void type_checker_report_duplicate_declaration(TypeChecker *checker,
                                               SourceLocation location,
                                               const char *symbol_name) {
  if (!checker || !symbol_name)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg), "Duplicate declaration of '%s'",
           symbol_name);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char suggestion[256];
    snprintf(suggestion, sizeof(suggestion),
             "use a different name or remove the duplicate declaration");
    SourceSpan span = source_span_from_location(location, strlen(symbol_name));
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, error_msg, suggestion);
  }
}

static const char *comptime_only_type_name(const Type *type) {
  if (!type) {
    return "Type";
  }
  if (type->kind == TYPE_FIELD ||
      (type->name && strcmp(type->name, "Field") == 0)) {
    return "Field";
  }
  if (type_is_comptime_only(type) && type->name) {
    return type->name;
  }
  if (type->kind == TYPE_FIELD) {
    return "Field";
  }
  return type->name ? type->name : "Type";
}

static const Type *comptime_only_payload(const Type *type) {
  if (!type) {
    return NULL;
  }
  if (type_is_comptime_only(type)) {
    return type;
  }
  if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY ||
      type->kind == TYPE_SLICE) {
    return comptime_only_payload(type->base_type);
  }
  if (type->kind == TYPE_FUNCTION_POINTER) {
    if (type_contains_comptime_only(type->fn_return_type)) {
      return comptime_only_payload(type->fn_return_type);
    }
    for (size_t i = 0; i < type->fn_param_count; i++) {
      if (type_contains_comptime_only(type->fn_param_types[i])) {
        return comptime_only_payload(type->fn_param_types[i]);
      }
    }
  }
  if (type->kind == TYPE_STRUCT) {
    for (size_t i = 0; i < type->field_count; i++) {
      if (type_contains_comptime_only(type->field_types[i])) {
        return comptime_only_payload(type->field_types[i]);
      }
    }
  }
  return type;
}

int type_checker_reject_no_runtime_repr(TypeChecker *checker,
                                        SourceLocation location,
                                        const Type *type) {
  if (!checker || !type_contains_comptime_only(type)) {
    return 0;
  }

  const Type *payload = comptime_only_payload(type);
  const char *name = comptime_only_type_name(payload ? payload : type);
  char error_msg[512];
  if (type_is_comptime_only(type)) {
    snprintf(error_msg, sizeof(error_msg),
             "type '%s' has no runtime representation", name);
  } else {
    snprintf(error_msg, sizeof(error_msg),
             "type '%s' has no runtime representation, so it cannot appear "
             "inside '%s'",
             name, type->name ? type->name : "this type");
  }

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    SourceSpan span = source_span_from_location(location, name ? strlen(name) : 1);
    span = error_reporter_span_snap_to_token(checker->error_reporter, span, name);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_TYPE, span, error_msg,
        "'Type' and 'Field' are compile-time reflection values; bind them "
        "with `const` and use them only at compile time");
    error_reporter_set_last_label(checker->error_reporter,
                                  "no runtime representation");
  }
  return 1;
}

int type_checker_reject_comptime_escape(TypeChecker *checker,
                                        SourceLocation location,
                                        const Type *type) {
  if (!checker || !type_is_comptime_only(type)) {
    return 0;
  }

  const char *name = comptime_only_type_name(type);
  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg),
           "value of type '%s' cannot escape into runtime code", name);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    SourceSpan span = source_span_from_location(location, name ? strlen(name) : 1);
    span = error_reporter_span_snap_to_token(checker->error_reporter, span, name);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_TYPE, span, error_msg,
        "'Type' and 'Field' exist only at compile time; they cannot be "
        "stored, returned, passed to a function, or used in a runtime "
        "expression");
    error_reporter_set_last_label(checker->error_reporter,
                                  "compile-time only");
  }
  return 1;
}

void type_checker_report_parameter_shadow(TypeChecker *checker,
                                          SourceLocation location,
                                          const char *symbol_name,
                                          const Symbol *parameter) {
  if (!checker || !symbol_name)
    return;

  char error_msg[512];
  snprintf(error_msg, sizeof(error_msg), "Variable '%s' shadows parameter '%s'",
           symbol_name, symbol_name);

  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(error_msg);

  if (checker->error_reporter) {
    char suggestion[256];
    snprintf(suggestion, sizeof(suggestion),
             "use a different name so the parameter remains visible");
    SourceSpan span = source_span_from_location(location, strlen(symbol_name));
    span = error_reporter_span_snap_to_token(checker->error_reporter, span,
                                             symbol_name);
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_SEMANTIC, span, error_msg, suggestion);

    if (parameter && parameter->decl_line) {
      char note[256];
      snprintf(note, sizeof(note), "function parameter '%s' is declared here",
               symbol_name);
      SourceSpan parameter_span =
          source_span_create(parameter->decl_line, parameter->decl_column,
                             strlen(symbol_name));
      parameter_span.filename = parameter->decl_file;
      parameter_span = error_reporter_span_snap_to_token(
          checker->error_reporter, parameter_span, symbol_name);
      error_reporter_add_note_of_span(checker->error_reporter, parameter_span,
                                      note);
    }
  }
}
