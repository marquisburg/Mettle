/* mtlc_lower_module.c - fill the backend IR's type registry + module symbol
 * table from the frontend, so codegen needs neither the AST nor the frontend
 * type/symbol tables. FRONTEND-side adapter (driver, not libmtlc). */
#include "frontend/mtlc_lower_module.h"
#include "ir/ir_interp.h"
#include "frontend/mtlc_frontend.h" // mtlc_type_from_frontend
#include "common.h"                 // mettle_fnv1a_hash

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Register `name` -> its MtlcType in the program type registry, resolving it via
 * the frontend type checker (which parses primitives, structs/enums, and the
 * composite fn(...)->R / T[] / T* forms). No-op for names that are not types. */
static void register_named_type(IRProgram *program, TypeChecker *tc,
                                const char *name) {
  if (!name || !name[0] || ir_program_lookup_type(program, name)) {
    return;
  }
  Type *t = type_checker_get_type_by_name(tc, name);
  if (t) {
    ir_program_register_type(program, name, mtlc_type_from_frontend(t));
  }
}

/* Set of type-name strings already probed against the frontend, so each
 * distinct spelling is resolved once. Probing used to run for EVERY
 * instruction's text (labels, temps, call targets included), which put
 * millions of type_checker_get_type_by_name calls on the compile path of
 * large modules. */
typedef struct {
  const char **names; /* NULL = empty slot */
  size_t slot_count;  /* power of two; 0 = allocation failed, probe anyway */
  size_t count;
} ProbedNameSet;

/* Returns 1 when `name` was already present, 0 when newly added (or when the
 * set is unavailable, so the caller still probes). */
static int probed_set_check_and_add(ProbedNameSet *set, const char *name) {
  if (set->slot_count && set->count * 2 >= set->slot_count) {
    size_t next = set->slot_count * 2;
    const char **grown = (const char **)calloc(next, sizeof(*grown));
    if (grown) {
      for (size_t i = 0; i < set->slot_count; i++) {
        if (!set->names[i]) {
          continue;
        }
        size_t h = mettle_fnv1a_hash(set->names[i]) & (next - 1);
        while (grown[h]) {
          h = (h + 1) & (next - 1);
        }
        grown[h] = set->names[i];
      }
      free((void *)set->names);
      set->names = grown;
      set->slot_count = next;
    }
  }
  if (!set->names) {
    return 0;
  }
  size_t mask = set->slot_count - 1;
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (set->names[h]) {
    if (strcmp(set->names[h], name) == 0) {
      return 1;
    }
    h = (h + 1) & mask;
  }
  set->names[h] = name;
  set->count++;
  return 0;
}

/* Register every type name the code generators may resolve: the primitives (for
 * get_resolved_type's defaults) plus every type name that appears in the IR as a
 * function parameter type, return type, or a type-carrying instruction's text
 * (IR_OP_CAST target, IR_OP_DECLARE_LOCAL local type, IR_OP_NEW allocation
 * type). Other opcodes put labels, operators, and call targets in text - never
 * type names - so they are not probed. */
static void populate_type_registry(IRProgram *program, TypeChecker *tc) {
  static const char *const builtins[] = {
      "bool",   "int8",    "int16",   "int32",   "int64",
      "uint8",  "uint16",  "uint32",  "uint64",  "float32",
      "float64", "string", "cstring", "rawptr", "void"};
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    register_named_type(program, tc, builtins[i]);
  }

  ProbedNameSet probed = {0};
  probed.names = (const char **)calloc(256, sizeof(*probed.names));
  probed.slot_count = probed.names ? 256 : 0;

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    for (size_t p = 0; p < fn->parameter_count; p++) {
      if (fn->parameter_types && fn->parameter_types[p] &&
          !probed_set_check_and_add(&probed, fn->parameter_types[p])) {
        register_named_type(program, tc, fn->parameter_types[p]);
      }
    }
    if (fn->return_type_name &&
        !probed_set_check_and_add(&probed, fn->return_type_name)) {
      register_named_type(program, tc, fn->return_type_name);
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->op != IR_OP_CAST && in->op != IR_OP_DECLARE_LOCAL &&
          in->op != IR_OP_NEW) {
        continue;
      }
      if (in->text && !probed_set_check_and_add(&probed, in->text)) {
        register_named_type(program, tc, in->text);
      }
    }
  }
  free((void *)probed.names);
}

/* ------------------------------------------------------------------ */
/* Module symbol table + global initializer evaluation.                */
/* ------------------------------------------------------------------ */

/* A folded numeric constant, mirroring code_generator's BinaryNumericConstant.
 * A float value is stored as its double; callers reinterpret to bits for a
 * 32/64-bit float global. */
typedef struct {
  int is_float;
  long long int_value;
  double float_value;
} NumConst;

static void num_from_int(NumConst *c, long long v) {
  c->is_float = 0;
  c->int_value = v;
  c->float_value = (double)v;
}
static void num_from_double(NumConst *c, double v) {
  c->is_float = 1;
  c->int_value = (long long)v;
  c->float_value = v;
}

/* IEEE-754 width of a frontend type: 32/64 for float32/float64, else 0. */
static int frontend_type_float_bits(const Type *t) {
  if (!t) {
    return 0;
  }
  if (t->kind == TYPE_FLOAT32 && t->size == 4) {
    return 32;
  }
  if (t->kind == TYPE_FLOAT64 && t->size == 8) {
    return 64;
  }
  if ((t->kind == TYPE_FLOAT16 || t->kind == TYPE_BFLOAT16) && t->size == 2) {
    return 16;
  }
  return 0;
}

static int num_is_float(const NumConst *v, ASTNode *expression) {
  if (v && v->is_float) {
    return 1;
  }
  return expression && expression->resolved_type &&
         frontend_type_float_bits(expression->resolved_type) != 0;
}

/* Reinterpret a stored module-symbol initializer back into a NumConst for
 * identifier references inside another initializer. */
static int module_symbol_numeric(const IRProgram *program, const char *name,
                                  NumConst *out) {
  const IRModuleSymbol *s = ir_program_lookup_symbol(program, name);
  if (!s) {
    return 0;
  }
  if (s->kind == IR_MODSYM_CONSTANT) {
    num_from_int(out, s->const_value);
    return 1;
  }
  if (s->kind == IR_MODSYM_VARIABLE && s->has_initializer) {
    if (s->init_is_float) {
      double d;
      memcpy(&d, &s->init_bits, sizeof(d));
      num_from_double(out, d);
    } else {
      num_from_int(out, s->init_bits);
    }
    return 1;
  }
  /* A global we own with no initializer is .bss, so its compile-time value is
   * zero -- `var a: int32; var b: int32 = a + 1;` lays out b as 1, the value it
   * would hold at load. An extern's storage belongs to another object and an
   * unfoldable initializer has already failed, so neither folds. */
  if (s->kind == IR_MODSYM_VARIABLE && !s->is_extern && !s->has_initializer &&
      !s->has_unfoldable_initializer && !s->init_symbol_ref && !s->init_bytes) {
    num_from_int(out, 0);
    return 1;
  }
  return 0;
}

/* Evaluate a constant global initializer expression to a NumConst. Ported from
 * code_generator_binary_eval_numeric_global_initializer (globals.c), with
 * identifier references resolved against the module symbols added so far. */
/* Recognises `&identifier` as a global initializer. The address of another
 * module symbol -- a function for a dispatch table, or a global for an alias --
 * is only known at link time, so it cannot be folded to a constant. Returns the
 * referenced name (borrowed from the AST) or NULL. */
static const char *lower_module_address_of_symbol_name(ASTNode *expression) {
  UnaryExpression *unary = NULL;
  Identifier *identifier = NULL;

  if (!expression || expression->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  unary = (UnaryExpression *)expression->data;
  if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
    return NULL;
  }
  if (!unary->operand || unary->operand->type != AST_IDENTIFIER) {
    return NULL;
  }
  identifier = (Identifier *)unary->operand->data;
  if (!identifier || !identifier->name || identifier->name[0] == '\0') {
    return NULL;
  }
  return identifier->name;
}

static int eval_numeric(const IRProgram *program, TypeChecker *tc,
                        SymbolTable *st, ASTNode *expression,
                        NumConst *out) {
  if (!expression || !out) {
    return 0;
  }
  switch (expression->type) {
  case AST_FUNCTION_CALL: {
    /* `sizeof(T)` is a compile-time integer, so it can initialize a global just
     * as it can a const. It reaches here as a call node rather than a literal
     * (see ir_lower_expr.c), and without this case it would be reported
     * unfoldable and fail in codegen with no source location. No other call is
     * foldable: there is no module initializer to run one in. */
    CallExpression *call = (CallExpression *)expression->data;
    Identifier *type_id = NULL;
    Type *sized = NULL;
    if (!call || !call->function_name ||
        strcmp(call->function_name, "sizeof") != 0) {
      return 0;
    }
    if (call->argument_count != 1 || !call->arguments || !call->arguments[0] ||
        call->arguments[0]->type != AST_IDENTIFIER) {
      return 0;
    }
    type_id = (Identifier *)call->arguments[0]->data;
    sized = (tc && type_id && type_id->name)
                ? type_checker_get_type_by_name(tc, type_id->name)
                : NULL;
    if (!sized || sized->size > (size_t)LLONG_MAX) {
      return 0;
    }
    num_from_int(out, (long long)sized->size);
    return 1;
  }
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      return 0;
    }
    if (literal->is_float) {
      num_from_double(out, literal->float_value);
    } else {
      num_from_int(out, literal->int_value);
    }
    return 1;
  }
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    Symbol *symbol = NULL;
    if (!identifier || !identifier->name) {
      return 0;
    }
    if (module_symbol_numeric(program, identifier->name, out)) {
      return 1;
    }
    /* An enum member is a frontend constant, not a module symbol, so it never
     * reaches the module table -- resolve it here so `var s: Status = Ok;`
     * lays out its discriminant. */
    symbol = st ? symbol_table_lookup(st, identifier->name) : NULL;
    if (symbol && symbol->kind == SYMBOL_CONSTANT) {
      num_from_int(out, symbol->data.constant.value);
      return 1;
    }
    return 0;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    NumConst operand = {0};
    if (!unary || !unary->operator|| !unary->operand ||
        !eval_numeric(program, tc, st, unary->operand, &operand)) {
      return 0;
    }
    if (strcmp(unary->operator, "+") == 0) {
      *out = operand;
      return 1;
    }
    if (strcmp(unary->operator, "-") == 0) {
      if (num_is_float(&operand, expression)) {
        num_from_double(out, -(operand.is_float ? operand.float_value
                                                : (double)operand.int_value));
      } else {
        num_from_int(out, -operand.int_value);
      }
      return 1;
    }
    if (strcmp(unary->operator, "!") == 0) {
      int is_zero = operand.is_float ? (operand.float_value == 0.0)
                                     : (operand.int_value == 0);
      num_from_int(out, is_zero);
      return 1;
    }
    if (strcmp(unary->operator, "~") == 0 && !operand.is_float) {
      num_from_int(out, ~operand.int_value);
      return 1;
    }
    return 0;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    NumConst left = {0};
    NumConst right = {0};
    if (!binary || !binary->operator|| !binary->left || !binary->right ||
        !eval_numeric(program, tc, st, binary->left, &left) ||
        !eval_numeric(program, tc, st, binary->right, &right)) {
      return 0;
    }
    if (left.is_float || right.is_float || num_is_float(NULL, expression)) {
      double l = left.is_float ? left.float_value : (double)left.int_value;
      double r = right.is_float ? right.float_value : (double)right.int_value;
      const char *o = binary->operator;
      if (strcmp(o, "+") == 0) {
        num_from_double(out, l + r);
      } else if (strcmp(o, "-") == 0) {
        num_from_double(out, l - r);
      } else if (strcmp(o, "*") == 0) {
        num_from_double(out, l * r);
      } else if (strcmp(o, "/") == 0) {
        num_from_double(out, l / r);
      } else if (strcmp(o, "==") == 0) {
        num_from_int(out, l == r);
      } else if (strcmp(o, "!=") == 0) {
        num_from_int(out, l != r);
      } else if (strcmp(o, "<") == 0) {
        num_from_int(out, l < r);
      } else if (strcmp(o, "<=") == 0) {
        num_from_int(out, l <= r);
      } else if (strcmp(o, ">") == 0) {
        num_from_int(out, l > r);
      } else if (strcmp(o, ">=") == 0) {
        num_from_int(out, l >= r);
      } else {
        return 0;
      }
      return 1;
    }
    long long l = left.int_value, r = right.int_value;
    const char *o = binary->operator;
    if (strcmp(o, "+") == 0) {
      num_from_int(out, l + r);
    } else if (strcmp(o, "-") == 0) {
      num_from_int(out, l - r);
    } else if (strcmp(o, "*") == 0) {
      num_from_int(out, l * r);
    } else if (strcmp(o, "/") == 0) {
      if (r == 0) {
        return 0;
      }
      num_from_int(out, l / r);
    } else if (strcmp(o, "%") == 0) {
      if (r == 0) {
        return 0;
      }
      num_from_int(out, l % r);
    } else if (strcmp(o, "==") == 0) {
      num_from_int(out, l == r);
    } else if (strcmp(o, "!=") == 0) {
      num_from_int(out, l != r);
    } else if (strcmp(o, "<") == 0) {
      num_from_int(out, l < r);
    } else if (strcmp(o, "<=") == 0) {
      num_from_int(out, l <= r);
    } else if (strcmp(o, ">") == 0) {
      num_from_int(out, l > r);
    } else if (strcmp(o, ">=") == 0) {
      num_from_int(out, l >= r);
    } else if (strcmp(o, "&&") == 0) {
      num_from_int(out, l != 0 && r != 0);
    } else if (strcmp(o, "||") == 0) {
      num_from_int(out, l != 0 || r != 0);
    } else if (strcmp(o, "&") == 0) {
      num_from_int(out, l & r);
    } else if (strcmp(o, "|") == 0) {
      num_from_int(out, l | r);
    } else if (strcmp(o, "^") == 0) {
      num_from_int(out, l ^ r);
    } else {
      return 0;
    }
    return 1;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    NumConst operand = {0};
    if (!cast || !cast->operand ||
        !eval_numeric(program, tc, st, cast->operand, &operand)) {
      return 0;
    }
    int target_float_bits =
        expression->resolved_type
            ? frontend_type_float_bits(expression->resolved_type)
            : 0;
    if (target_float_bits != 0) {
      num_from_double(out, operand.is_float ? operand.float_value
                                            : (double)operand.int_value);
    } else {
      num_from_int(out, operand.is_float ? (long long)operand.float_value
                                         : operand.int_value);
    }
    return 1;
  }
  default:
    return 0;
  }
}

/* Build a borrowed-MtlcType parameter array for a function symbol. Returns a
 * malloc'd array (the caller frees it after ir_program_add_symbol copies it) and
 * sets *count; NULL when the function has no parameters. */
static MtlcType **build_param_types(const Symbol *s, size_t *count) {
  *count = 0;
  if (!s || s->kind != SYMBOL_FUNCTION ||
      s->data.function.parameter_count == 0 ||
      !s->data.function.parameter_types) {
    return NULL;
  }
  size_t n = s->data.function.parameter_count;
  MtlcType **arr = (MtlcType **)malloc(n * sizeof(MtlcType *));
  if (!arr) {
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    arr[i] = mtlc_type_from_frontend(s->data.function.parameter_types[i]);
  }
  *count = n;
  return arr;
}

/* Open-addressing set of the program's lowered function names, built once per
 * populate_module_symbols call. The previous per-declaration linear scan over
 * program->functions made module population quadratic in function count
 * (13k-function fixtures spent seconds in strcmp here). */
typedef struct {
  const char **names; /* NULL = empty slot */
  size_t slot_count;  /* power of two, 0 when allocation failed */
} FnBodySet;

static void fn_body_set_build(FnBodySet *set, const IRProgram *program) {
  set->names = NULL;
  set->slot_count = 0;
  size_t slot_count = 16;
  while (slot_count < program->function_count * 2) {
    slot_count *= 2;
  }
  const char **names = (const char **)calloc(slot_count, sizeof(*names));
  if (!names) {
    return; /* lookups fall back to the linear scan */
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (!program->functions[i] || !program->functions[i]->name) {
      continue;
    }
    const char *name = program->functions[i]->name;
    size_t mask = slot_count - 1;
    size_t h = mettle_fnv1a_hash(name) & mask;
    while (names[h] && strcmp(names[h], name) != 0) {
      h = (h + 1) & mask;
    }
    names[h] = name;
  }
  set->names = names;
  set->slot_count = slot_count;
}

static void fn_body_set_free(FnBodySet *set) {
  free((void *)set->names);
  set->names = NULL;
  set->slot_count = 0;
}

static int program_has_function_body(const IRProgram *program,
                                     const FnBodySet *set, const char *name) {
  if (set->names) {
    size_t mask = set->slot_count - 1;
    size_t h = mettle_fnv1a_hash(name) & mask;
    while (set->names[h]) {
      if (strcmp(set->names[h], name) == 0) {
        return 1;
      }
      h = (h + 1) & mask;
    }
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->name &&
        strcmp(program->functions[i]->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void populate_function_symbol(IRProgram *program,
                                    const FnBodySet *body_set, SymbolTable *st,
                                    FunctionDeclaration *fd) {
  Symbol *s = symbol_table_lookup(st, fd->name);
  IRModuleSymbol entry = {0};
  MtlcType **params;
  size_t pc = 0;

  entry.name = fd->name;
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = fd->is_extern;
  entry.is_kernel = fd->is_kernel;
  /* Whether the MODULE defines this function, not whether this particular
   * declaration carries the body: a forward declaration and its later
   * definition each add a symbol entry, and lookups find the first. Asking
   * fd->body made the forward declaration's entry claim there was no body, so
   * a backend that skips body-less symbols skipped the definition. */
  entry.has_body = program_has_function_body(program, body_set, fd->name);
  entry.link_name = s ? s->link_name : NULL;
  entry.type = s ? mtlc_type_from_frontend(s->type) : NULL;
  if (s && s->kind == SYMBOL_FUNCTION) {
    entry.return_type = mtlc_type_from_frontend(s->data.function.return_type);
  }
  params = build_param_types(s, &pc);
  entry.param_types = params;
  entry.param_count = pc;
  char clause[512];
  clause[0] = '\0';
  if (fd->effects_with_count > 0) {
    size_t used = 0;
    for (size_t i = 0; i < fd->effects_with_count && used + 2 < sizeof(clause);
         i++) {
      int wrote = snprintf(clause + used, sizeof(clause) - used, "%s%s",
                           i ? "," : "", fd->effects_with[i]);
      if (wrote < 0) {
        break;
      }
      used += (size_t)wrote;
    }
    entry.effect_clause = clause;
  }
  ir_program_add_symbol(program, &entry); /* copies param_types */
  free(params);
}

/* An aggregate literal was already folded to its laid-out bytes when the type
 * checker validated it against the declared type, so the image just moves
 * across. Codegen blits it and emits the relocations; nothing re-walks the
 * AST. */
static IRInitReloc *populate_aggregate_initializer(IRModuleSymbol *entry,
                                                   ASTNode *initializer) {
  AggregateLiteral *literal = (AggregateLiteral *)initializer->data;
  IRInitReloc *relocs;

  if (!literal || !literal->image) {
    entry->has_unfoldable_initializer = 1;
    return NULL;
  }
  entry->init_bytes = literal->image;
  entry->init_bytes_size = literal->image_size;
  if (literal->reloc_count == 0) {
    return NULL;
  }
  relocs = calloc(literal->reloc_count, sizeof(IRInitReloc));
  if (!relocs) {
    return NULL;
  }
  for (size_t r = 0; r < literal->reloc_count; r++) {
    relocs[r].offset = literal->relocs[r].offset;
    relocs[r].symbol = literal->relocs[r].symbol;
    relocs[r].string = literal->relocs[r].string;
    relocs[r].string_length = literal->relocs[r].string_length;
    relocs[r].string_wants_record = literal->relocs[r].string_wants_record;
  }
  entry->init_relocs = relocs;
  entry->init_reloc_count = literal->reloc_count;
  return relocs;
}

/* A `const` whose initializer is a call to a function the program wrote. The
 * interpreter runs it here, where the function's IR exists and nothing has run
 * yet, and the answer becomes the bytes in the object file. The budget is the
 * interpreter's fuel: a table that does not finish computing is a build that
 * says so rather than one that hangs.
 *
 * This is the same interpreter that runs `@test`, holds the optimizer to
 * `--verify` and runs the rules. There is no second evaluator with a smaller
 * language in it, which is the point: any function the interpreter can run can
 * compute a constant. */
#define MTLC_CONST_CALL_FUEL 20000000LL

static long long g_const_call_steps;

long long mtlc_lower_const_call_steps(void) { return g_const_call_steps; }

static IRFunction *const_call_target(IRProgram *program,
                                     const ASTNode *initializer,
                                     const CallExpression **out_call) {
  const CallExpression *call;
  if (!initializer || initializer->type != AST_FUNCTION_CALL ||
      !initializer->data) {
    return NULL;
  }
  call = (const CallExpression *)initializer->data;
  if (!call->function_name || call->object) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && fn->instruction_count > 0 &&
        strcmp(fn->name, call->function_name) == 0) {
      *out_call = call;
      return fn;
    }
  }
  return NULL;
}

static int const_call_arguments(TypeChecker *tc, SymbolTable *st,
                                IRProgram *program, const CallExpression *call,
                                IRInterpValue *args, size_t capacity,
                                size_t *out_count) {
  (void)st;
  (void)program;
  if (call->argument_count > capacity) {
    return 0;
  }
  for (size_t i = 0; i < call->argument_count; i++) {
    ComptimeValue folded = comptime_none();
    if (!type_checker_eval_comptime(tc, call->arguments[i], &folded)) {
      return 0;
    }
    memset(&args[i], 0, sizeof(args[i]));
    if (folded.kind == COMPTIME_INT) {
      args[i].i = folded.as.int_value;
    } else if (folded.kind == COMPTIME_FLOAT) {
      args[i].is_float = 1;
      args[i].f = folded.as.float_value;
    } else {
      return 0;
    }
  }
  *out_count = call->argument_count;
  return 1;
}

/* Run the call and answer what it returned. `out_bytes` is filled for an
   aggregate result the caller asked to lay out. */
static int const_call_run(IRProgram *program, TypeChecker *tc, SymbolTable *st,
                          const ASTNode *initializer, IRInterpValue *out_value,
                          IRInterpMachine **out_machine) {
  const CallExpression *call = NULL;
  IRFunction *fn = const_call_target(program, initializer, &call);
  IRInterpValue args[8];
  size_t arg_count = 0;
  IRInterpMachine *machine;
  IRInterpStatus status;
  if (!fn || !call) {
    return 0;
  }
  if (fn->parameter_count != call->argument_count) {
    return 0;
  }
  machine = ir_interp_create(program);
  if (!machine) {
    return 0;
  }
  if (!const_call_arguments(tc, st, program, call, args,
                            sizeof(args) / sizeof(args[0]), &arg_count)) {
    ir_interp_destroy(machine);
    return 0;
  }
  memset(out_value, 0, sizeof(*out_value));
  status = ir_interp_run(machine, fn, args, arg_count, out_value,
                         MTLC_CONST_CALL_FUEL);
  g_const_call_steps +=
      MTLC_CONST_CALL_FUEL - ir_interp_fuel_remaining(machine);
  if (status != IR_INTERP_OK) {
    ir_interp_destroy(machine);
    return 0;
  }
  if (out_machine) {
    *out_machine = machine;
  } else {
    ir_interp_destroy(machine);
  }
  return 1;
}

static void populate_scalar_initializer(IRProgram *program, TypeChecker *tc,
                                        SymbolTable *st, IRModuleSymbol *entry,
                                        ASTNode *initializer) {
  NumConst c = {0};
  const char *addressed;

  if (entry->type && entry->type->kind == MTLC_TYPE_STRING) {
    if (initializer->type == AST_STRING_LITERAL) {
      StringLiteral *lit = (StringLiteral *)initializer->data;
      entry->has_initializer = 1;
      entry->init_string = lit && lit->value ? lit->value : "";
      entry->init_string_length = lit && lit->value ? lit->length : 0;
    } else {
      entry->has_unfoldable_initializer = 1;
    }
    return;
  }

  addressed = lower_module_address_of_symbol_name(initializer);
  if (eval_numeric(program, tc, st, initializer, &c)) {
    entry->has_initializer = 1;
    entry->init_is_float = c.is_float;
    if (c.is_float) {
      double d = c.float_value;
      memcpy(&entry->init_bits, &d, sizeof(d));
    } else {
      entry->init_bits = c.int_value;
    }
  } else if (addressed) {
    /* `&other_symbol`: the address is a link-time value, so record the name
     * and let the backend emit a relocation. */
    entry->init_symbol_ref = (char *)addressed;
  } else {
    IRInterpValue answered;
    if (const_call_run(program, tc, st, initializer, &answered, NULL)) {
      entry->has_initializer = 1;
      entry->init_is_float = answered.is_float;
      if (answered.is_float) {
        double d = answered.f;
        memcpy(&entry->init_bits, &d, sizeof(d));
      } else {
        entry->init_bits = answered.i;
      }
      return;
    }
    entry->has_unfoldable_initializer = 1;
  }
}

/* An array `const` computed by a function: the interpreter runs it, and the
   buffer it returns becomes the object file's bytes. */
static int populate_computed_array(IRProgram *program, TypeChecker *tc,
                                   SymbolTable *st, IRModuleSymbol *entry,
                                   ASTNode *initializer, const Type *vtype) {
  IRInterpValue answered;
  IRInterpMachine *machine = NULL;
  size_t bytes;
  unsigned char *image;
  if (!vtype || vtype->size == 0 ||
      (vtype->kind != TYPE_ARRAY && vtype->kind != TYPE_STRUCT)) {
    return 0;
  }
  if (!const_call_run(program, tc, st, initializer, &answered, &machine)) {
    return 0;
  }
  bytes = vtype->size;
  image = calloc(bytes ? bytes : 1, 1);
  if (!image) {
    ir_interp_destroy(machine);
    return 0;
  }
  {
    long long got = ir_interp_read_bytes(
        machine, (unsigned long long)answered.i, image, bytes);
    if (got != (long long)bytes) {
      free(image);
      ir_interp_destroy(machine);
      return 0;
    }
  }
  ir_interp_destroy(machine);
  entry->init_bytes = image;
  entry->init_bytes_size = bytes;
  return 1;
}

static void populate_variable_symbol(IRProgram *program, TypeChecker *tc,
                                     SymbolTable *st, VarDeclaration *vd) {
  Symbol *s = symbol_table_lookup(st, vd->name);
  IRModuleSymbol entry = {0};
  IRInitReloc *aggregate_relocs = NULL;

  entry.name = vd->name;
  entry.is_extern = vd->is_extern;
  entry.link_name = s ? s->link_name : NULL;
  if (s && s->kind == SYMBOL_CONSTANT) {
    /* Type/Field reflection consts have no runtime representation and must not
     * become module symbols. */
    if (type_is_comptime_only(s->type)) {
      return;
    }
    entry.kind = IR_MODSYM_CONSTANT;
    entry.const_value = s->data.constant.value;
    entry.type = mtlc_type_from_frontend(s->type);
  } else {
    Type *vtype = s ? s->type : NULL;
    entry.kind = IR_MODSYM_VARIABLE;
    entry.is_immutable = s ? s->is_immutable : 0;
    entry.is_exported = vd->is_exported;
    if (!vtype && vd->type_name) {
      vtype = type_checker_get_type_by_name(tc, vd->type_name);
    }
    entry.type = mtlc_type_from_frontend(vtype);
    entry.is_volatile = vtype && vtype->is_volatile ? 1 : 0;
    if (!vd->is_extern && vd->initializer) {
      if (vd->initializer->type == AST_AGGREGATE_LITERAL) {
        aggregate_relocs =
            populate_aggregate_initializer(&entry, vd->initializer);
      } else if (vtype && (vtype->kind == TYPE_ARRAY ||
                           vtype->kind == TYPE_STRUCT)) {
        if (!populate_computed_array(program, tc, st, &entry, vd->initializer,
                                     vtype)) {
          entry.has_unfoldable_initializer = 1;
        }
      } else {
        populate_scalar_initializer(program, tc, st, &entry, vd->initializer);
      }
    }
  }
  ir_program_add_symbol(program, &entry); /* deep-copies the image */
  free(aggregate_relocs);
}

/* Register user-defined named types so codegen can resolve them. */
static void populate_named_type_symbol(IRProgram *program, TypeChecker *tc,
                                       SymbolTable *st, ASTNode *decl) {
  Symbol *s = NULL;
  if (decl->type == AST_STRUCT_DECLARATION) {
    StructDeclaration *sd = (StructDeclaration *)decl->data;
    if (sd && sd->name) {
      s = symbol_table_lookup(st, sd->name);
    }
  } else {
    EnumDeclaration *ed = (EnumDeclaration *)decl->data;
    if (ed && ed->name) {
      s = symbol_table_lookup(st, ed->name);
    }
  }
  if (s && s->name && s->type) {
    register_named_type(program, tc, s->name);
  }
}

static void populate_module_symbols(IRProgram *program, ASTNode *ast_program,
                                    TypeChecker *tc, SymbolTable *st) {
  Program *pdata = (Program *)ast_program->data;
  FnBodySet body_set;

  if (!pdata) {
    return;
  }
  fn_body_set_build(&body_set, program);
  for (size_t i = 0; i < pdata->declaration_count; i++) {
    ASTNode *decl = pdata->declarations[i];
    if (!decl) {
      continue;
    }
    if (decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      /* `extern kernel` names a device entry point, not a host symbol. It
       * exists so `dispatch` can check its arguments; the handle is resolved
       * from the loaded GPU module at run time. Emitting a module symbol for
       * it would make the host linker look for a definition that is, by
       * construction, on the other side of the PTX boundary. */
      if (fd && fd->name && !(fd->is_extern && fd->is_kernel)) {
        populate_function_symbol(program, &body_set, st, fd);
      }
    } else if (decl->type == AST_VAR_DECLARATION) {
      VarDeclaration *vd = (VarDeclaration *)decl->data;
      if (vd && vd->name) {
        populate_variable_symbol(program, tc, st, vd);
      }
    } else if (decl->type == AST_STRUCT_DECLARATION ||
               decl->type == AST_ENUM_DECLARATION) {
      populate_named_type_symbol(program, tc, st, decl);
    }
  }
  fn_body_set_free(&body_set);
}

/* main() takes (argc, argv) when its lowered signature has two parameters. */
static void populate_main_flag(IRProgram *program) {
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && strcmp(fn->name, "main") == 0) {
      program->main_wants_argc_argv = (fn->parameter_count == 2) ? 1 : 0;
      return;
    }
  }
}

void mtlc_lower_populate_module(IRProgram *program, ASTNode *ast_program,
                                TypeChecker *tc, SymbolTable *st) {
  if (!program || !tc) {
    return;
  }
  populate_type_registry(program, tc);
  if (ast_program && st) {
    populate_module_symbols(program, ast_program, tc, st);
  }
  populate_main_flag(program);
}
