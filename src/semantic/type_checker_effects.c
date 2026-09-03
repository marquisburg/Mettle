#include "type_checker_internal.h"
#include "../string_intern.h"

static const char *const BUILTIN_EFFECTS[] = {"alloc", "asm", "syscall"};

static int effect_name_is_reserved(const char *name) {
  return strcmp(name, "none") == 0 || strcmp(name, "unknown") == 0;
}

int type_checker_find_effect(const TypeChecker *checker, const char *name) {
  if (!checker || !name) {
    return -1;
  }
  for (size_t i = 0; i < checker->effect_count; i++) {
    if (strcmp(checker->effects[i].name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int type_checker_declare_effect(TypeChecker *checker, const char *name,
                                SourceLocation site, int is_exported,
                                int is_builtin) {
  TypeCheckerEffect *entry;
  if (!checker || !name) {
    return 0;
  }
  if (checker->effect_count == checker->effect_capacity) {
    size_t next = checker->effect_capacity ? checker->effect_capacity * 2 : 8;
    TypeCheckerEffect *grown =
        realloc(checker->effects, next * sizeof(TypeCheckerEffect));
    if (!grown) {
      return 0;
    }
    checker->effects = grown;
    checker->effect_capacity = next;
  }
  entry = &checker->effects[checker->effect_count++];
  entry->name = string_intern(name);
  entry->site = site;
  entry->is_exported = is_exported;
  entry->is_builtin = is_builtin;
  return 1;
}

void type_checker_declare_builtin_effects(TypeChecker *checker) {
  SourceLocation nowhere = source_location_create(0, 0);
  for (size_t i = 0; i < sizeof(BUILTIN_EFFECTS) / sizeof(BUILTIN_EFFECTS[0]);
       i++) {
    type_checker_declare_effect(checker, BUILTIN_EFFECTS[i], nowhere, 1, 1);
  }
}

int type_checker_register_effects(TypeChecker *checker, ASTNode *program) {
  Program *prog;
  int ok = 1;
  if (!checker || !program || program->type != AST_PROGRAM) {
    return 0;
  }
  prog = (Program *)program->data;
  if (!prog) {
    return 0;
  }
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    EffectDeclaration *effect;
    int existing;
    if (!decl || decl->type != AST_EFFECT_DECLARATION || !decl->data) {
      continue;
    }
    effect = (EffectDeclaration *)decl->data;
    if (!effect->name) {
      continue;
    }
    if (effect_name_is_reserved(effect->name)) {
      type_checker_set_error_at_location(
          checker, decl->location,
          "'%s' is reserved: `with none` names the empty set of effects and "
          "'unknown' is what a call the compiler cannot follow performs",
          effect->name);
      ok = 0;
      continue;
    }
    existing = type_checker_find_effect(checker, effect->name);
    if (existing >= 0 && checker->effects[existing].is_builtin) {
      type_checker_set_error_at_location(
          checker, decl->location,
          "'%s' is a built-in effect; the compiler already knows what "
          "performs it",
          effect->name);
      ok = 0;
      continue;
    }
    if (existing >= 0) {
      type_checker_set_error_at_location(checker, decl->location,
                                         "effect '%s' is declared twice",
                                         effect->name);
      if (checker->error_reporter) {
        error_reporter_add_note_of_span(
            checker->error_reporter,
            source_span_from_location(checker->effects[existing].site, 6),
            "first declared here");
      }
      ok = 0;
      continue;
    }
    if (!type_checker_declare_effect(checker, effect->name, decl->location,
                                     effect->is_exported, 0)) {
      return 0;
    }
  }
  return ok;
}

static int clause_names_ok(TypeChecker *checker, ASTNode *declaration,
                           const char *function, const char *clause,
                           char **names, size_t count, int allow_none) {
  for (size_t i = 0; i < count; i++) {
    if (!names[i]) {
      continue;
    }
    if (strcmp(names[i], "none") == 0) {
      if (!allow_none || count != 1) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "'with none' says a function performs no effect at all, so it "
            "stands alone and belongs on an extern or a function type; a "
            "body's effects are inferred");
        return 0;
      }
      continue;
    }
    if (type_checker_find_effect(checker, names[i]) < 0) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "unknown effect '%s' in the '%s' clause of '%s'; declare it with "
          "`effect %s;`",
          names[i], clause, function, names[i]);
      return 0;
    }
    for (size_t j = 0; j < i; j++) {
      if (names[j] && strcmp(names[j], names[i]) == 0) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "'%s' is listed twice in the '%s' clause of '%s'", names[i], clause,
            function);
        return 0;
      }
    }
  }
  return 1;
}

static int clauses_overlap(char **left, size_t left_count, char **right,
                           size_t right_count, const char **out_name) {
  for (size_t i = 0; i < left_count; i++) {
    for (size_t j = 0; j < right_count; j++) {
      if (left[i] && right[j] && strcmp(left[i], right[j]) == 0) {
        *out_name = left[i];
        return 1;
      }
    }
  }
  return 0;
}

int type_checker_validate_effect_clauses(TypeChecker *checker,
                                         ASTNode *declaration,
                                         FunctionDeclaration *fd) {
  const char *name;
  const char *overlap = NULL;
  int any;
  if (!checker || !declaration || !fd) {
    return 0;
  }
  name = fd->name ? fd->name : "?";
  any = fd->effects_with_count || fd->effects_forbids_count ||
        fd->effects_requires_count || fd->effects_provides_count;
  if (!any) {
    return 1;
  }
  if (fd->is_rule) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "a @rule runs while compiling and never becomes code, so it has no "
        "effects to declare");
    return 0;
  }
  if (fd->is_extern &&
      (fd->effects_forbids_count > 0 || fd->effects_provides_count > 0)) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "an extern function has no body to prove anything over, so it can "
        "neither forbid nor provide an effect; `with` says what '%s' "
        "performs and `requires` what it needs",
        name);
    return 0;
  }
  if (!clause_names_ok(checker, declaration, name, "with", fd->effects_with,
                       fd->effects_with_count, fd->is_extern) ||
      !clause_names_ok(checker, declaration, name, "forbids",
                       fd->effects_forbids, fd->effects_forbids_count, 0) ||
      !clause_names_ok(checker, declaration, name, "requires",
                       fd->effects_requires, fd->effects_requires_count, 0) ||
      !clause_names_ok(checker, declaration, name, "provides",
                       fd->effects_provides, fd->effects_provides_count, 0)) {
    return 0;
  }
  if (clauses_overlap(fd->effects_with, fd->effects_with_count,
                      fd->effects_forbids, fd->effects_forbids_count,
                      &overlap)) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "'%s' both performs and forbids '%s'", name, overlap);
    return 0;
  }
  if (clauses_overlap(fd->effects_requires, fd->effects_requires_count,
                      fd->effects_provides, fd->effects_provides_count,
                      &overlap)) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "'%s' both requires and provides '%s'; a function that provides an "
        "effect grants it to its body, so it cannot also need it from its "
        "caller",
        name, overlap);
    return 0;
  }
  return 1;
}

static void append_text(char *buffer, size_t size, size_t *length,
                        const char *text) {
  size_t take = strlen(text);
  if (*length + take >= size) {
    take = size - *length - 1;
  }
  memcpy(buffer + *length, text, take);
  *length += take;
  buffer[*length] = '\0';
}

const char *type_checker_effect_signature(const char *const *with,
                                          size_t with_count, int closed,
                                          const char *const *requires,
                                          size_t require_count) {
  char buffer[1024];
  size_t length = 0;
  buffer[0] = '\0';
  if (closed) {
    append_text(buffer, sizeof(buffer), &length, "with ");
    if (with_count == 0) {
      append_text(buffer, sizeof(buffer), &length, "none");
    }
    for (size_t i = 0; i < with_count; i++) {
      if (i) {
        append_text(buffer, sizeof(buffer), &length, ",");
      }
      append_text(buffer, sizeof(buffer), &length, with[i]);
    }
  }
  if (require_count > 0) {
    if (length) {
      append_text(buffer, sizeof(buffer), &length, " ");
    }
    append_text(buffer, sizeof(buffer), &length, "requires ");
    for (size_t i = 0; i < require_count; i++) {
      if (i) {
        append_text(buffer, sizeof(buffer), &length, ",");
      }
      append_text(buffer, sizeof(buffer), &length, requires[i]);
    }
  }
  if (length == 0) {
    return NULL;
  }
  return string_intern(buffer);
}

int type_checker_add_effect_obligation(TypeChecker *checker,
                                       const char *function,
                                       const char *signature,
                                       SourceLocation location) {
  TypeCheckerEffectObligation *entry;
  if (!checker || !function || !signature) {
    return 1;
  }
  if (checker->effect_obligation_count == checker->effect_obligation_capacity) {
    size_t next = checker->effect_obligation_capacity
                      ? checker->effect_obligation_capacity * 2
                      : 8;
    TypeCheckerEffectObligation *grown =
        realloc(checker->effect_obligations,
                next * sizeof(TypeCheckerEffectObligation));
    if (!grown) {
      return 0;
    }
    checker->effect_obligations = grown;
    checker->effect_obligation_capacity = next;
  }
  entry = &checker->effect_obligations[checker->effect_obligation_count++];
  entry->function = string_intern(function);
  entry->signature = signature;
  entry->location = location;
  return 1;
}

const char *type_checker_function_value_name(TypeChecker *checker,
                                             const ASTNode *expression) {
  if (!checker || !expression) {
    return NULL;
  }
  switch (expression->type) {
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expression->data;
    Symbol *symbol = id ? type_checker_resolve_identifier(checker, id) : NULL;
    if (symbol && symbol->kind == SYMBOL_FUNCTION) {
      return symbol->name;
    }
    return NULL;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    if (unary && unary->operator && strcmp(unary->operator, "&") == 0) {
      return type_checker_function_value_name(checker, unary->operand);
    }
    return NULL;
  }
  case AST_CLOSURE_ADAPT_EXPRESSION: {
    ClosureAdapt *adapt = (ClosureAdapt *)expression->data;
    return adapt ? type_checker_function_value_name(checker, adapt->inner)
                 : NULL;
  }
  case AST_LAMBDA_EXPRESSION: {
    FunctionDeclaration *lam = (FunctionDeclaration *)expression->data;
    if (!lam || !lam->name) {
      return NULL;
    }
    if (strncmp(lam->name, "__make_lam_", 11) == 0) {
      char body[48];
      snprintf(body, sizeof(body), "__lam_%s", lam->name + 11);
      return string_intern(body);
    }
    return lam->name;
  }
  default:
    return NULL;
  }
}

static int set_contains(const char **items, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(items[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

int type_checker_fn_effect_sets_equal(const Type *lhs, const Type *rhs) {
  if (lhs->fn_effects_closed != rhs->fn_effects_closed ||
      lhs->fn_effect_count != rhs->fn_effect_count ||
      lhs->fn_require_count != rhs->fn_require_count) {
    return 0;
  }
  for (size_t i = 0; i < lhs->fn_effect_count; i++) {
    if (!set_contains(rhs->fn_effects, rhs->fn_effect_count,
                      lhs->fn_effects[i])) {
      return 0;
    }
  }
  for (size_t i = 0; i < lhs->fn_require_count; i++) {
    if (!set_contains(rhs->fn_requires, rhs->fn_require_count,
                      lhs->fn_requires[i])) {
      return 0;
    }
  }
  return 1;
}

static void set_effect_failure(TypeChecker *checker, const char *text) {
  free(checker->effect_failure);
  checker->effect_failure = strdup(text);
}

static const char *signature_or_open(const Type *type) {
  return type->fn_effect_signature ? type->fn_effect_signature
                                   : "no effect clause";
}

int type_checker_fn_effects_flow(TypeChecker *checker, const Type *dest,
                                 const Type *src) {
  char message[512];
  if (dest->fn_effects_closed) {
    if (!src->fn_effects_closed) {
      snprintf(message, sizeof(message),
               "a function value of a type with %s cannot flow into a type "
               "declaring `%s`: nothing bounds what it performs. Give the "
               "source type a `with` clause, or pass the function by name so "
               "the compiler can check its effects",
               signature_or_open(src), dest->fn_effect_signature);
      set_effect_failure(checker, message);
      return 0;
    }
    for (size_t i = 0; i < src->fn_effect_count; i++) {
      if (!set_contains(dest->fn_effects, dest->fn_effect_count,
                        src->fn_effects[i])) {
        snprintf(message, sizeof(message),
                 "a function value that may perform '%s' cannot flow into a "
                 "type declaring `%s`",
                 src->fn_effects[i], dest->fn_effect_signature);
        set_effect_failure(checker, message);
        return 0;
      }
    }
  }
  for (size_t i = 0; i < src->fn_require_count; i++) {
    if (!set_contains(dest->fn_requires, dest->fn_require_count,
                      src->fn_requires[i])) {
      snprintf(message, sizeof(message),
               "a function value that requires '%s' cannot flow into a type "
               "with %s: whoever calls through it would not know to provide "
               "'%s'",
               src->fn_requires[i], signature_or_open(dest),
               src->fn_requires[i]);
      set_effect_failure(checker, message);
      return 0;
    }
  }
  return 1;
}

const char *type_checker_fn_type_effect_clause_start(const char *text) {
  int depth = 0;
  if (!text || strncmp(text, "fn(", 3) == 0 || strncmp(text, "Fn(", 3) == 0) {
    return NULL;
  }
  for (size_t i = 0; text[i]; i++) {
    char ch = text[i];
    if (ch == '(' || ch == '<' || ch == '[') {
      depth++;
    } else if (ch == ')' || ch == ']') {
      depth--;
    } else if (ch == '>' && (i == 0 || text[i - 1] != '-')) {
      depth--;
    } else if (ch == ' ' && depth == 0 &&
               (strncmp(text + i, " with ", 6) == 0 ||
                strncmp(text + i, " requires ", 10) == 0)) {
      return text + i;
    }
  }
  return NULL;
}

static int add_effect_name(const char ***items, size_t *count,
                           const char *name) {
  const char **grown;
  if (set_contains(*items, *count, name)) {
    return 1;
  }
  grown = realloc(*items, (*count + 1) * sizeof(const char *));
  if (!grown) {
    return 0;
  }
  *items = grown;
  (*items)[(*count)++] = string_intern(name);
  return 1;
}

int type_checker_fn_type_apply_effect_clauses(TypeChecker *checker, Type *fp,
                                              const char *clauses) {
  const char *p = clauses ? clauses : "";
  while (*p) {
    int is_with;
    const char *end;
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      break;
    }
    is_with = strncmp(p, "with ", 5) == 0;
    if (!is_with && strncmp(p, "requires ", 9) != 0) {
      type_checker_set_error(checker,
                             "a function type carries `with` and `requires` "
                             "clauses and nothing else after its return type");
      return 0;
    }
    p += is_with ? 5 : 9;
    end = p;
    while (*end && *end != ' ') {
      end++;
    }
    while (p < end) {
      const char *comma = p;
      char name[128];
      size_t take;
      while (comma < end && *comma != ',') {
        comma++;
      }
      take = (size_t)(comma - p);
      if (take == 0 || take >= sizeof(name)) {
        type_checker_set_error(checker, "malformed effect list in a function "
                                        "type");
        return 0;
      }
      memcpy(name, p, take);
      name[take] = '\0';
      if (is_with && strcmp(name, "none") == 0) {
        fp->fn_effects_closed = 1;
      } else if (type_checker_find_effect(checker, name) < 0) {
        type_checker_set_error(checker,
                               "unknown effect '%s' in a function type; "
                               "declare it with `effect %s;`",
                               name, name);
        return 0;
      } else if (!add_effect_name(is_with ? &fp->fn_effects : &fp->fn_requires,
                                  is_with ? &fp->fn_effect_count
                                          : &fp->fn_require_count,
                                  name)) {
        return 0;
      }
      p = comma < end ? comma + 1 : end;
    }
    if (is_with) {
      fp->fn_effects_closed = 1;
    }
    p = end;
  }
  fp->fn_effect_signature = type_checker_effect_signature(
      fp->fn_effects, fp->fn_effect_count, fp->fn_effects_closed,
      fp->fn_requires, fp->fn_require_count);
  return 1;
}

void type_checker_report_effect_failure(TypeChecker *checker,
                                        const ASTNode *src_expr,
                                        SourceLocation location) {
  if (!checker || !checker->effect_failure) {
    return;
  }
  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(checker->effect_failure);
  if (checker->error_reporter) {
    SourceLocation where = src_expr ? src_expr->location : location;
    size_t span_length = src_expr ? type_checker_node_span_length(src_expr) : 1;
    SourceSpan span = source_span_from_location(where, span_length);
    error_reporter_add_error_with_span(checker->error_reporter, ERROR_TYPE,
                                       span, checker->effect_failure);
    error_reporter_set_last_code(checker->error_reporter, "F0003");
    error_reporter_set_last_label(checker->error_reporter,
                                  "its effects do not fit the type");
  }
  free(checker->effect_failure);
  checker->effect_failure = NULL;
}
