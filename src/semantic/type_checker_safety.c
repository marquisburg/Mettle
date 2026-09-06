// Type checker: constant evaluation, buffer-extent / alignment safety analysis.
#include "type_checker_internal.h"

int type_checker_is_lvalue_expression(ASTNode *expression) {
  if (!expression) {
    return 0;
  }

  switch (expression->type) {
  case AST_IDENTIFIER:
  case AST_MEMBER_ACCESS:
  case AST_INDEX_EXPRESSION:
    return 1;
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    return unary && unary->operator && strcmp(unary->operator, "*") == 0;
  }
  default:
    return 0;
  }
}

typedef struct {
  int is_float;
  long long int_value;
  double float_value;
} TypeCheckerConstant;

static void type_checker_constant_from_int(TypeCheckerConstant *value,
                                           long long int_value) {
  value->is_float = 0;
  value->int_value = int_value;
  value->float_value = (double)int_value;
}

static void type_checker_constant_from_float(TypeCheckerConstant *value,
                                             double float_value) {
  value->is_float = 1;
  value->int_value = (long long)float_value;
  value->float_value = float_value;
}

/* The width and signedness a cast to this type converts to, or 0 when the type
 * is not one an integer constant can land in. */
static int type_checker_integer_type_shape(const Type *type, int *bits_out,
                                           int *is_signed_out) {
  int bits = 0;
  int is_signed = 0;

  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_INT8: bits = 8; is_signed = 1; break;
  case TYPE_INT16: bits = 16; is_signed = 1; break;
  case TYPE_INT32: bits = 32; is_signed = 1; break;
  case TYPE_INT64: bits = 64; is_signed = 1; break;
  case TYPE_UINT8: bits = 8; break;
  case TYPE_UINT16: bits = 16; break;
  case TYPE_UINT32: bits = 32; break;
  case TYPE_UINT64: bits = 64; break;
  case TYPE_BOOL: bits = 8; break;
  case TYPE_CHAR: bits = 8; break;
  default:
    return 0;
  }
  *bits_out = bits;
  *is_signed_out = is_signed;
  return 1;
}

/* Fold the result of `expression` the way the machine would leave it.
 *
 * Arithmetic wraps at the expression's own type, and the constant evaluator
 * worked in 64 bits and never cut back, so a `const` and the same expression
 * written at run time gave different answers: `(uint8)2 - (uint8)249` is 9 on
 * the machine and folded to -247, `(int8)100 + (int8)100` is -56 and folded to
 * 200, and an untyped `2147483647 + 1`, which is int32 like every untyped
 * integer literal, folded to 2147483648. */
static void type_checker_wrap_constant_to_expression_type(
    TypeChecker *checker, ASTNode *expression, TypeCheckerConstant *value);

static long long type_checker_wrap_integer(long long value, int bits,
                                           int is_signed) {
  unsigned long long mask = bits >= 64
                                ? ~0ULL
                                : ((1ULL << bits) - 1ULL);
  unsigned long long bits_value = (unsigned long long)value & mask;

  if (is_signed && bits < 64 &&
      (bits_value & (1ULL << (bits - 1))) != 0ULL) {
    return (long long)(bits_value | ~mask);
  }
  return (long long)bits_value;
}

static void type_checker_wrap_constant_to_expression_type(
    TypeChecker *checker, ASTNode *expression, TypeCheckerConstant *value) {
  Type *type = NULL;
  int bits = 0;
  int is_signed = 0;

  if (!checker || !expression || !value || value->is_float) {
    return;
  }
  type = type_checker_infer_type(checker, expression);
  if (!type || !type_checker_integer_type_shape(type, &bits, &is_signed)) {
    return;
  }
  type_checker_constant_from_int(
      value, type_checker_wrap_integer(value->int_value, bits, is_signed));
}

static int type_checker_eval_numeric_constant(TypeChecker *checker,
                                              ASTNode *expression,
                                              TypeCheckerConstant *out_value) {
  if (!expression || !out_value) {
    return 0;
  }

  switch (expression->type) {
  /* `(int8)(-51)` is as constant as `-51`, and writing the type is how a
   * program says which one it means. Without this case every constant context
   * -- a `const` initializer, an array size, a `case` label, static_assert --
   * rejected a cast with "must be a compile-time integer constant
   * expression", which is exactly what it is. */
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    TypeCheckerConstant operand = {0};
    Type *target = NULL;
    int bits = 0;
    int is_signed = 0;

    if (!cast || !cast->type_name || !cast->operand || !checker ||
        !type_checker_eval_numeric_constant(checker, cast->operand,
                                            &operand)) {
      return 0;
    }
    target = type_checker_get_type_by_name(checker, cast->type_name);
    if (!target) {
      return 0;
    }
    if (type_checker_is_floating_type(target)) {
      double value = operand.is_float ? operand.float_value
                                      : (double)operand.int_value;
      if (target->kind == TYPE_FLOAT32) {
        value = (double)(float)value;
      }
      type_checker_constant_from_float(out_value, value);
      return 1;
    }
    if (!type_checker_integer_type_shape(target, &bits, &is_signed)) {
      return 0;
    }
    {
      /* A float to an integer rounds toward zero, the same direction the
       * generated cast takes. */
      long long value = operand.is_float ? (long long)operand.float_value
                                         : operand.int_value;
      type_checker_constant_from_int(
          out_value, type_checker_wrap_integer(value, bits, is_signed));
    }
    return 1;
  }

  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal || literal->is_float) {
      if (!literal) {
        return 0;
      }
      type_checker_constant_from_float(out_value, literal->float_value);
      return 1;
    }
    type_checker_constant_from_int(out_value, literal->int_value);
    return 1;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      return 0;
    }

    Symbol *symbol = checker
                         ? type_checker_resolve_identifier(checker, identifier)
                         : NULL;
    if (!symbol || (symbol->kind != SYMBOL_CONSTANT &&
                    !symbol->has_constant_value)) {
      return 0;
    }

    if (symbol->has_constant_value && symbol->constant_is_float) {
      type_checker_constant_from_float(out_value,
                                       symbol->constant_float_value);
    } else {
      long long value = symbol->has_constant_value
                            ? symbol->constant_integer_value
                            : symbol->data.constant.value;
      type_checker_constant_from_int(out_value, value);
    }
    return 1;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (!call || !call->function_name) {
      return 0;
    }
    if (strcmp(call->function_name, "offsetof") == 0) {
      long long offset = 0;
      if (!type_checker_eval_offsetof(checker, call, expression->location,
                                      &offset)) {
        return 0;
      }
      type_checker_constant_from_int(out_value, offset);
      return 1;
    }
    if (strcmp(call->function_name, "layoutof") == 0) {
      long long digest = 0;
      if (!type_checker_eval_layoutof(checker, call, expression->location,
                                      &digest)) {
        return 0;
      }
      type_checker_constant_from_int(out_value, digest);
      return 1;
    }
    if (strcmp(call->function_name, "sizeof") != 0 ||
        call->argument_count != 1 || !call->arguments[0] ||
        call->arguments[0]->type != AST_IDENTIFIER) {
      return 0;
    }

    Identifier *type_id = (Identifier *)call->arguments[0]->data;
    Type *type = (checker && type_id)
                     ? type_checker_get_type_by_name(checker, type_id->name)
                     : NULL;
    if (!type || type->size > (size_t)LLONG_MAX) {
      return 0;
    }

    type_checker_constant_from_int(out_value, (long long)type->size);
    return 1;
  }

  /* `f.offset` and friends: a Field member is a compile-time integer, so it
   * belongs in every constant context sizeof and offsetof already reach. */
  case AST_MEMBER_ACCESS: {
    ComptimeValue folded = comptime_none();
    if (!checker ||
        !type_checker_eval_comptime(checker, expression, &folded)) {
      return 0;
    }
    if (folded.kind == COMPTIME_INT) {
      type_checker_constant_from_int(out_value, folded.as.int_value);
      return 1;
    }
    if (folded.kind == COMPTIME_FLOAT) {
      type_checker_constant_from_float(out_value, folded.as.float_value);
      return 1;
    }
    return 0;
  }

  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary_expr = (UnaryExpression *)expression->data;
    TypeCheckerConstant operand = {0};
    if (!unary_expr || !unary_expr->operator || !unary_expr->operand ||
        !type_checker_eval_numeric_constant(
            checker, unary_expr->operand, &operand)) {
      return 0;
    }

    if (strcmp(unary_expr->operator, "+") == 0) {
      *out_value = operand;
      return 1;
    }
    if (strcmp(unary_expr->operator, "-") == 0) {
      if (operand.is_float) {
        type_checker_constant_from_float(out_value, -operand.float_value);
      } else {
        type_checker_constant_from_int(out_value, -operand.int_value);
        type_checker_wrap_constant_to_expression_type(checker, expression,
                                                      out_value);
      }
      return 1;
    }
    if (strcmp(unary_expr->operator, "!") == 0) {
      int is_zero = operand.is_float ? operand.float_value == 0.0
                                     : operand.int_value == 0;
      type_checker_constant_from_int(out_value, is_zero);
      return 1;
    }
    if (strcmp(unary_expr->operator, "~") == 0 && !operand.is_float) {
      type_checker_constant_from_int(out_value, ~operand.int_value);
      type_checker_wrap_constant_to_expression_type(checker, expression,
                                                    out_value);
      return 1;
    }
    return 0;
  }

  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    TypeCheckerConstant left = {0};
    TypeCheckerConstant right = {0};
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right) {
      return 0;
    }

    /* Compile-time strings compare for equality. A `.name` read off the field
     * table is how a metaprogram asks whether two declarations agree, so the
     * comparison folds to 0 or 1 here instead of reaching the backend, which
     * has no compile-time string to compare. Attempted before the numeric
     * fold because a string operand is not a number and would fail it. */
    if (checker && (strcmp(binary_expr->operator, "==") == 0 ||
                    strcmp(binary_expr->operator, "!=") == 0)) {
      ComptimeValue left_string = comptime_none();
      ComptimeValue right_string = comptime_none();
      if (type_checker_eval_comptime(checker, binary_expr->left,
                                     &left_string) &&
          left_string.kind == COMPTIME_STRING && left_string.as.string.value &&
          type_checker_eval_comptime(checker, binary_expr->right,
                                     &right_string) &&
          right_string.kind == COMPTIME_STRING &&
          right_string.as.string.value) {
        int equal = strcmp(left_string.as.string.value,
                           right_string.as.string.value) == 0;
        type_checker_constant_from_int(
            out_value, binary_expr->operator[0] == '!' ? !equal : equal);
        return 1;
      }
    }

    if (!type_checker_eval_numeric_constant(
            checker, binary_expr->left, &left) ||
        !type_checker_eval_numeric_constant(
            checker, binary_expr->right, &right)) {
      return 0;
    }

    if (left.is_float || right.is_float) {
      double left_value = left.is_float ? left.float_value
                                        : (double)left.int_value;
      double right_value = right.is_float ? right.float_value
                                          : (double)right.int_value;
      const char *operator = binary_expr->operator;
      if (strcmp(operator, "+") == 0) {
        type_checker_constant_from_float(out_value,
                                         left_value + right_value);
        return 1;
      }
      if (strcmp(operator, "-") == 0) {
        type_checker_constant_from_float(out_value,
                                         left_value - right_value);
        return 1;
      }
      if (strcmp(operator, "*") == 0) {
        type_checker_constant_from_float(out_value,
                                         left_value * right_value);
        return 1;
      }
      if (strcmp(operator, "/") == 0) {
        if (right_value == 0.0) {
          return 0;
        }
        type_checker_constant_from_float(out_value,
                                         left_value / right_value);
        return 1;
      }
      if (strcmp(operator, "==") == 0) {
        type_checker_constant_from_int(out_value, left_value == right_value);
        return 1;
      }
      if (strcmp(operator, "!=") == 0) {
        type_checker_constant_from_int(out_value, left_value != right_value);
        return 1;
      }
      if (strcmp(operator, "<") == 0) {
        type_checker_constant_from_int(out_value, left_value < right_value);
        return 1;
      }
      if (strcmp(operator, "<=") == 0) {
        type_checker_constant_from_int(out_value, left_value <= right_value);
        return 1;
      }
      if (strcmp(operator, ">") == 0) {
        type_checker_constant_from_int(out_value, left_value > right_value);
        return 1;
      }
      if (strcmp(operator, ">=") == 0) {
        type_checker_constant_from_int(out_value, left_value >= right_value);
        return 1;
      }
      if (strcmp(operator, "&&") == 0) {
        type_checker_constant_from_int(
            out_value, (left_value != 0.0) && (right_value != 0.0));
        return 1;
      }
      if (strcmp(operator, "||") == 0) {
        type_checker_constant_from_int(
            out_value, (left_value != 0.0) || (right_value != 0.0));
        return 1;
      }
      return 0;
    }

    long long left_value = left.int_value;
    long long right_value = right.int_value;
    unsigned long long left_bits = (unsigned long long)left_value;
    unsigned long long right_bits = (unsigned long long)right_value;
    const char *operator = binary_expr->operator;
    /* The width this expression's result lives in, when it can be worked out.
     * Every arithmetic fold below wraps to it, because that is what the
     * machine leaves behind and a `const` that says otherwise is a different
     * program from the same expression written at run time. Without a shape
     * the folds keep the guards they had, which decline rather than guess. */
    int bits = 0;
    int is_signed = 0;
    Type *result_type = checker ? type_checker_infer_type(checker, expression)
                                : NULL;
    int shaped =
        type_checker_integer_type_shape(result_type, &bits, &is_signed);

    if (strcmp(operator, "+") == 0 || strcmp(operator, "-") == 0 ||
        strcmp(operator, "*") == 0) {
      unsigned long long folded = operator[0] == '+' ? left_bits + right_bits
                                 : operator[0] == '-' ? left_bits - right_bits
                                                      : left_bits * right_bits;
      type_checker_constant_from_int(out_value, (long long)folded);
      if (shaped) {
        type_checker_constant_from_int(
            out_value, type_checker_wrap_integer((long long)folded, bits,
                                                 is_signed));
      }
      return 1;
    }
    if (strcmp(operator, "/") == 0 || strcmp(operator, "%") == 0) {
      long long quotient = 0;
      if (right_value == 0) {
        return 0;
      }
      if (shaped && !is_signed) {
        unsigned long long l = (unsigned long long)type_checker_wrap_integer(
            left_value, bits, 0);
        unsigned long long r = (unsigned long long)type_checker_wrap_integer(
            right_value, bits, 0);
        if (r == 0) {
          return 0;
        }
        quotient = (long long)(operator[0] == '/' ? l / r : l % r);
      } else {
        /* The signed minimum over -1 has no result to fold; the machine
         * faults on it. */
        if (left_value == LLONG_MIN && right_value == -1) {
          return 0;
        }
        quotient = operator[0] == '/' ? left_value / right_value
                                      : left_value % right_value;
      }
      type_checker_constant_from_int(out_value, quotient);
      if (shaped) {
        type_checker_constant_from_int(
            out_value, type_checker_wrap_integer(quotient, bits, is_signed));
      }
      return 1;
    }
    /* Bitwise and shift folding. These are how a byte constant is usually
     * written -- `1 << 7`, `0xF0 | 0x0F` -- so leaving them unfolded would
     * make the range check refuse constants that plainly fit. */
    if (strcmp(operator, "&") == 0 || strcmp(operator, "|") == 0 ||
        strcmp(operator, "^") == 0) {
      unsigned long long folded = operator[0] == '&' ? left_bits & right_bits
                                 : operator[0] == '|' ? left_bits | right_bits
                                                      : left_bits ^ right_bits;
      type_checker_constant_from_int(out_value, (long long)folded);
      if (shaped) {
        type_checker_constant_from_int(
            out_value, type_checker_wrap_integer((long long)folded, bits,
                                                 is_signed));
      }
      return 1;
    }
    if (strcmp(operator, "<<") == 0 || strcmp(operator, ">>") == 0) {
      int shift_left = operator[0] == '<';
      if (shaped) {
        /* The hardware masks the count to the operand's width, and the
         * language says so. `(uint8)1 << (uint8)9` is 2, not 0. */
        unsigned long long count =
            (unsigned long long)type_checker_wrap_integer(right_value, bits, 0)
            % (unsigned long long)bits;
        long long value =
            type_checker_wrap_integer(left_value, bits, is_signed);
        long long folded;
        if (shift_left) {
          folded = (long long)((unsigned long long)value << count);
        } else if (is_signed) {
          folded = value >> count;
        } else {
          folded = (long long)((unsigned long long)type_checker_wrap_integer(
                                   value, bits, 0) >>
                               count);
        }
        type_checker_constant_from_int(
            out_value, type_checker_wrap_integer(folded, bits, is_signed));
        return 1;
      }
      /* No shape to mask against: a count at or past the width has no value
       * to fold, so decline and let the caller treat this as non-constant. */
      if (right_value < 0 || right_value > (shift_left ? 62 : 63)) {
        return 0;
      }
      if (shift_left) {
        if (left_value < 0 || left_value > (LLONG_MAX >> right_value)) {
          return 0;
        }
        type_checker_constant_from_int(out_value, left_value << right_value);
        return 1;
      }
      type_checker_constant_from_int(out_value, left_value >> right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "==") == 0) {
      type_checker_constant_from_int(out_value, left_value == right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "!=") == 0) {
      type_checker_constant_from_int(out_value, left_value != right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "<") == 0) {
      type_checker_constant_from_int(out_value, left_value < right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "<=") == 0) {
      type_checker_constant_from_int(out_value, left_value <= right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, ">") == 0) {
      type_checker_constant_from_int(out_value, left_value > right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, ">=") == 0) {
      type_checker_constant_from_int(out_value, left_value >= right_value);
      return 1;
    }
    if (strcmp(binary_expr->operator, "&&") == 0) {
      type_checker_constant_from_int(out_value,
                                     (left_value != 0) && (right_value != 0));
      return 1;
    }
    if (strcmp(binary_expr->operator, "||") == 0) {
      type_checker_constant_from_int(out_value,
                                     (left_value != 0) || (right_value != 0));
      return 1;
    }
    return 0;
  }

  default:
    return 0;
  }
}

int type_checker_eval_integer_constant_with_checker(TypeChecker *checker,
                                                    ASTNode *expression,
                                                    long long *out_value) {
  TypeCheckerConstant value = {0};
  if (!out_value || !type_checker_eval_numeric_constant(checker, expression,
                                                         &value) ||
      value.is_float) {
    return 0;
  }
  *out_value = value.int_value;
  return 1;
}

int type_checker_eval_float_constant_with_checker(TypeChecker *checker,
                                                 ASTNode *expression,
                                                 double *out_value) {
  TypeCheckerConstant value = {0};
  if (!out_value || !type_checker_eval_numeric_constant(checker, expression,
                                                         &value)) {
    return 0;
  }
  *out_value = value.is_float ? value.float_value : (double)value.int_value;
  return 1;
}

int type_checker_eval_integer_constant(ASTNode *expression,
                                              long long *out_value) {
  return type_checker_eval_integer_constant_with_checker(NULL, expression,
                                                        out_value);
}

Type *type_checker_resolve_sizeof_argument(TypeChecker *checker,
                                                  CallExpression *call,
                                                  SourceLocation location) {
  if (!checker || !call) {
    return NULL;
  }

  if (call->argument_count != 1) {
    type_checker_set_error_at_location(
        checker, location, "sizeof expects exactly one type argument");
    return NULL;
  }

  ASTNode *arg = call->arguments ? call->arguments[0] : NULL;
  if (!arg || arg->type != AST_IDENTIFIER) {
    type_checker_set_error_at_location(
        checker, location, "sizeof expects a type name");
    return NULL;
  }

  Identifier *type_id = (Identifier *)arg->data;
  Type *type = type_id ? type_checker_get_type_by_name(checker, type_id->name)
                       : NULL;
  if (!type) {
    type_checker_set_error_at_location(
        checker, arg->location, "Unknown type '%s' in sizeof",
        type_id && type_id->name ? type_id->name : "<invalid>");
    return NULL;
  }

  if (type_contains_comptime_only(type)) {
    type_checker_reject_no_runtime_repr(checker, arg->location, type);
    return NULL;
  }

  return type;
}

/* Name the fields a type does have, so a misspelling reads as a list rather
 * than as "no such field". Truncates rather than growing without bound. */
static void append_field_names(const Type *owner, char *buffer, size_t size) {
  size_t used = 0;
  buffer[0] = '\0';
  for (size_t i = 0; i < owner->field_count; i++) {
    TypeField field;
    if (!type_field_at((Type *)owner, (uint32_t)i, &field) || !field.name) {
      continue;
    }
    size_t need = strlen(field.name) + (used ? 2 : 0);
    if (used + need + 4 >= size) {
      snprintf(buffer + used, size - used, "%s...", used ? ", " : "");
      return;
    }
    used += (size_t)snprintf(buffer + used, size - used, "%s%s",
                             used ? ", " : "", field.name);
  }
}

/* FNV-1a over a canonical rendering of a layout. Only declared facts feed it:
 * kind, size, alignment, and each field's name, offset, width and own layout.
 * No address, no pointer, no ordering that depends on how the compiler was
 * built, so the digest is the same for the same declaration on every host. */
static void layout_mix(uint64_t *hash, const void *bytes, size_t length) {
  const unsigned char *p = (const unsigned char *)bytes;
  for (size_t i = 0; i < length; i++) {
    *hash ^= (uint64_t)p[i];
    *hash *= 1099511628211ULL;
  }
}

static void layout_mix_u64(uint64_t *hash, uint64_t value) {
  unsigned char bytes[8];
  for (int i = 0; i < 8; i++) {
    bytes[i] = (unsigned char)((value >> (i * 8)) & 0xFF);
  }
  layout_mix(hash, bytes, sizeof(bytes));
}

static void layout_mix_str(uint64_t *hash, const char *s) {
  layout_mix(hash, s ? s : "", s ? strlen(s) : 0);
  layout_mix_u64(hash, 0x1F);
}

/* A pointer contributes that it is a pointer, never its pointee's layout: a
 * self-referential struct would not terminate, and what a pointer field costs
 * a layout is its width, which `size` already carries. */
static void layout_digest(const Type *type, uint64_t *hash, int depth) {
  if (!type || depth > 8) {
    layout_mix_u64(hash, 0xDEAD);
    return;
  }
  layout_mix_u64(hash, (uint64_t)type->kind);
  layout_mix_u64(hash, (uint64_t)type->size);
  layout_mix_u64(hash, (uint64_t)type->alignment);
  if (type->kind == TYPE_POINTER) {
    return;
  }
  size_t count = type_field_count(type);
  layout_mix_u64(hash, (uint64_t)count);
  for (size_t i = 0; i < count; i++) {
    TypeField field;
    if (!type_field_at((Type *)type, i, &field)) {
      layout_mix_u64(hash, 0xBAD);
      continue;
    }
    layout_mix_str(hash, field.name);
    layout_mix_u64(hash, (uint64_t)field.byte_offset);
    layout_mix_u64(hash, (uint64_t)field.bit_offset);
    layout_mix_u64(hash, (uint64_t)field.bit_width);
    layout_digest(field.type, hash, depth + 1);
  }
}

int type_checker_eval_layoutof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location,
                               long long *out_digest) {
  if (!checker || !call || !out_digest) {
    return 0;
  }
  if (call->argument_count != 1 || !call->arguments || !call->arguments[0]) {
    type_checker_set_error_at_location(
        checker, location,
        "layoutof expects exactly one type (for example layoutof(Player))");
    return 0;
  }

  ComptimeValue owner = comptime_none();
  if (!type_checker_eval_comptime(checker, call->arguments[0], &owner) ||
      owner.kind != COMPTIME_TYPE_REF) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "layoutof expects a compile-time type");
    return 0;
  }

  Type *type =
      type_checker_type_from_index(checker, owner.as.type_ref.type_index);
  if (!type) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "layoutof could not read that type from the type table");
    return 0;
  }

  uint64_t hash = 14695981039346656037ULL;
  layout_digest(type, &hash, 0);
  /* Clear the top bit so the digest is a positive int64 and compares the way
   * a programmer writes it, without a sign surprise at the boundary. */
  *out_digest = (long long)(hash & 0x7FFFFFFFFFFFFFFFULL);
  return 1;
}

long long type_checker_layout_digest(const Type *type) {
  uint64_t hash = 14695981039346656037ULL;
  if (!type) {
    return 0;
  }
  layout_digest(type, &hash, 0);
  return (long long)(hash & 0x7FFFFFFFFFFFFFFFULL);
}

int type_checker_eval_fieldof(TypeChecker *checker, CallExpression *call,
                              SourceLocation location,
                              ComptimeValue *out_value) {
  if (!checker || !call || !out_value) {
    return 0;
  }
  if (call->argument_count != 2 || !call->arguments || !call->arguments[0] ||
      !call->arguments[1]) {
    type_checker_set_error_at_location(
        checker, location,
        "fieldof expects a type and a field name (for example "
        "fieldof(Point, \"x\"))");
    return 0;
  }

  ComptimeValue owner_value = comptime_none();
  if (!type_checker_eval_comptime(checker, call->arguments[0], &owner_value) ||
      owner_value.kind != COMPTIME_TYPE_REF) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "fieldof expects a compile-time type as its first argument");
    return 0;
  }

  ComptimeValue name_value = comptime_none();
  if (!type_checker_eval_comptime(checker, call->arguments[1], &name_value) ||
      name_value.kind != COMPTIME_STRING || !name_value.as.string.value) {
    type_checker_set_error_at_location(
        checker, call->arguments[1]->location,
        "fieldof expects a compile-time string as its second argument (a "
        "string literal, or a `.name` query)");
    return 0;
  }

  Type *owner =
      type_checker_type_from_index(checker, owner_value.as.type_ref.type_index);
  if (!owner) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "fieldof could not read that type from the type table");
    return 0;
  }

  int field_index = type_get_field_index(owner, name_value.as.string.value);
  if (field_index < 0) {
    char names[256];
    append_field_names(owner, names, sizeof(names));
    if (names[0] != '\0') {
      type_checker_set_error_at_location(
          checker, call->arguments[1]->location,
          "'%s' has no field named '%s'; it has %s",
          owner->name ? owner->name : "<anonymous>",
          name_value.as.string.value, names);
    } else {
      type_checker_set_error_at_location(
          checker, call->arguments[1]->location, "'%s' has no fields",
          owner->name ? owner->name : "<anonymous>");
    }
    return 0;
  }

  *out_value = comptime_field_ref(owner_value.as.type_ref.type_index,
                                  (uint32_t)field_index);
  return 1;
}

int type_checker_eval_offsetof(TypeChecker *checker, CallExpression *call,
                               SourceLocation location, long long *out_offset) {
  if (!checker || !call || !out_offset) {
    return 0;
  }
  if (call->argument_count != 1 || !call->arguments || !call->arguments[0]) {
    type_checker_set_error_at_location(
        checker, location, "offsetof expects exactly one field argument");
    return 0;
  }

  ASTNode *arg = call->arguments[0];
  Type *arg_type = type_checker_infer_type(checker, arg);
  if (!arg_type) {
    return 0;
  }
  if (arg_type->kind != TYPE_FIELD) {
    type_checker_set_error_at_location(
        checker, arg->location,
        "offsetof expects a compile-time Field (for example Point.x)");
    return 0;
  }

  ComptimeValue value = comptime_none();
  if (!type_checker_eval_comptime(checker, arg, &value) ||
      value.kind != COMPTIME_FIELD_REF) {
    type_checker_set_error_at_location(
        checker, arg->location,
        "offsetof argument must be a compile-time Field value");
    return 0;
  }

  Type *owner =
      type_checker_type_from_index(checker, value.as.field_ref.type_index);
  TypeField field;
  if (!owner ||
      !type_field_at(owner, value.as.field_ref.field_index, &field)) {
    type_checker_set_error_at_location(
        checker, arg->location,
        "offsetof could not read that field from the type table");
    return 0;
  }
  if (field.byte_offset > (size_t)LLONG_MAX) {
    type_checker_set_error_at_location(checker, arg->location,
                                       "field offset does not fit in int64");
    return 0;
  }
  *out_offset = (long long)field.byte_offset;
  return 1;
}

Type *type_checker_resolve_typeof_argument(TypeChecker *checker,
                                           CallExpression *call,
                                           SourceLocation location) {
  if (!checker || !call) {
    return NULL;
  }

  if (call->argument_count != 1 || !call->arguments || !call->arguments[0]) {
    type_checker_set_error_at_location(
        checker, location, "typeof expects exactly one argument");
    return NULL;
  }

  ASTNode *arg = call->arguments[0];
  if (arg->type == AST_IDENTIFIER && arg->data) {
    Identifier *id = (Identifier *)arg->data;
    if (id && id->name) {
      Type *named = type_checker_get_type_by_name(checker, id->name);
      Symbol *symbol = type_checker_resolve_identifier(checker, id);
      if (named && (!symbol || symbol->kind == SYMBOL_STRUCT ||
                    symbol->kind == SYMBOL_ENUM)) {
        return named;
      }
    }
  }

  Type *inferred = type_checker_infer_type(checker, arg);
  if (!inferred) {
    return NULL;
  }
  return inferred;
}

static int eval_comptime_from_symbol(TypeChecker *checker, Symbol *symbol,
                                     ComptimeValue *out_value) {
  if (!symbol || !out_value) {
    return 0;
  }
  if (!comptime_is_none(symbol->comptime_value)) {
    *out_value = symbol->comptime_value;
    return 1;
  }
  if (symbol->kind == SYMBOL_STRUCT || symbol->kind == SYMBOL_ENUM) {
    if (!symbol->type) {
      return 0;
    }
    uint32_t index = type_checker_intern_type(checker, symbol->type);
    if (index == UINT32_MAX) {
      return 0;
    }
    *out_value = comptime_type_ref(index);
    return 1;
  }
  if (symbol->has_constant_value) {
    if (symbol->constant_is_float) {
      *out_value = comptime_float(symbol->constant_float_value);
    } else {
      *out_value = comptime_int(symbol->constant_integer_value);
    }
    return 1;
  }
  if (symbol->kind == SYMBOL_CONSTANT) {
    *out_value = comptime_int(symbol->data.constant.value);
    return 1;
  }
  return 0;
}

static int type_checker_eval_comptime_member(TypeChecker *checker,
                                             ASTNode *expression,
                                             ComptimeValue *out_value);

/* Compile-time text. A wire format has two ends, and the tag on the wire has
 * to be the same string in both; the only way to make that true by construction
 * is to build it once, while compiling, from the same table both ends read.
 *
 * The cost is on the same ledger as everything else the expansion spends: every
 * byte built is counted, and `--expansion-budget` bounds the total, so a
 * program cannot generate a megabyte of text without saying so. */
#define COMPTIME_TEXT_MAX 65536

static const char *comptime_text_of(TypeChecker *checker, ComptimeValue value) {
  char buffer[64];
  switch (value.kind) {
  case COMPTIME_STRING:
    return value.as.string.value;
  case COMPTIME_INT:
    snprintf(buffer, sizeof(buffer), "%lld", value.as.int_value);
    return string_intern(buffer);
  case COMPTIME_FLOAT:
    snprintf(buffer, sizeof(buffer), "%g", value.as.float_value);
    return string_intern(buffer);
  default:
    (void)checker;
    return NULL;
  }
}

static int comptime_join(TypeChecker *checker, const char *left,
                         const char *right, ComptimeValue *out_value) {
  size_t total;
  char *joined;
  if (!left || !right) {
    return 0;
  }
  total = strlen(left) + strlen(right);
  if (total >= COMPTIME_TEXT_MAX) {
    return 0;
  }
  joined = malloc(total + 1);
  if (!joined) {
    return 0;
  }
  memcpy(joined, left, strlen(left));
  memcpy(joined + strlen(left), right, strlen(right) + 1);
  *out_value = comptime_string(string_intern(joined));
  free(joined);
  checker->comptime_text_bytes += total;
  return out_value->as.string.value != NULL;
}

static int comptime_binary(TypeChecker *checker, ASTNode *expression,
                           ComptimeValue *out_value) {
  BinaryExpression *binary = (BinaryExpression *)expression->data;
  ComptimeValue left = comptime_none();
  ComptimeValue right = comptime_none();
  const char *op;
  if (!binary || !binary->operator|| !binary->left || !binary->right) {
    return 0;
  }
  op = binary->operator;
  if (!type_checker_eval_comptime(checker, binary->left, &left) ||
      !type_checker_eval_comptime(checker, binary->right, &right)) {
    return 0;
  }
  if (strcmp(op, "+") == 0 &&
      (left.kind == COMPTIME_STRING || right.kind == COMPTIME_STRING)) {
    return comptime_join(checker, comptime_text_of(checker, left),
                         comptime_text_of(checker, right), out_value);
  }
  if (left.kind == COMPTIME_INT && right.kind == COMPTIME_INT) {
    long long a = left.as.int_value;
    long long b = right.as.int_value;
    if (strcmp(op, "+") == 0) {
      *out_value = comptime_int(a + b);
    } else if (strcmp(op, "-") == 0) {
      *out_value = comptime_int(a - b);
    } else if (strcmp(op, "*") == 0) {
      *out_value = comptime_int(a * b);
    } else if (strcmp(op, "/") == 0 && b != 0) {
      *out_value = comptime_int(a / b);
    } else if (strcmp(op, "%") == 0 && b != 0) {
      *out_value = comptime_int(a % b);
    } else if (strcmp(op, "<") == 0) {
      *out_value = comptime_int(a < b);
    } else if (strcmp(op, "<=") == 0) {
      *out_value = comptime_int(a <= b);
    } else if (strcmp(op, ">") == 0) {
      *out_value = comptime_int(a > b);
    } else if (strcmp(op, ">=") == 0) {
      *out_value = comptime_int(a >= b);
    } else if (strcmp(op, "==") == 0) {
      *out_value = comptime_int(a == b);
    } else if (strcmp(op, "!=") == 0) {
      *out_value = comptime_int(a != b);
    } else {
      return 0;
    }
    return 1;
  }
  return 0;
}

int type_checker_eval_comptime(TypeChecker *checker, ASTNode *expression,
                               ComptimeValue *out_value) {
  if (!checker || !expression || !out_value) {
    return 0;
  }
  *out_value = comptime_none();

  switch (expression->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expression->data;
    if (!literal) {
      return 0;
    }
    if (literal->is_float) {
      *out_value = comptime_float(literal->float_value);
    } else {
      *out_value = comptime_int(literal->int_value);
    }
    return 1;
  }

  case AST_STRING_LITERAL: {
    StringLiteral *literal = (StringLiteral *)expression->data;
    if (!literal || !literal->value) {
      return 0;
    }
    *out_value = comptime_string(string_intern(literal->value));
    return out_value->as.string.value != NULL;
  }

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    if (!identifier || !identifier->name) {
      return 0;
    }
    Symbol *symbol = type_checker_resolve_identifier(checker, identifier);
    if (eval_comptime_from_symbol(checker, symbol, out_value)) {
      return 1;
    }
    Type *named = type_checker_get_type_by_name(checker, identifier->name);
    if (named) {
      uint32_t index = type_checker_intern_type(checker, named);
      if (index == UINT32_MAX) {
        return 0;
      }
      *out_value = comptime_type_ref(index);
      return 1;
    }
    return 0;
  }

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (!call || !call->function_name) {
      return 0;
    }
    if (strcmp(call->function_name, "offsetof") == 0) {
      long long offset = 0;
      if (!type_checker_eval_offsetof(checker, call, expression->location,
                                      &offset)) {
        return 0;
      }
      *out_value = comptime_int(offset);
      return 1;
    }
    if (strcmp(call->function_name, "typeof") == 0) {
      Type *referred = type_checker_resolve_typeof_argument(
          checker, call, expression->location);
      if (!referred) {
        return 0;
      }
      uint32_t index = type_checker_intern_type(checker, referred);
      if (index == UINT32_MAX) {
        return 0;
      }
      *out_value = comptime_type_ref(index);
      return 1;
    }
    if (strcmp(call->function_name, "sizeof") == 0) {
      Type *sized = type_checker_resolve_sizeof_argument(
          checker, call, expression->location);
      if (!sized) {
        return 0;
      }
      *out_value = comptime_int((long long)sized->size);
      return 1;
    }
    if (strcmp(call->function_name, "fieldof") == 0) {
      return type_checker_eval_fieldof(checker, call, expression->location,
                                       out_value);
    }
    if (strcmp(call->function_name, "layoutof") == 0) {
      long long digest = 0;
      if (!type_checker_eval_layoutof(checker, call, expression->location,
                                      &digest)) {
        return 0;
      }
      *out_value = comptime_int(digest);
      return 1;
    }
    if (strcmp(call->function_name, "textof") == 0 &&
        call->argument_count == 1) {
      ComptimeValue inner = comptime_none();
      const char *text;
      if (!type_checker_eval_comptime(checker, call->arguments[0], &inner)) {
        return 0;
      }
      text = comptime_text_of(checker, inner);
      if (!text) {
        return 0;
      }
      *out_value = comptime_string(string_intern(text));
      checker->comptime_text_bytes += strlen(text);
      return out_value->as.string.value != NULL;
    }
    return 0;
  }

  case AST_BINARY_EXPRESSION:
    return comptime_binary(checker, expression, out_value);

  case AST_MEMBER_ACCESS:
    return type_checker_eval_comptime_member(checker, expression, out_value);

  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index_expr =
        (ArrayIndexExpression *)expression->data;
    if (!index_expr || !index_expr->array || !index_expr->index) {
      return 0;
    }
    ComptimeValue sequence = comptime_none();
    ComptimeValue subscript = comptime_none();
    if (!type_checker_eval_comptime(checker, index_expr->array, &sequence) ||
        sequence.kind != COMPTIME_SEQUENCE ||
        !type_checker_eval_comptime(checker, index_expr->index, &subscript) ||
        subscript.kind != COMPTIME_INT) {
      return 0;
    }
    return type_checker_eval_sequence_index(sequence, subscript.as.int_value,
                                            out_value);
  }

  default:
    return 0;
  }
}

static int type_checker_eval_comptime_member(TypeChecker *checker,
                                             ASTNode *expression,
                                             ComptimeValue *out_value) {
  MemberAccess *member = (MemberAccess *)expression->data;
  if (!member || !member->object || !member->member) {
    return 0;
  }
  ComptimeValue owner = comptime_none();
  if (!type_checker_eval_comptime(checker, member->object, &owner)) {
    return 0;
  }
  if (owner.kind == COMPTIME_FIELD_REF) {
    return type_checker_eval_field_member(checker, owner, member->member,
                                          out_value);
  }
  if (owner.kind == COMPTIME_SEQUENCE) {
    return type_checker_eval_sequence_member(checker, owner, member->member,
                                             out_value);
  }
  if (owner.kind == COMPTIME_ROW) {
    return type_checker_eval_row_member(checker, owner, member->member,
                                        out_value);
  }
  if (owner.kind != COMPTIME_TYPE_REF) {
    return 0;
  }
  Type *referred =
      type_checker_type_from_index(checker, owner.as.type_ref.type_index);
  if (!referred) {
    return 0;
  }
  /* `Color.Red` on a plain enum reads the member off the type table rather
   * than the variant's bare global, so a compiler-registered enum that
   * deliberately declares no bare globals still folds. */
  if (referred->kind == TYPE_ENUM) {
    for (size_t i = 0; i < referred->enum_member_count; i++) {
      if (referred->enum_member_names[i] &&
          strcmp(referred->enum_member_names[i], member->member) == 0) {
        *out_value = comptime_int(referred->enum_member_values[i]);
        return 1;
      }
    }
    /* Not a variant, so fall through: an enum type answers the same shape
     * queries every other type does (`typeof(Color).kind`). */
  }
  /* A struct field named the same as a query wins: the program's own
   * declaration is never shadowed by the reflection surface. */
  int field_index = type_get_field_index(referred, member->member);
  if (field_index >= 0) {
    *out_value = comptime_field_ref(owner.as.type_ref.type_index,
                                    (uint32_t)field_index);
    return 1;
  }
  return type_checker_eval_type_member(checker, owner, member->member,
                                       out_value);
}

int type_checker_validate_static_assert(TypeChecker *checker,
                                               CallExpression *call,
                                               SourceLocation location) {
  if (!checker || !call) {
    return 0;
  }

  if (call->argument_count != 1) {
    type_checker_set_error_at_location(
        checker, location, "static_assert expects exactly one condition");
    return 0;
  }

  long long value = 0;
  if (!type_checker_eval_integer_constant_with_checker(
          checker, call->arguments[0], &value)) {
    /* Folding failed, which says the condition is not constant but not why.
     * Type checking the condition first surfaces the real reason -- an unknown
     * query, a sequence index out of range -- and only when that comes back
     * clean is "not a constant" actually the whole story. */
    if (call->arguments[0] &&
        !type_checker_infer_type(checker, call->arguments[0])) {
      return 0;
    }
    type_checker_set_error_at_location(
        checker, call->arguments[0] ? call->arguments[0]->location : location,
        "static_assert condition must be a compile-time integer expression");
    return 0;
  }

  if (value == 0) {
    type_checker_set_error_at_location(checker, location,
                                       "static_assert failed");
    return 0;
  }

  return 1;
}

void type_checker_buffer_extent_clear(TypeChecker *checker) {
  if (!checker) {
    return;
  }

  TrackedBufferExtent *node = checker->tracked_buffer_extents;
  while (node) {
    TrackedBufferExtent *next = node->next;
    free(node->name);
    free(node);
    node = next;
  }
  checker->tracked_buffer_extents = NULL;
}

void type_checker_buffer_extent_exit_scope(TypeChecker *checker,
                                                  int scope_depth) {
  if (!checker) {
    return;
  }

  TrackedBufferExtent **node_ptr = &checker->tracked_buffer_extents;
  while (*node_ptr) {
    TrackedBufferExtent *node = *node_ptr;
    if (node->scope_depth == scope_depth) {
      *node_ptr = node->next;
      free(node->name);
      free(node);
      continue;
    }
    node_ptr = &node->next;
  }
}

TrackedBufferExtent *
type_checker_buffer_extent_find(TypeChecker *checker, const char *name) {
  if (!checker || !name) {
    return NULL;
  }

  TrackedBufferExtent *node = checker->tracked_buffer_extents;
  while (node) {
    if (node->name && strcmp(node->name, name) == 0) {
      return node;
    }
    node = node->next;
  }
  return NULL;
}

int type_checker_buffer_extent_declare(TypeChecker *checker,
                                              const char *name,
                                              long long byte_count,
                                              long long known_alignment) {
  if (!checker || !name) {
    return 0;
  }

  TrackedBufferExtent *node = malloc(sizeof(TrackedBufferExtent));
  if (!node) {
    return 0;
  }

  node->name = strdup(name);
  if (!node->name) {
    free(node);
    return 0;
  }

  node->byte_count = byte_count;
  node->known_alignment = known_alignment;
  node->scope_depth = checker->tracked_scope_depth;
  node->next = checker->tracked_buffer_extents;
  checker->tracked_buffer_extents = node;
  return 1;
}

int type_checker_buffer_extent_set(TypeChecker *checker, const char *name,
                                          long long byte_count,
                                          long long known_alignment) {
  if (!checker || !name) {
    return 0;
  }

  TrackedBufferExtent *node = type_checker_buffer_extent_find(checker, name);
  if (!node) {
    return type_checker_buffer_extent_declare(checker, name, byte_count,
                                              known_alignment);
  }

  node->byte_count = byte_count;
  node->known_alignment = known_alignment;
  return 1;
}

long long type_checker_default_heap_alignment(void) {
  // Current backend target is 64-bit; model malloc/calloc as at least 8-byte
  // aligned so we can reason about common scalar casts.
  return 8;
}

long long
type_checker_extract_allocation_call_alignment(CallExpression *call) {
  if (!call || !call->function_name) {
    return -1;
  }
  if (strcmp(call->function_name, "malloc") == 0 ||
      strcmp(call->function_name, "calloc") == 0) {
    return type_checker_default_heap_alignment();
  }
  return -1;
}

long long type_checker_known_alignment_after_offset(long long base_align,
                                                           long long offset) {
  if (base_align <= 0) {
    return -1;
  }
  if (offset == 0) {
    return base_align;
  }
  if (offset == LLONG_MIN) {
    return 1;
  }

  long long magnitude = offset < 0 ? -offset : offset;
  long long result = base_align;
  while (result > 1 && (magnitude % result) != 0) {
    result /= 2;
  }
  return result > 0 ? result : 1;
}

const char *type_checker_extract_identifier_name(ASTNode *expression) {
  if (!expression) {
    return NULL;
  }

  if (expression->type == AST_CAST_EXPRESSION) {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr) {
      return NULL;
    }
    return type_checker_extract_identifier_name(cast_expr->operand);
  }

  if (expression->type != AST_IDENTIFIER) {
    return NULL;
  }

  Identifier *id = (Identifier *)expression->data;
  if (!id || !id->name) {
    return NULL;
  }
  return id->name;
}

long long
type_checker_extract_allocation_call_extent(CallExpression *call) {
  if (!call || !call->function_name) {
    return -1;
  }

  if (strcmp(call->function_name, "malloc") == 0) {
    if (call->argument_count != 1) {
      return -1;
    }
    long long size = 0;
    if (!type_checker_eval_integer_constant(call->arguments[0], &size) ||
        size < 0) {
      return -1;
    }
    return size;
  }

  if (strcmp(call->function_name, "calloc") == 0) {
    if (call->argument_count != 2) {
      return -1;
    }
    long long count = 0;
    long long size = 0;
    if (!type_checker_eval_integer_constant(call->arguments[0], &count) ||
        !type_checker_eval_integer_constant(call->arguments[1], &size) ||
        count < 0 || size < 0) {
      return -1;
    }
    if (count > 0 && size > (LLONG_MAX / count)) {
      return -1;
    }
    return count * size;
  }

  return -1;
}

long long type_checker_extract_known_buffer_extent(TypeChecker *checker,
                                                          ASTNode *expression) {
  if (!expression) {
    return -1;
  }

  if (expression->type == AST_CAST_EXPRESSION) {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr) {
      return -1;
    }
    return type_checker_extract_known_buffer_extent(checker, cast_expr->operand);
  }

  if (expression->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)expression->data;
    return type_checker_extract_allocation_call_extent(call);
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right) {
      return -1;
    }
    if (strcmp(binary_expr->operator, "+") == 0) {
      long long offset = 0;
      long long base_extent = -1;
      if (type_checker_eval_integer_constant(binary_expr->right, &offset)) {
        base_extent =
            type_checker_extract_known_buffer_extent(checker, binary_expr->left);
      } else if (type_checker_eval_integer_constant(binary_expr->left, &offset)) {
        base_extent =
            type_checker_extract_known_buffer_extent(checker, binary_expr->right);
      } else {
        return -1;
      }
      if (base_extent < 0 || offset < 0) {
        return -1;
      }
      if (offset >= base_extent) {
        return 0;
      }
      return base_extent - offset;
    }
  }

  const char *name = type_checker_extract_identifier_name(expression);
  if (!name) {
    return -1;
  }

  TrackedBufferExtent *node = type_checker_buffer_extent_find(checker, name);
  if (!node) {
    return -1;
  }

  return node->byte_count;
}

long long
type_checker_extract_known_pointer_alignment(TypeChecker *checker,
                                             ASTNode *expression) {
  if (!expression) {
    return -1;
  }

  if (expression->type == AST_CAST_EXPRESSION) {
    CastExpression *cast_expr = (CastExpression *)expression->data;
    if (!cast_expr) {
      return -1;
    }
    return type_checker_extract_known_pointer_alignment(checker,
                                                        cast_expr->operand);
  }

  if (expression->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)expression->data;
    return type_checker_extract_allocation_call_alignment(call);
  }

  if (expression->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *binary_expr = (BinaryExpression *)expression->data;
    if (!binary_expr || !binary_expr->operator || !binary_expr->left ||
        !binary_expr->right) {
      return -1;
    }

    if (strcmp(binary_expr->operator, "+") == 0 ||
        strcmp(binary_expr->operator, "-") == 0) {
      long long offset = 0;
      long long base_align = -1;

      if (type_checker_eval_integer_constant(binary_expr->right, &offset)) {
        base_align = type_checker_extract_known_pointer_alignment(
            checker, binary_expr->left);
      } else if (strcmp(binary_expr->operator, "+") == 0 &&
                 type_checker_eval_integer_constant(binary_expr->left,
                                                   &offset)) {
        base_align = type_checker_extract_known_pointer_alignment(
            checker, binary_expr->right);
      } else {
        return -1;
      }

      if (base_align <= 0) {
        return -1;
      }
      return type_checker_known_alignment_after_offset(base_align, offset);
    }
  }

  const char *name = type_checker_extract_identifier_name(expression);
  if (!name) {
    return -1;
  }

  TrackedBufferExtent *node = type_checker_buffer_extent_find(checker, name);
  if (!node) {
    return -1;
  }

  return node->known_alignment;
}

void type_checker_warn_potential_misaligned_cast(TypeChecker *checker,
                                                        ASTNode *expression,
                                                        CastExpression *cast_expr,
                                                        Type *target_type) {
  if (!checker || !checker->error_reporter || !expression || !cast_expr ||
      !target_type || target_type->kind != TYPE_POINTER ||
      !target_type->base_type) {
    return;
  }

  size_t required_alignment = target_type->base_type->alignment;
  if (required_alignment <= 1) {
    return;
  }

  long long known_alignment =
      type_checker_extract_known_pointer_alignment(checker, cast_expr->operand);
  if (known_alignment <= 0) {
    return;
  }

  if (known_alignment < (long long)required_alignment) {
    char message[512];
    snprintf(
        message, sizeof(message),
        "Cast to %s may violate required %zu-byte alignment (known alignment %lld)",
        target_type->name ? target_type->name : "pointer", required_alignment,
        known_alignment);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               expression->location, message);
  }
}

/* `(T*)((int64)p)`, where p is already a pointer. The integer in the middle
 * carries nothing the pointer did not: it is the same address, spelled through
 * a type that says less. What it costs is real -- the borrow checker and the
 * translation validator both have to give up on a value whose provenance was
 * laundered through an integer, and every such cast is a hole in what they can
 * prove about the program around it.
 *
 * Reported, and then seen through: lowering keeps the pointer, so the analyses
 * are not blinded while the source is cleaned up. Going the other way, an
 * integer that really is an address (a handle from the operating system, a
 * device pointer) still casts to a pointer, and a pointer still casts to an
 * integer to be printed or hashed. Only the round trip is noise. */
static ASTNode *type_checker_pointer_laundered_through_integer(
    TypeChecker *checker, ASTNode *operand) {
  CastExpression *inner = NULL;
  Type *inner_target = NULL;
  Type *inner_source = NULL;

  if (!operand || operand->type != AST_CAST_EXPRESSION || !operand->data) {
    return NULL;
  }
  inner = (CastExpression *)operand->data;
  if (!inner->type_name || !inner->operand) {
    return NULL;
  }
  inner_target = type_checker_get_type_by_name(checker, inner->type_name);
  if (!inner_target || !type_checker_is_integer_type(inner_target)) {
    return NULL;
  }
  inner_source = inner->operand->resolved_type;
  if (!inner_source) {
    inner_source = type_checker_infer_type(checker, inner->operand);
  }
  if (!inner_source || (inner_source->kind != TYPE_POINTER &&
                        inner_source->kind != TYPE_FUNCTION_POINTER)) {
    return NULL;
  }
  return inner->operand;
}

void type_checker_warn_pointer_integer_round_trip(TypeChecker *checker,
                                                  ASTNode *expression,
                                                  CastExpression *cast_expr,
                                                  Type *target_type) {
  ASTNode *pointer = NULL;
  char message[512];
  char help[512];

  if (!checker || !checker->error_reporter || !expression || !cast_expr ||
      !target_type || (target_type->kind != TYPE_POINTER &&
                       target_type->kind != TYPE_FUNCTION_POINTER)) {
    return;
  }
  pointer = type_checker_pointer_laundered_through_integer(checker,
                                                           cast_expr->operand);
  if (!pointer) {
    return;
  }

  snprintf(message, sizeof(message),
           "This pointer is cast to an integer and straight back to a pointer");
  snprintf(help, sizeof(help),
           "write the pointer cast on its own: (%s)<pointer>. The integer in "
           "between is the same address with its provenance dropped, which the "
           "borrow checker and --verify cannot follow through",
           target_type->name ? target_type->name : "T*");
  error_reporter_add_warning_with_suggestion(
      checker->error_reporter, ERROR_SEMANTIC, expression->location, message,
      help);
  error_reporter_set_last_code(checker->error_reporter, "M0120");
}

void type_checker_warn_recv_buffer_bounds(TypeChecker *checker,
                                                 CallExpression *call) {
  if (!checker || !checker->error_reporter || !call || !call->function_name) {
    return;
  }
  if (strcmp(call->function_name, "recv") != 0 || call->argument_count < 3) {
    return;
  }

  const char *buffer_name = type_checker_extract_identifier_name(call->arguments[1]);
  if (!buffer_name) {
    return;
  }

  TrackedBufferExtent *fact =
      type_checker_buffer_extent_find(checker, buffer_name);
  if (!fact || fact->byte_count < 0) {
    return;
  }

  long long recv_len = 0;
  if (!type_checker_eval_integer_constant(call->arguments[2], &recv_len)) {
    return;
  }

  char message[512];
  if (recv_len > fact->byte_count) {
    snprintf(message, sizeof(message),
             "recv length %lld exceeds tracked allocation %lld bytes for '%s'",
             recv_len, fact->byte_count, buffer_name);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               call->arguments[2]->location, message);
  }
}

void type_checker_warn_memcpy_buffer_bounds(TypeChecker *checker,
                                                   CallExpression *call) {
  if (!checker || !checker->error_reporter || !call || !call->function_name) {
    return;
  }
  int is_memcpy = strcmp(call->function_name, "memcpy") == 0;
  int is_memmove = strcmp(call->function_name, "memmove") == 0;
  if ((!is_memcpy && !is_memmove) || call->argument_count < 3) {
    return;
  }

  long long copy_len = 0;
  if (!type_checker_eval_integer_constant(call->arguments[2], &copy_len) ||
      copy_len < 0) {
    return;
  }

  long long dst_extent =
      type_checker_extract_known_buffer_extent(checker, call->arguments[0]);
  long long src_extent =
      type_checker_extract_known_buffer_extent(checker, call->arguments[1]);
  const char *fn_name = call->function_name;

  char message[512];
  if (dst_extent >= 0 && copy_len > dst_extent) {
    snprintf(message, sizeof(message),
             "%s length %lld exceeds known destination extent %lld bytes",
             fn_name, copy_len, dst_extent);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               call->arguments[2]->location, message);
  }

  if (src_extent >= 0 && copy_len > src_extent) {
    snprintf(message, sizeof(message),
             "%s length %lld exceeds known source extent %lld bytes", fn_name,
             copy_len, src_extent);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               call->arguments[2]->location, message);
  }
}

int type_checker_ast_contains_node_type(ASTNode *node,
                                               ASTNodeType target_type) {
  if (!node) {
    return 0;
  }
  if (node->type == target_type) {
    return 1;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (type_checker_ast_contains_node_type(node->children[i], target_type)) {
      return 1;
    }
  }
  return 0;
}

int type_checker_is_null_pointer_constant(ASTNode *expression) {
  long long value = 0;
  return type_checker_eval_integer_constant(expression, &value) && value == 0;
}

int type_checker_type_accepts_null_pointer(const Type *type) {
  if (!type) {
    return 0;
  }
  return type->kind == TYPE_POINTER || type->kind == TYPE_FUNCTION_POINTER;
}

int type_checker_statement_guarantees_termination(ASTNode *statement) {
  if (!statement) {
    return 0;
  }

  switch (statement->type) {
  case AST_RETURN_STATEMENT:
  case AST_BREAK_STATEMENT:
  case AST_CONTINUE_STATEMENT:
    return 1;
  /* `quiesce;` transfers control nowhere: it applies staged swaps and falls
   * through to the next statement, so everything after it is reachable. */
  case AST_QUIESCE_STATEMENT:
    return 0;
  /* `fallthrough;` leaves this case for the next one, so what follows it in
   * the same case is unreachable, exactly as after a `break`. */
  case AST_FALLTHROUGH_STATEMENT:
    return 1;
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)statement->data;
    if (!if_stmt || !if_stmt->then_branch || !if_stmt->else_branch) {
      return 0;
    }
    if (!type_checker_statement_guarantees_termination(if_stmt->then_branch)) {
      return 0;
    }
    for (size_t i = 0; i < if_stmt->else_if_count; i++) {
      if (!if_stmt->else_ifs[i].body ||
          !type_checker_statement_guarantees_termination(
              if_stmt->else_ifs[i].body)) {
        return 0;
      }
    }
    return type_checker_statement_guarantees_termination(if_stmt->else_branch);
  }
  case AST_PROGRAM: {
    for (size_t i = 0; i < statement->child_count; i++) {
      if (type_checker_statement_guarantees_termination(
              statement->children[i])) {
        return 1;
      }
    }
    return 0;
  }
  default:
    return 0;
  }
}
