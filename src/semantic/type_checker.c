// Type checker: lifecycle, function-signature registration, program driver.
#include "type_checker_internal.h"

Symbol *type_checker_resolve_identifier(TypeChecker *checker,
                                        Identifier *identifier) {
  if (!checker || !checker->symbol_table || !identifier ||
      !identifier->name) {
    return NULL;
  }

  /* A generated module-scope declaration is checked with its `comptime for`
   * binding in effect. A module has no scope to hold it in, so it is asked for
   * first, and only while the declaration it belongs to is in flight. */
  Symbol *symbol =
      type_checker_lookup_expansion_binding(checker, identifier->name);
  if (symbol) {
    return symbol;
  }
  symbol = symbol_table_lookup(checker->symbol_table, identifier->name);
  /* `Kind` is registered on first mention rather than at startup, so a program
   * that never reflects allocates none of its type and interns none of its
   * member names -- and --report-expansion can say so rather than ask you to
   * assume it. */
  if (!symbol && strcmp(identifier->name, "Kind") == 0) {
    type_checker_register_kind_enum(checker);
    symbol = symbol_table_lookup(checker->symbol_table, identifier->name);
  }
  if (symbol && symbol->scope) {
    identifier->scope_id = symbol->scope->scope_id;
  }
  return symbol;
}

TypeChecker *type_checker_create(SymbolTable *symbol_table) {
  return type_checker_create_with_error_reporter(symbol_table, NULL);
}

TypeChecker *
type_checker_create_with_error_reporter(SymbolTable *symbol_table,
                                        ErrorReporter *error_reporter) {
  TypeChecker *checker = malloc(sizeof(TypeChecker));
  if (!checker)
    return NULL;

  checker->symbol_table = symbol_table;
  checker->has_error = 0;
  checker->error_message = NULL;
  checker->error_reporter = error_reporter;
  checker->current_function = NULL;
  checker->current_function_decl = NULL;
  checker->loop_depth = 0;
  checker->switch_depth = 0;
  checker->loop_labels = NULL;
  checker->loop_label_count = 0;
  checker->loop_label_capacity = 0;
  checker->tracked_var_names = NULL;
  checker->tracked_var_initialized = NULL;
  checker->tracked_var_scope_depth = NULL;
  checker->tracked_var_count = 0;
  checker->tracked_var_capacity = 0;
  checker->tracked_scope_markers = NULL;
  checker->tracked_scope_count = 0;
  checker->tracked_scope_capacity = 0;
  checker->tracked_scope_depth = 0;
  checker->tracked_buffer_extents = NULL;
  checker->aggregate_target_type = NULL;
  checker->aggregate_requires_constant = 0;
  checker->module_program = NULL;
  checker->struct_placeholders = NULL;
  checker->struct_placeholder_count = 0;
  checker->struct_placeholder_capacity = 0;

  // Initialize built-in type pointers to NULL
  checker->builtin_int8 = NULL;
  checker->builtin_int16 = NULL;
  checker->builtin_int32 = NULL;
  checker->builtin_int64 = NULL;
  checker->builtin_uint8 = NULL;
  checker->builtin_uint16 = NULL;
  checker->builtin_uint32 = NULL;
  checker->builtin_uint64 = NULL;
  checker->builtin_bool = NULL;
  checker->builtin_float32 = NULL;
  checker->builtin_float64 = NULL;
  checker->builtin_string = NULL;
  checker->builtin_cstring = NULL;
  checker->builtin_void = NULL;
  checker->builtin_type = NULL;
  checker->builtin_field = NULL;
  checker->builtin_row = NULL;
  checker->builtin_sequence = NULL;
  checker->type_table = NULL;
  checker->type_table_count = 0;
  checker->type_table_capacity = 0;
  checker->expansions = NULL;
  checker->comptime_bindings = NULL;
  checker->comptime_binding_count = 0;
  checker->comptime_binding_capacity = 0;
  checker->builtin_kind = NULL;
  checker->sequences = NULL;
  checker->generic_enum_templates = NULL;
  checker->generic_enum_template_count = 0;

  // Initialize built-in types
  type_checker_init_builtin_types(checker);

  // Test builtins: assert(cond) / assert_eq(left, right). Registered always
  // so @test bodies type-check in every build; calling them outside a @test
  // function is rejected at the call site (they only execute under
  // `mettle test`, where the interpreter implements them natively).
  type_checker_register_test_builtin(checker, "assert", 1);
  type_checker_register_test_builtin(checker, "assert_eq", 2);

  return checker;
}

void type_checker_register_test_builtin(TypeChecker *checker, const char *name,
                                        size_t parameter_count) {
  if (!checker || !checker->symbol_table || parameter_count > 2) {
    return;
  }
  if (symbol_table_lookup_current_scope(checker->symbol_table, name)) {
    return;
  }
  Symbol *symbol = symbol_create(name, SYMBOL_FUNCTION, checker->builtin_void);
  if (!symbol) {
    return;
  }
  Type **param_types = malloc(parameter_count * sizeof(Type *));
  char **param_names = malloc(parameter_count * sizeof(char *));
  if (!param_types || !param_names) {
    free(param_types);
    free(param_names);
    symbol_destroy(symbol);
    return;
  }
  static const char *NAMES[2] = {"left", "right"};
  for (size_t i = 0; i < parameter_count; i++) {
    param_types[i] = checker->builtin_int64;
    param_names[i] = strdup(NAMES[i]);
  }
  symbol->data.function.parameter_count = parameter_count;
  symbol->data.function.parameter_types = param_types;
  symbol->data.function.parameter_names = param_names;
  symbol->data.function.return_type = checker->builtin_void;
  symbol->is_extern = 1;
  symbol->is_builtin = 1;
  symbol->is_initialized = 1;
  symbol->link_name = strdup(name);
  if (!symbol_table_declare(checker->symbol_table, symbol)) {
    symbol_destroy(symbol);
  }
}

void type_checker_destroy(TypeChecker *checker) {
  if (checker) {
    free(checker->guards);
    free(checker->refine_failure);
    // Clean up built-in types
    type_destroy(checker->builtin_int8);
    type_destroy(checker->builtin_int16);
    type_destroy(checker->builtin_int32);
    type_destroy(checker->builtin_int64);
    type_destroy(checker->builtin_uint8);
    type_destroy(checker->builtin_uint16);
    type_destroy(checker->builtin_uint32);
    type_destroy(checker->builtin_uint64);
    type_destroy(checker->builtin_bool);
    type_destroy(checker->builtin_float32);
    type_destroy(checker->builtin_float64);
    type_destroy(checker->builtin_string);
    type_destroy(checker->builtin_cstring);
    type_destroy(checker->builtin_void);
    type_destroy(checker->builtin_type);
    type_destroy(checker->builtin_field);
    type_destroy(checker->builtin_row);
    type_destroy(checker->builtin_sequence);
    free(checker->type_table);
    free(checker->struct_placeholders);
    type_checker_expansions_destroy(checker->expansions);
    free(checker->comptime_bindings);
    type_checker_sequences_destroy(checker->sequences);
    free(checker->generic_enum_templates);

    for (size_t i = 0; i < checker->tracked_var_count; i++) {
      free(checker->tracked_var_names[i]);
    }
    free(checker->tracked_var_names);
    free(checker->tracked_var_initialized);
    free(checker->tracked_var_scope_depth);
    free(checker->tracked_scope_markers);
    free(checker->loop_labels);
    type_checker_buffer_extent_clear(checker);

    free(checker->error_message);
    free(checker);
  }
}

void type_checker_note_gathered_parameter(FunctionDeclaration *declaration) {
  size_t last = 0;
  const char *written = NULL;
  size_t length = 0;
  if (!declaration || declaration->parameter_count == 0) {
    return;
  }
  last = declaration->parameter_count - 1;
  written = declaration->parameter_types ? declaration->parameter_types[last]
                                         : NULL;
  length = written ? strlen(written) : 0;
  if (length > 4 && strcmp(written + length - 4, "[..]") == 0) {
    declaration->is_variadic = 1;
  }
}

static int type_checker_type_is_named(const Type *type,
                                      const char *qualified) {
  return type && ((type->qualified_name &&
                   strcmp(type->qualified_name, qualified) == 0) ||
                  (type->name && strcmp(type->name, qualified) == 0));
}

int type_checker_validate_rule_signature(TypeChecker *checker,
                                                ASTNode *declaration,
                                                FunctionDeclaration *func_decl,
                                                Type **param_types,
                                                Type *return_type) {
  int shape_ok = func_decl->parameter_count == 1 && param_types &&
                 type_checker_type_is_named(param_types[0],
                                            "std/rule.Program") &&
                 type_checker_type_is_named(return_type, "std/rule.Verdict");
  if (shape_ok && !func_decl->is_exported && !func_decl->is_extern &&
      func_decl->body) {
    return 1;
  }
  type_checker_set_error_at_location(
      checker, declaration->location,
      "a @rule is declared `@rule fn %s(p: Program) -> Verdict` with a body, "
      "taking the Program and returning the Verdict from std/rule",
      func_decl->name ? func_decl->name : "name");
  if (checker->error_reporter) {
    error_reporter_set_last_label(checker->error_reporter,
                                  "signature is not (Program) -> Verdict");
  }
  return 0;
}

int type_checker_register_function_signature(TypeChecker *checker,
                                                    ASTNode *declaration) {
  if (!checker || !declaration ||
      declaration->type != AST_FUNCTION_DECLARATION) {
    return 0;
  }

  FunctionDeclaration *func_decl = (FunctionDeclaration *)declaration->data;
  if (!func_decl || !func_decl->name)
    return 0;

  if (func_decl->return_type_count > 0 &&
      !type_checker_ensure_multi_return_type(checker, func_decl,
                                             declaration->location)) {
    return 0;
  }

  Symbol *existing =
      symbol_table_lookup_current_scope(checker->symbol_table, func_decl->name);
  if (existing)
    return 1;

  Type *return_type = NULL;
  if (func_decl->return_type) {
    return_type =
        type_checker_get_type_by_name(checker, func_decl->return_type);
    if (!return_type)
      return 0;
  } else {
    return_type = checker->builtin_void;
  }

  /* A last parameter written `T[..]` gathers, and this is the signature a call
     earlier in the file resolves against, so it has to know that here. */
  type_checker_note_gathered_parameter(func_decl);

  Type **param_types = NULL;
  if (func_decl->parameter_count > 0) {
    param_types = malloc(func_decl->parameter_count * sizeof(Type *));
    if (!param_types)
      return 0;
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      param_types[i] =
          type_checker_get_type_by_name(checker, func_decl->parameter_types[i]);
      if (!param_types[i]) {
        free(param_types);
        return 0;
      }
    }
  }

  char **param_names_copy = NULL;
  if (func_decl->parameter_count > 0) {
    param_names_copy = malloc(func_decl->parameter_count * sizeof(char *));
    if (!param_names_copy) {
      free(param_types);
      return 0;
    }
    for (size_t i = 0; i < func_decl->parameter_count; i++) {
      param_names_copy[i] = strdup(func_decl->parameter_names[i]);
    }
  }

  Symbol *func_symbol =
      symbol_create(func_decl->name, SYMBOL_FUNCTION, return_type);
  if (func_symbol) {
    func_symbol->decl_line = declaration->location.line;
    func_symbol->decl_column = declaration->location.column;
    func_symbol->decl_file = declaration->location.filename;
  }
  if (!func_symbol) {
    for (size_t i = 0; i < func_decl->parameter_count; i++)
      free(param_names_copy[i]);
    free(param_names_copy);
    free(param_types);
    return 0;
  }

  func_symbol->data.function.parameter_count = func_decl->parameter_count;
  func_symbol->data.function.parameter_names = param_names_copy;
  func_symbol->data.function.parameter_types = param_types;
  func_symbol->data.function.return_type = return_type;
  func_symbol->data.function.is_variadic = func_decl->is_variadic;
  /* Kernel identity travels with the signature: this pre-registration is the
   * symbol a `dispatch` earlier in the file resolves against, so it has to
   * know the declaration was a `kernel` and what block shape it declared. */
  func_symbol->is_kernel = func_decl->is_kernel;
  func_symbol->is_rule = func_decl->is_rule;
  func_symbol->kernel_block[0] = func_decl->kernel_block[0];
  func_symbol->kernel_block[1] = func_decl->kernel_block[1];
  func_symbol->kernel_block[2] = func_decl->kernel_block[2];
  func_symbol->kernel_threads_per_item = func_decl->kernel_threads_per_item;
  func_symbol->is_extern = func_decl->is_extern;
  if (func_decl->is_extern) {
    const char *effective_link_name = type_checker_decl_link_name(
        func_decl->name, func_decl->is_extern, func_decl->link_name);
    func_symbol->link_name =
        effective_link_name ? strdup(effective_link_name) : NULL;
    if (!func_symbol->link_name) {
      symbol_destroy(func_symbol);
      return 0;
    }
  }
  func_symbol->is_initialized = 0;
  func_symbol->is_forward_declaration = 1;

  if (!symbol_table_declare_forward(checker->symbol_table, func_symbol)) {
    symbol_destroy(func_symbol);
    return 0;
  }

  return 1;
}

/* The name a declaration introduces, or NULL when it introduces none. */
static const char *type_decl_name(const ASTNode *decl) {
  if (!decl) {
    return NULL;
  }
  if (decl->type == AST_STRUCT_DECLARATION) {
    const StructDeclaration *s = (const StructDeclaration *)decl->data;
    return s ? s->name : NULL;
  }
  if (decl->type == AST_ENUM_DECLARATION) {
    const EnumDeclaration *en = (const EnumDeclaration *)decl->data;
    return en ? en->name : NULL;
  }
  if (decl->type == AST_TYPE_DECLARATION) {
    const TypeDeclaration *td = (const TypeDeclaration *)decl->data;
    return td ? td->name : NULL;
  }
  return NULL;
}

/* Does this type text name `what` as a whole identifier? `Span`, `Span*`,
 * `Span[4]` and `Cell<Span>` all do; `Spanner` does not. */
static int type_text_names(const char *text, const char *what) {
  size_t length;
  const char *at;

  if (!text || !what || !*what) {
    return 0;
  }
  length = strlen(what);
  for (at = strstr(text, what); at; at = strstr(at + 1, what)) {
    int left_ok = at == text || (!isalnum((unsigned char)at[-1]) &&
                                 at[-1] != '_');
    char right = at[length];
    int right_ok = !isalnum((unsigned char)right) && right != '_';
    if (left_ok && right_ok) {
      return 1;
    }
  }
  return 0;
}

/* Does this type text name `what` somewhere a stored value of `what` has to
 * be there, rather than a pointer to one? `Span` and `Span[4]` do; `Span*` and
 * `Span*[4]` do not. A pointer needs the name to exist and nothing more, which
 * is what lets a cycle of pointers be registered at all. */
static int type_text_names_by_value(const char *text, const char *what) {
  size_t length;
  const char *at;

  if (!text || !what || !*what) {
    return 0;
  }
  length = strlen(what);
  for (at = strstr(text, what); at; at = strstr(at + 1, what)) {
    const char *after;
    int left_ok = at == text || (!isalnum((unsigned char)at[-1]) &&
                                 at[-1] != '_');
    char right = at[length];
    int right_ok = !isalnum((unsigned char)right) && right != '_';
    if (!left_ok || !right_ok) {
      continue;
    }
    after = at + length;
    for (;;) {
      while (*after == ' ' || *after == '	') {
        after++;
      }
      if (*after == '[') {
        const char *close = strchr(after, ']');
        if (!close) {
          break;
        }
        after = close + 1;
        continue;
      }
      break;
    }
    if (*after == '*') {
      continue;
    }
    return 1;
  }
  return 0;
}

/* Does `decl` store a value of `name`, so that its own size depends on it? */
static int type_decl_holds_by_value(const ASTNode *decl, const char *name) {
  size_t i;
  if (!decl || !name) {
    return 0;
  }
  if (decl->type == AST_STRUCT_DECLARATION) {
    const StructDeclaration *s = (const StructDeclaration *)decl->data;
    if (!s) {
      return 0;
    }
    for (i = 0; i < s->field_count; i++) {
      if (type_text_names_by_value(s->field_types ? s->field_types[i] : NULL,
                                   name)) {
        return 1;
      }
    }
    return 0;
  }
  if (decl->type == AST_ENUM_DECLARATION) {
    const EnumDeclaration *en = (const EnumDeclaration *)decl->data;
    if (!en) {
      return 0;
    }
    for (i = 0; i < en->variant_count; i++) {
      if (type_text_names_by_value(en->variants[i].payload_type, name)) {
        return 1;
      }
    }
  }
  return 0;
}

/* Does `decl` refer to `name` in a field type or a variant payload? */
static int type_decl_refers_to(const ASTNode *decl, const char *name) {
  size_t i;
  if (!decl || !name) {
    return 0;
  }
  if (decl->type == AST_TYPE_DECLARATION) {
    const TypeDeclaration *td = (const TypeDeclaration *)decl->data;
    return td && type_text_names(td->base_type, name);
  }
  if (decl->type == AST_STRUCT_DECLARATION) {
    const StructDeclaration *s = (const StructDeclaration *)decl->data;
    if (!s) {
      return 0;
    }
    for (i = 0; i < s->field_count; i++) {
      if (type_text_names(s->field_types ? s->field_types[i] : NULL, name)) {
        return 1;
      }
    }
    return 0;
  }
  if (decl->type == AST_ENUM_DECLARATION) {
    const EnumDeclaration *en = (const EnumDeclaration *)decl->data;
    if (!en) {
      return 0;
    }
    for (i = 0; i < en->variant_count; i++) {
      if (type_text_names(en->variants[i].payload_type, name)) {
        return 1;
      }
    }
  }
  return 0;
}

/* Struct and enum registration, over either the declarations the programmer
 * wrote or the ones expansion generated. Two sweeps rather than one because
 * expansion sits between them: a directive reflects on a type, so the written
 * types have to exist before it runs, and the types it generates only exist
 * after.
 *
 * Within a sweep the order is by dependency, not by where the declarations sit
 * in the file. A function can call one declared below it, and a type gets the
 * same courtesy: `enum Shape { Wide(Span) }` above `struct Span` used to be
 * "payload of unknown type 'Span'". A declaration waits while any type it names
 * is still pending; when a round settles nothing, the rest are processed in
 * source order so a genuine cycle or a genuinely unknown name reports itself. */
static int type_checker_report_type_cycles(TypeChecker *checker,
                                           Program *prog, char *pending,
                                           size_t *remaining_out) {
  size_t remaining = *remaining_out;
  size_t i, j;
  int ok = 1;

  /* Nothing moved, so what is left refers to itself in a circle. A circle
   * of pointers is a shape a program is entitled to write, and it has no
   * order that puts every name before its use, so the names are declared
   * first and the fields filled in afterwards. A circle that stores values
   * has no layout at all and is reported here, once, naming both ends. */
  for (i = 0; i < prog->declaration_count; i++) {
    const char *self;
    if (!pending[i]) {
      continue;
    }
    self = type_decl_name(prog->declarations[i]);
    if (!self) {
      continue;
    }
    for (j = 0; j < prog->declaration_count; j++) {
      const char *other;
      if (j == i || !pending[j]) {
        continue;
      }
      other = type_decl_name(prog->declarations[j]);
      if (!other || !type_decl_holds_by_value(prog->declarations[i],
                                              other) ||
          !type_decl_holds_by_value(prog->declarations[j], self)) {
        continue;
      }
      type_checker_set_error_at_location(
          checker, prog->declarations[i]->location,
          "'%s' and '%s' each store a value of the other, so neither has "
          "a size. Hold one of them by pointer: '%s*'",
          self, other, other);
      ok = 0;
      pending[i] = 0;
      pending[j] = 0;
      remaining -= 2;
      break;
    }
  }
  for (i = 0; i < prog->declaration_count; i++) {
    if (pending[i] &&
        prog->declarations[i]->type == AST_STRUCT_DECLARATION) {
      type_checker_declare_struct_placeholder(checker,
                                              prog->declarations[i]);
    }
  }
  *remaining_out = remaining;
  return ok;
}

static int type_checker_register_types(TypeChecker *checker, Program *prog,
                                       int generated_only) {
  int ok = 1;
  char *pending = calloc(prog->declaration_count, 1);
  size_t remaining = 0;
  size_t i, j;

  for (i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    ComptimeDeclScope expansion;
    if (!decl || (decl->type != AST_STRUCT_DECLARATION &&
                  decl->type != AST_ENUM_DECLARATION &&
                  decl->type != AST_TYPE_DECLARATION)) {
      continue;
    }
    type_checker_enter_expansion_decl(checker, decl, &expansion);
    if ((expansion.bindings_pushed > 0) == generated_only) {
      if (pending) {
        pending[i] = 1;
        remaining++;
      }
    }
    type_checker_leave_expansion_decl(checker, &expansion);
  }

  while (pending && remaining > 0) {
    size_t settled = 0;
    for (i = 0; i < prog->declaration_count; i++) {
      ASTNode *decl;
      ComptimeDeclScope expansion;
      const char *self;
      int waiting = 0;
      if (!pending[i]) {
        continue;
      }
      decl = prog->declarations[i];
      self = type_decl_name(decl);
      for (j = 0; j < prog->declaration_count && !waiting; j++) {
        const char *other;
        if (j == i || !pending[j]) {
          continue;
        }
        other = type_decl_name(prog->declarations[j]);
        /* A name declared twice is a duplicate, reported when it is processed;
         * waiting on the other one here would just stall the round. */
        if (other && (!self || strcmp(other, self) != 0) &&
            type_decl_refers_to(decl, other)) {
          waiting = 1;
        }
      }
      if (waiting) {
        continue;
      }
      pending[i] = 0;
      remaining--;
      settled++;
      type_checker_enter_expansion_decl(checker, decl, &expansion);
      if (decl->type == AST_TYPE_DECLARATION
              ? !type_checker_process_type_declaration(checker, decl)
              : decl->type == AST_STRUCT_DECLARATION
                    ? !type_checker_process_struct_declaration(checker, decl)
                    : !type_checker_process_enum_declaration(checker, decl)) {
        ok = 0;
      }
      type_checker_leave_expansion_decl(checker, &expansion);
    }
    if (settled == 0) {
      if (!type_checker_report_type_cycles(checker, prog, pending,
                                           &remaining)) {
        ok = 0;
      }
      break;
    }
  }

  for (i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl;
    ComptimeDeclScope expansion;
    if (pending && !pending[i]) {
      continue;
    }
    decl = prog->declarations[i];
    if (!decl || (decl->type != AST_STRUCT_DECLARATION &&
                  decl->type != AST_ENUM_DECLARATION)) {
      continue;
    }
    type_checker_enter_expansion_decl(checker, decl, &expansion);
    if ((expansion.bindings_pushed > 0) == generated_only) {
      if (decl->type == AST_STRUCT_DECLARATION
              ? !type_checker_process_struct_declaration(checker, decl)
              : !type_checker_process_enum_declaration(checker, decl)) {
        ok = 0;
      }
    }
    type_checker_leave_expansion_decl(checker, &expansion);
  }

  free(pending);
  return ok;
}

int type_checker_check_program(TypeChecker *checker, ASTNode *program) {
  if (!checker || !program || program->type != AST_PROGRAM) {
    return 0;
  }

  Program *prog = (Program *)program->data;
  if (!prog)
    return 0;
  checker->module_program = program;

  // Pass 1: Register struct and enum types. On failure keep going so every
  // bad declaration is reported in one compile, not one per rebuild.
  int ok = 1;
  if (!type_checker_register_types(checker, prog, 0)) {
    ok = 0;
  }

  /* Pass 1.5: expand every module-scope `comptime for` into the declarations it
     generates. It runs after the types are registered, because a directive
     reflects on one (`typeof(T).fields`), and before anything looks for a
     function to check, because what it generates is exactly that. From here on
     nothing downstream can tell a generated declaration from a written one,
     which is the point: contracts, diagnostics and the borrow checker all hold
     generated code to the standard hand-written code is held to. */
  if (!type_checker_expand_comptime_block(checker, program, 1)) {
    ok = 0;
  }
  if (!type_checker_check_composed_names(checker, program)) {
    ok = 0;
  }
  if (!ok)
    return 0;

  /* Types the expansion generated, registered in their own sweep so a
     generated struct can be referred to by a generated function declared
     before it, the same way written ones can. */
  if (!type_checker_register_types(checker, prog, 1)) {
    ok = 0;
  }

  // Pass 2: Register all function signatures so any function can call any other
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION) {
      ComptimeDeclScope expansion;
      type_checker_enter_expansion_decl(checker, decl, &expansion);
      type_checker_register_function_signature(checker, decl);
      type_checker_leave_expansion_decl(checker, &expansion);
    }
  }

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_TYPE_DECLARATION) {
      ComptimeDeclScope expansion;
      type_checker_enter_expansion_decl(checker, decl, &expansion);
      if (!type_checker_check_type_predicate(checker, decl)) {
        ok = 0;
      }
      type_checker_leave_expansion_decl(checker, &expansion);
    }
  }
  if (!ok)
    return 0;

  /* Pass 3: process the remaining declarations, globals before function
   * bodies. A function may call one declared below it, and a type may name one
   * declared below it; a global was the odd one out, so a function reading a
   * global written later in the file was "Undefined variable". Taking the
   * globals first is what puts them on the same footing. Their initializers are
   * compile-time constants, and any function or type they name is already
   * registered by the passes above. */
  for (int functions_now = 0; functions_now <= 1; functions_now++) {
    for (size_t i = 0; i < prog->declaration_count; i++) {
      ASTNode *decl = prog->declarations[i];
      ComptimeDeclScope expansion;
      if (!decl || decl->type == AST_STRUCT_DECLARATION ||
          decl->type == AST_ENUM_DECLARATION ||
          decl->type == AST_TYPE_DECLARATION) {
        continue;
      }
      if ((decl->type == AST_FUNCTION_DECLARATION) != functions_now) {
        continue;
      }
      type_checker_enter_expansion_decl(checker, decl, &expansion);
      if (!type_checker_process_declaration(checker, decl)) {
        ok = 0;
      }
      type_checker_leave_expansion_decl(checker, &expansion);
    }
  }

  if (!ok)
    return 0;

  // Pass 4: whole-program memory diagnostics. Ownership summaries are
  // inferred over the call graph, then cross-call use-after-free and leak
  // analysis runs with them (type_checker_memory.c). Skipped when earlier
  // passes failed: the AST is not fully typed.
  if (!getenv("METTLE_NO_MEM_INTERPROC") &&
      !type_checker_check_program_memory(checker, program)) {
    return 0;
  }

  return 1;
}

const char *type_checker_decl_link_name(const char *name, int is_extern,
                                               const char *link_name) {
  if (!is_extern) {
    return name;
  }
  if (link_name && link_name[0] != '\0') {
    return link_name;
  }
  return name;
}

const char *type_checker_symbol_link_name(const Symbol *symbol) {
  if (!symbol) {
    return NULL;
  }
  if (symbol->is_extern && symbol->link_name && symbol->link_name[0] != '\0') {
    return symbol->link_name;
  }
  return symbol->name;
}

int type_checker_link_name_matches_symbol(const Symbol *symbol,
                                                 const char *decl_name,
                                                 int decl_is_extern,
                                                 const char *decl_link_name) {
  const char *existing = type_checker_symbol_link_name(symbol);
  const char *incoming =
      type_checker_decl_link_name(decl_name, decl_is_extern, decl_link_name);
  if (!existing || !incoming) {
    return existing == incoming;
  }
  return strcmp(existing, incoming) == 0;
}
