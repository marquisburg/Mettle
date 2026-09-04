#include "type_checker_internal.h"
#include "../ir/ir_explain_ledger.h"

struct TypeCheckerGuard {
  ASTNode *condition;
  int negated;
  const char *name;
  int has_min;
  int has_max;
  long long min;
  long long max;
};

typedef struct {
  int has_min;
  int has_max;
  long long min;
  long long max;
} Range;

static Range range_unknown(void) {
  Range r;
  r.has_min = 0;
  r.has_max = 0;
  r.min = 0;
  r.max = 0;
  return r;
}

static Range range_exact(long long v) {
  Range r;
  r.has_min = 1;
  r.has_max = 1;
  r.min = v;
  r.max = v;
  return r;
}

static Range range_of_type(const Type *type) {
  Range r = range_unknown();
  long long min = 0;
  unsigned long long max = 0;
  if (!type) {
    return r;
  }
  if (type->refined_base && type->refine_has_range) {
    r.has_min = 1;
    r.has_max = 1;
    r.min = type->refine_min;
    r.max = type->refine_max;
    return r;
  }
  if (type->refined_base) {
    return range_of_type(type->refined_base);
  }
  if (type->kind == TYPE_ENUM ||
      !type_checker_integer_bounds(type, &min, &max)) {
    return r;
  }
  r.has_min = 1;
  r.min = min;
  if (max <= (unsigned long long)LLONG_MAX) {
    r.has_max = 1;
    r.max = (long long)max;
  }
  return r;
}

static void range_meet(Range *r, const Range *other) {
  if (other->has_min && (!r->has_min || other->min > r->min)) {
    r->has_min = 1;
    r->min = other->min;
  }
  if (other->has_max && (!r->has_max || other->max < r->max)) {
    r->has_max = 1;
    r->max = other->max;
  }
}

static int is_comparison(const char *op) {
  return op && (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
}

static const char *flip_operator(const char *op) {
  if (strcmp(op, "<") == 0) {
    return ">";
  }
  if (strcmp(op, "<=") == 0) {
    return ">=";
  }
  if (strcmp(op, ">") == 0) {
    return "<";
  }
  if (strcmp(op, ">=") == 0) {
    return "<=";
  }
  return op;
}

static const char *negate_operator(const char *op) {
  if (strcmp(op, "<") == 0) {
    return ">=";
  }
  if (strcmp(op, "<=") == 0) {
    return ">";
  }
  if (strcmp(op, ">") == 0) {
    return "<=";
  }
  if (strcmp(op, ">=") == 0) {
    return "<";
  }
  if (strcmp(op, "==") == 0) {
    return "!=";
  }
  if (strcmp(op, "!=") == 0) {
    return "==";
  }
  return NULL;
}

static const char *identifier_name(const ASTNode *node) {
  if (!node || node->type != AST_IDENTIFIER || !node->data) {
    return NULL;
  }
  return ((const Identifier *)node->data)->name;
}

static int binary_parts(const ASTNode *node, const char **op, ASTNode **left,
                        ASTNode **right) {
  const BinaryExpression *binary;
  if (!node || node->type != AST_BINARY_EXPRESSION || !node->data) {
    return 0;
  }
  binary = (const BinaryExpression *)node->data;
  *op = binary->operator;
  *left = binary->left;
  *right = binary->right;
  return *op != NULL;
}

static int unary_parts(const ASTNode *node, const char **op,
                       ASTNode **operand) {
  const UnaryExpression *unary;
  if (!node || node->type != AST_UNARY_EXPRESSION || !node->data) {
    return 0;
  }
  unary = (const UnaryExpression *)node->data;
  *op = unary->operator;
  *operand = unary->operand;
  return *op != NULL && *operand != NULL;
}

typedef struct {
  ASTNode *node;
  int negated;
} Atom;

typedef void (*AtomVisitor)(void *ctx, ASTNode *node, int negated);

static void visit_atoms(ASTNode *node, int negated, AtomVisitor visit,
                        void *ctx, int depth) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  ASTNode *operand = NULL;
  if (!node || depth > 16) {
    return;
  }
  if (unary_parts(node, &op, &operand) && strcmp(op, "!") == 0) {
    visit_atoms(operand, !negated, visit, ctx, depth + 1);
    return;
  }
  if (binary_parts(node, &op, &left, &right)) {
    if (!negated && strcmp(op, "&&") == 0) {
      visit_atoms(left, 0, visit, ctx, depth + 1);
      visit_atoms(right, 0, visit, ctx, depth + 1);
      return;
    }
    if (negated && strcmp(op, "||") == 0) {
      visit_atoms(left, 1, visit, ctx, depth + 1);
      visit_atoms(right, 1, visit, ctx, depth + 1);
      return;
    }
  }
  visit(ctx, node, negated);
}

static int range_of(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth);

typedef struct {
  TypeChecker *checker;
  const char *name;
  Range *range;
  int depth;
} NarrowContext;

static void narrow_by_comparison(NarrowContext *ctx, const char *op,
                                 ASTNode *other) {
  Range bound;
  if (!range_of(ctx->checker, other, &bound, ctx->depth + 1)) {
    return;
  }
  if (strcmp(op, "<") == 0 && bound.has_max && bound.max > LLONG_MIN) {
    Range r = range_unknown();
    r.has_max = 1;
    r.max = bound.max - 1;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, "<=") == 0 && bound.has_max) {
    Range r = range_unknown();
    r.has_max = 1;
    r.max = bound.max;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, ">") == 0 && bound.has_min && bound.min < LLONG_MAX) {
    Range r = range_unknown();
    r.has_min = 1;
    r.min = bound.min + 1;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, ">=") == 0 && bound.has_min) {
    Range r = range_unknown();
    r.has_min = 1;
    r.min = bound.min;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, "==") == 0) {
    range_meet(ctx->range, &bound);
  } else if (strcmp(op, "!=") == 0 && bound.has_min && bound.has_max &&
             bound.min == bound.max) {
    if (ctx->range->has_min && ctx->range->min == bound.min &&
        bound.min < LLONG_MAX) {
      ctx->range->min = bound.min + 1;
    }
    if (ctx->range->has_max && ctx->range->max == bound.max &&
        bound.max > LLONG_MIN) {
      ctx->range->max = bound.max - 1;
    }
  }
}

static void narrow_visit(void *raw, ASTNode *node, int negated) {
  NarrowContext *ctx = (NarrowContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  const char *left_name;
  const char *right_name;
  if (!binary_parts(node, &op, &left, &right) || !is_comparison(op)) {
    return;
  }
  if (negated) {
    op = negate_operator(op);
    if (!op) {
      return;
    }
  }
  left_name = identifier_name(left);
  right_name = identifier_name(right);
  if (left_name && strcmp(left_name, ctx->name) == 0) {
    narrow_by_comparison(ctx, op, right);
  } else if (right_name && strcmp(right_name, ctx->name) == 0) {
    narrow_by_comparison(ctx, flip_operator(op), left);
  }
}

static void narrow_by_guards(TypeChecker *checker, const char *name,
                             Range *range, int depth) {
  NarrowContext ctx;
  ctx.checker = checker;
  ctx.name = name;
  ctx.range = range;
  ctx.depth = depth;
  for (size_t i = 0; i < checker->guard_count; i++) {
    struct TypeCheckerGuard *guard = &checker->guards[i];
    if (guard->condition) {
      visit_atoms(guard->condition, guard->negated, narrow_visit, &ctx, 0);
    } else if (guard->name && strcmp(guard->name, name) == 0) {
      Range r = range_unknown();
      r.has_min = guard->has_min;
      r.min = guard->min;
      r.has_max = guard->has_max;
      r.max = guard->max;
      range_meet(range, &r);
    }
  }
}

static int add_checked(long long a, long long b, long long *out) {
  return !__builtin_add_overflow(a, b, out);
}

static int sub_checked(long long a, long long b, long long *out) {
  return !__builtin_sub_overflow(a, b, out);
}

static int mul_checked(long long a, long long b, long long *out) {
  return !__builtin_mul_overflow(a, b, out);
}

static int range_of(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {
  if (checker) {
    checker->proof_steps++;
  }
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  ASTNode *operand = NULL;
  *out = range_unknown();
  if (!expr || depth > 32) {
    return 0;
  }
  switch (expr->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expr->data;
    if (!literal || literal->is_float) {
      return 0;
    }
    *out = range_exact(literal->int_value);
    return 1;
  }
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expr->data;
    Symbol *symbol = identifier
                         ? type_checker_resolve_identifier(checker, identifier)
                         : NULL;
    if (symbol && symbol->kind == SYMBOL_CONSTANT) {
      *out = range_exact(symbol->data.constant.value);
      return 1;
    }
    if (symbol && symbol->has_constant_value && !symbol->constant_is_float) {
      *out = range_exact(symbol->constant_integer_value);
      return 1;
    }
    {
      Type *type = expr->resolved_type ? expr->resolved_type
                                       : (symbol ? symbol->type : NULL);
      if (!type || type->kind == TYPE_ENUM ||
          !type_checker_is_integer_type(type)) {
        return 0;
      }
      *out = range_of_type(type);
    }
    if (identifier && identifier->name) {
      narrow_by_guards(checker, identifier->name, out, depth);
    }
    return out->has_min || out->has_max;
  }
  case AST_BINARY_EXPRESSION: {
    Range l;
    Range r;
    if (!binary_parts(expr, &op, &left, &right)) {
      return 0;
    }
    if (is_comparison(op) || strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
      out->has_min = 1;
      out->has_max = 1;
      out->min = 0;
      out->max = 1;
      return 1;
    }
    {
      int have_l = range_of(checker, left, &l, depth + 1);
      int have_r = range_of(checker, right, &r, depth + 1);
      if (strcmp(op, "+") == 0 && have_l && have_r) {
        if (l.has_min && r.has_min && add_checked(l.min, r.min, &out->min)) {
          out->has_min = 1;
        }
        if (l.has_max && r.has_max && add_checked(l.max, r.max, &out->max)) {
          out->has_max = 1;
        }
      } else if (strcmp(op, "-") == 0 && have_l && have_r) {
        if (l.has_min && r.has_max && sub_checked(l.min, r.max, &out->min)) {
          out->has_min = 1;
        }
        if (l.has_max && r.has_min && sub_checked(l.max, r.min, &out->max)) {
          out->has_max = 1;
        }
      } else if (strcmp(op, "*") == 0 && have_l && have_r && l.has_min &&
                 l.has_max && r.has_min && r.has_max) {
        long long products[4];
        int ok = mul_checked(l.min, r.min, &products[0]) &&
                 mul_checked(l.min, r.max, &products[1]) &&
                 mul_checked(l.max, r.min, &products[2]) &&
                 mul_checked(l.max, r.max, &products[3]);
        if (ok) {
          out->min = products[0];
          out->max = products[0];
          for (int i = 1; i < 4; i++) {
            if (products[i] < out->min) {
              out->min = products[i];
            }
            if (products[i] > out->max) {
              out->max = products[i];
            }
          }
          out->has_min = 1;
          out->has_max = 1;
        }
      } else if (strcmp(op, "%") == 0 && have_r && r.has_min && r.has_max &&
                 r.min > 0) {
        out->has_min = 1;
        out->has_max = 1;
        out->max = r.max - 1;
        out->min = have_l && l.has_min && l.min >= 0 ? 0 : -(r.max - 1);
      } else if (strcmp(op, "/") == 0 && have_l && have_r && r.has_min &&
                 r.has_max && r.min == r.max && r.min > 0) {
        if (l.has_min) {
          out->has_min = 1;
          out->min = l.min / r.min;
        }
        if (l.has_max) {
          out->has_max = 1;
          out->max = l.max / r.min;
        }
      } else if (strcmp(op, "&") == 0) {
        long long mask = 0;
        int have_mask = 0;
        if (have_r && r.has_min && r.has_max && r.min == r.max &&
            r.min >= 0) {
          mask = r.min;
          have_mask = 1;
        } else if (have_l && l.has_min && l.has_max && l.min == l.max &&
                   l.min >= 0) {
          mask = l.min;
          have_mask = 1;
        }
        if (have_mask) {
          out->has_min = 1;
          out->has_max = 1;
          out->min = 0;
          out->max = mask;
        } else if (have_l && have_r && l.has_min && r.has_min && l.min >= 0 &&
                   r.min >= 0 && (l.has_max || r.has_max)) {
          out->has_min = 1;
          out->min = 0;
          out->has_max = 1;
          out->max = l.has_max && r.has_max ? (l.max < r.max ? l.max : r.max)
                                            : (l.has_max ? l.max : r.max);
        }
      } else if (strcmp(op, ">>") == 0 && have_l && have_r && l.has_min &&
                 l.min >= 0 && r.has_min && r.has_max && r.min == r.max &&
                 r.min >= 0 && r.min < 63) {
        out->has_min = 1;
        out->min = l.min >> r.min;
        if (l.has_max) {
          out->has_max = 1;
          out->max = l.max >> r.min;
        }
      }
    }
    if (!out->has_min && !out->has_max) {
      Range fallback = range_of_type(expr->resolved_type);
      *out = fallback;
    }
    return out->has_min || out->has_max;
  }
  case AST_UNARY_EXPRESSION: {
    if (!unary_parts(expr, &op, &operand)) {
      return 0;
    }
    if (strcmp(op, "-") == 0) {
      Range inner;
      if (range_of(checker, operand, &inner, depth + 1)) {
        if (inner.has_max && inner.max > LLONG_MIN) {
          out->has_min = 1;
          out->min = -inner.max;
        }
        if (inner.has_min && inner.min > LLONG_MIN) {
          out->has_max = 1;
          out->max = -inner.min;
        }
      }
      return out->has_min || out->has_max;
    }
    if (strcmp(op, "!") == 0) {
      out->has_min = 1;
      out->has_max = 1;
      out->min = 0;
      out->max = 1;
      return 1;
    }
    return 0;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expr->data;
    Range target = range_of_type(expr->resolved_type);
    Range inner;
    if (cast && cast->operand && range_of(checker, cast->operand, &inner,
                                          depth + 1) &&
        inner.has_min && inner.has_max && target.has_min && target.has_max &&
        inner.min >= target.min && inner.max <= target.max) {
      *out = inner;
      return 1;
    }
    *out = target;
    return out->has_min || out->has_max;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expr->data;
    Type *object_type = member && member->object
                            ? member->object->resolved_type
                            : NULL;
    if (member && member->member && strcmp(member->member, "length") == 0 &&
        object_type) {
      if (object_type->kind == TYPE_ARRAY) {
        *out = range_exact((long long)object_type->array_size);
        return 1;
      }
      if (object_type->kind == TYPE_SLICE || object_type->kind == TYPE_STRING) {
        out->has_min = 1;
        out->min = 0;
        return 1;
      }
    }
    *out = range_of_type(expr->resolved_type);
    return out->has_min || out->has_max;
  }
  default:
    *out = range_of_type(expr->resolved_type);
    return out->has_min || out->has_max;
  }
}

int type_checker_expression_range(TypeChecker *checker, ASTNode *expr,
                                  int *has_min, long long *min, int *has_max,
                                  long long *max) {
  Range r;
  if (!checker || !expr) {
    return 0;
  }
  if (!range_of(checker, expr, &r, 0)) {
    return 0;
  }
  *has_min = r.has_min;
  *min = r.min;
  *has_max = r.has_max;
  *max = r.max;
  return 1;
}

static int guard_grow(TypeChecker *checker) {
  if (checker->guard_count < checker->guard_capacity) {
    return 1;
  }
  size_t next = checker->guard_capacity ? checker->guard_capacity * 2 : 16;
  struct TypeCheckerGuard *grown =
      realloc(checker->guards, next * sizeof(struct TypeCheckerGuard));
  if (!grown) {
    return 0;
  }
  checker->guards = grown;
  checker->guard_capacity = next;
  return 1;
}

int type_checker_push_guard(TypeChecker *checker, ASTNode *condition,
                            int negated) {
  if (!checker || !condition || !guard_grow(checker)) {
    return 0;
  }
  memset(&checker->guards[checker->guard_count], 0,
         sizeof(struct TypeCheckerGuard));
  checker->guards[checker->guard_count].condition = condition;
  checker->guards[checker->guard_count].negated = negated;
  checker->guard_count++;
  return 1;
}

int type_checker_push_range_guard(TypeChecker *checker, const char *name,
                                  int has_min, long long min, int has_max,
                                  long long max) {
  if (!checker || !name || !guard_grow(checker)) {
    return 0;
  }
  memset(&checker->guards[checker->guard_count], 0,
         sizeof(struct TypeCheckerGuard));
  checker->guards[checker->guard_count].name = name;
  checker->guards[checker->guard_count].has_min = has_min;
  checker->guards[checker->guard_count].min = min;
  checker->guards[checker->guard_count].has_max = has_max;
  checker->guards[checker->guard_count].max = max;
  checker->guard_count++;
  return 1;
}

size_t type_checker_guard_depth(const TypeChecker *checker) {
  return checker ? checker->guard_count : 0;
}

void type_checker_pop_guards(TypeChecker *checker, size_t depth) {
  if (checker && depth <= checker->guard_count) {
    checker->guard_count = depth;
  }
}

static int nodes_equal(const ASTNode *a, const ASTNode *b, const char *hole,
                       const ASTNode *filler, int depth);

static int nodes_equal_children(const ASTNode *a, const ASTNode *b,
                                const char *hole, const ASTNode *filler,
                                int depth) {
  if (a->child_count != b->child_count) {
    return 0;
  }
  for (size_t i = 0; i < a->child_count; i++) {
    if (!nodes_equal(a->children[i], b->children[i], hole, filler,
                     depth + 1)) {
      return 0;
    }
  }
  return 1;
}

static int nodes_equal(const ASTNode *a, const ASTNode *b, const char *hole,
                       const ASTNode *filler, int depth) {
  if (!a || !b || depth > 32) {
    return a == b;
  }
  if (hole && a->type == AST_IDENTIFIER) {
    const char *name = identifier_name(a);
    if (name && strcmp(name, hole) == 0) {
      return nodes_equal(filler, b, NULL, NULL, depth + 1);
    }
  }
  if (a->type != b->type) {
    return 0;
  }
  switch (a->type) {
  case AST_IDENTIFIER: {
    const char *na = identifier_name(a);
    const char *nb = identifier_name(b);
    return na && nb && strcmp(na, nb) == 0;
  }
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *la = (const NumberLiteral *)a->data;
    const NumberLiteral *lb = (const NumberLiteral *)b->data;
    if (!la || !lb || la->is_float != lb->is_float) {
      return 0;
    }
    return la->is_float ? la->float_value == lb->float_value
                        : la->int_value == lb->int_value;
  }
  case AST_STRING_LITERAL: {
    const StringLiteral *la = (const StringLiteral *)a->data;
    const StringLiteral *lb = (const StringLiteral *)b->data;
    return la && lb && la->value && lb->value &&
           strcmp(la->value, lb->value) == 0;
  }
  case AST_BINARY_EXPRESSION: {
    const char *oa = NULL;
    const char *ob = NULL;
    ASTNode *la = NULL;
    ASTNode *ra = NULL;
    ASTNode *lb = NULL;
    ASTNode *rb = NULL;
    if (!binary_parts(a, &oa, &la, &ra) || !binary_parts(b, &ob, &lb, &rb) ||
        strcmp(oa, ob) != 0) {
      return 0;
    }
    return nodes_equal(la, lb, hole, filler, depth + 1) &&
           nodes_equal(ra, rb, hole, filler, depth + 1);
  }
  case AST_UNARY_EXPRESSION: {
    const char *oa = NULL;
    const char *ob = NULL;
    ASTNode *pa = NULL;
    ASTNode *pb = NULL;
    if (!unary_parts(a, &oa, &pa) || !unary_parts(b, &ob, &pb) ||
        strcmp(oa, ob) != 0) {
      return 0;
    }
    return nodes_equal(pa, pb, hole, filler, depth + 1);
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *ma = (const MemberAccess *)a->data;
    const MemberAccess *mb = (const MemberAccess *)b->data;
    if (!ma || !mb || !ma->member || !mb->member ||
        strcmp(ma->member, mb->member) != 0) {
      return 0;
    }
    return nodes_equal(ma->object, mb->object, hole, filler, depth + 1);
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *ca = (const CallExpression *)a->data;
    const CallExpression *cb = (const CallExpression *)b->data;
    if (!ca || !cb || !ca->function_name || !cb->function_name ||
        strcmp(ca->function_name, cb->function_name) != 0 ||
        ca->argument_count != cb->argument_count ||
        (ca->object == NULL) != (cb->object == NULL)) {
      return 0;
    }
    if (ca->object && !nodes_equal(ca->object, cb->object, hole, filler,
                                   depth + 1)) {
      return 0;
    }
    for (size_t i = 0; i < ca->argument_count; i++) {
      if (!nodes_equal(ca->arguments[i], cb->arguments[i], hole, filler,
                       depth + 1)) {
        return 0;
      }
    }
    return 1;
  }
  case AST_INDEX_EXPRESSION:
  case AST_CAST_EXPRESSION:
    return nodes_equal_children(a, b, hole, filler, depth);
  default:
    return 0;
  }
}

typedef struct {
  const ASTNode *pattern;
  int pattern_negated;
  const ASTNode *filler;
  const char *binding;
  int found;
} MatchContext;

static void match_visit(void *raw, ASTNode *node, int negated) {
  MatchContext *ctx = (MatchContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  if (ctx->found) {
    return;
  }
  if (negated == ctx->pattern_negated &&
      nodes_equal(ctx->pattern, node, ctx->binding, ctx->filler, 0)) {
    ctx->found = 1;
    return;
  }
  if (negated != ctx->pattern_negated && binary_parts(node, &op, &left, &right) &&
      is_comparison(op)) {
    const char *pop = NULL;
    ASTNode *pleft = NULL;
    ASTNode *pright = NULL;
    const char *flipped = negate_operator(op);
    if (flipped && binary_parts(ctx->pattern, &pop, &pleft, &pright) &&
        strcmp(pop, flipped) == 0 &&
        nodes_equal(pleft, left, ctx->binding, ctx->filler, 0) &&
        nodes_equal(pright, right, ctx->binding, ctx->filler, 0)) {
      ctx->found = 1;
    }
  }
}

static int guards_prove_atom(TypeChecker *checker, ASTNode *atom,
                             int atom_negated, ASTNode *filler,
                             const char *binding) {
  MatchContext ctx;
  ctx.pattern = atom;
  ctx.pattern_negated = atom_negated;
  ctx.filler = filler;
  ctx.binding = binding;
  ctx.found = 0;
  for (size_t i = 0; i < checker->guard_count && !ctx.found; i++) {
    struct TypeCheckerGuard *guard = &checker->guards[i];
    if (guard->condition) {
      visit_atoms(guard->condition, guard->negated, match_visit, &ctx, 0);
    }
  }
  return ctx.found;
}

static int comparison_holds(const char *op, const Range *value,
                            const Range *bound) {
  if (strcmp(op, "<") == 0) {
    return value->has_max && bound->has_min && value->max < bound->min;
  }
  if (strcmp(op, "<=") == 0) {
    return value->has_max && bound->has_min && value->max <= bound->min;
  }
  if (strcmp(op, ">") == 0) {
    return value->has_min && bound->has_max && value->min > bound->max;
  }
  if (strcmp(op, ">=") == 0) {
    return value->has_min && bound->has_max && value->min >= bound->max;
  }
  if (strcmp(op, "==") == 0) {
    return value->has_min && value->has_max && bound->has_min &&
           bound->has_max && value->min == value->max &&
           bound->min == bound->max && value->min == bound->min;
  }
  if (strcmp(op, "!=") == 0) {
    return (value->has_max && bound->has_min && value->max < bound->min) ||
           (value->has_min && bound->has_max && value->min > bound->max);
  }
  return 0;
}

static void describe(const ASTNode *node, char *out, size_t capacity,
                     int depth);

static void describe_append(char *out, size_t capacity, const char *text) {
  size_t used = strlen(out);
  if (used + 1 < capacity) {
    snprintf(out + used, capacity - used, "%s", text);
  }
}

static void describe(const ASTNode *node, char *out, size_t capacity,
                     int depth) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  ASTNode *operand = NULL;
  char scratch[64];
  if (!node || depth > 8) {
    describe_append(out, capacity, "...");
    return;
  }
  switch (node->type) {
  case AST_IDENTIFIER:
    describe_append(out, capacity, identifier_name(node) ? identifier_name(node)
                                                         : "?");
    return;
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *literal = (const NumberLiteral *)node->data;
    if (literal && literal->is_float) {
      snprintf(scratch, sizeof(scratch), "%g", literal->float_value);
    } else {
      snprintf(scratch, sizeof(scratch), "%lld",
               literal ? literal->int_value : 0);
    }
    describe_append(out, capacity, scratch);
    return;
  }
  case AST_STRING_LITERAL:
    describe_append(out, capacity, "\"...\"");
    return;
  case AST_BINARY_EXPRESSION:
    if (binary_parts(node, &op, &left, &right)) {
      describe(left, out, capacity, depth + 1);
      describe_append(out, capacity, " ");
      describe_append(out, capacity, op);
      describe_append(out, capacity, " ");
      describe(right, out, capacity, depth + 1);
    }
    return;
  case AST_UNARY_EXPRESSION:
    if (unary_parts(node, &op, &operand)) {
      describe_append(out, capacity, op);
      describe(operand, out, capacity, depth + 1);
    }
    return;
  case AST_MEMBER_ACCESS: {
    const MemberAccess *member = (const MemberAccess *)node->data;
    if (member) {
      describe(member->object, out, capacity, depth + 1);
      describe_append(out, capacity, ".");
      describe_append(out, capacity, member->member ? member->member : "?");
    }
    return;
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)node->data;
    if (call) {
      describe_append(out, capacity,
                      call->written_name ? call->written_name
                                         : (call->function_name
                                                ? call->function_name
                                                : "?"));
      describe_append(out, capacity, "(");
      for (size_t i = 0; i < call->argument_count; i++) {
        if (i) {
          describe_append(out, capacity, ", ");
        }
        describe(call->arguments[i], out, capacity, depth + 1);
      }
      describe_append(out, capacity, ")");
    }
    return;
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)node->data;
    if (cast) {
      describe_append(out, capacity, "(");
      describe_append(out, capacity, cast->type_name ? cast->type_name : "?");
      describe_append(out, capacity, ")");
      describe(cast->operand, out, capacity, depth + 1);
    }
    return;
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index = (const ArrayIndexExpression *)node->data;
    if (index) {
      describe(index->array, out, capacity, depth + 1);
      describe_append(out, capacity, "[");
      describe(index->index, out, capacity, depth + 1);
      describe_append(out, capacity, "]");
    }
    return;
  }
  default:
    describe_append(out, capacity, "<expression>");
    return;
  }
}

typedef struct {
  TypeChecker *checker;
  ASTNode *expr;
  const char *binding;
  Range value_range;
  int have_range;
  int failed;
  ASTNode *failed_atom;
  int failed_negated;
  long long steps;
  char route[192];
} ProveContext;

static void prove_visit(void *raw, ASTNode *atom, int negated) {
  ProveContext *ctx = (ProveContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  if (ctx->failed) {
    return;
  }
  if (binary_parts(atom, &op, &left, &right) && is_comparison(op)) {
    const char *effective = negated ? negate_operator(op) : op;
    ASTNode *other = NULL;
    const char *use = NULL;
    if (identifier_name(left) &&
        strcmp(identifier_name(left), ctx->binding) == 0) {
      other = right;
      use = effective;
    } else if (identifier_name(right) &&
               strcmp(identifier_name(right), ctx->binding) == 0) {
      other = left;
      use = flip_operator(effective);
    }
    if (other && use && ctx->have_range) {
      Range bound;
      if (range_of(ctx->checker, other, &bound, 0) &&
          comparison_holds(use, &ctx->value_range, &bound)) {
        if (ctx->value_range.has_min && ctx->value_range.has_max) {
          snprintf(ctx->route, sizeof(ctx->route),
                   "the value's range %lld..%lld settles the comparison",
                   ctx->value_range.min, ctx->value_range.max);
        } else {
          snprintf(ctx->route, sizeof(ctx->route),
                   "the value's range settles the comparison");
        }
        return;
      }
    }
  }
  if (guards_prove_atom(ctx->checker, atom, negated, ctx->expr,
                        ctx->binding)) {
    snprintf(ctx->route, sizeof(ctx->route),
             "a dominating test in scope repeats the predicate");
    return;
  }
  ctx->failed = 1;
  ctx->failed_atom = atom;
  ctx->failed_negated = negated;
}

static void set_failure(TypeChecker *checker, const char *text) {
  free(checker->refine_failure);
  checker->refine_failure = strdup(text);
}

static char *proof_copy(const char *text) {
  char *copy = strdup(text ? text : "?");
  return copy;
}

static void proof_log(TypeChecker *checker, const Type *layer,
                      const ASTNode *expr, const char *proof, int proven,
                      long long steps) {
  TypeCheckerProof *entry;
  char expr_text[160] = "";
  if (checker->proof_log_count == checker->proof_log_capacity) {
    size_t grown =
        checker->proof_log_capacity ? checker->proof_log_capacity * 2 : 16;
    TypeCheckerProof *table = (TypeCheckerProof *)realloc(
        checker->proof_log, grown * sizeof(TypeCheckerProof));
    if (!table) {
      return;
    }
    checker->proof_log = table;
    checker->proof_log_capacity = grown;
  }
  describe((ASTNode *)expr, expr_text, sizeof(expr_text), 0);
  entry = &checker->proof_log[checker->proof_log_count++];
  entry->type_name = proof_copy(layer && layer->name ? layer->name : "?");
  entry->expression = proof_copy(expr_text);
  entry->proof = proof_copy(proof);
  entry->line = expr ? expr->location.line : 0;
  entry->column = expr ? expr->location.column : 0;
  entry->proven = proven;
  entry->steps = steps;
}

long long type_checker_proof_steps(const TypeChecker *checker) {
  return checker ? checker->proof_steps : 0;
}

void type_checker_report_proofs(const TypeChecker *checker, FILE *out) {
  if (!checker || !out) {
    return;
  }
  for (size_t i = 0; i < checker->proof_log_count; i++) {
    const TypeCheckerProof *p = &checker->proof_log[i];
    fprintf(out, "proof %s for `%s` at %zu:%zu: %s, %lld steps (%s)\n",
            p->type_name, p->expression, p->line, p->column,
            p->proven ? "proven" : "refused", p->steps, p->proof);
  }
  fprintf(out,
          "proofs: %zu attempted, %zu proven, %zu refused, %lld steps\n",
          checker->proofs_attempted, checker->proofs_proven,
          checker->proofs_refused, checker->proof_steps);
}

int type_checker_prove_refinement(TypeChecker *checker, Type *refined,
                                  ASTNode *expr) {
  ProveContext ctx;
  Type *layer;
  if (!checker || !refined || !expr) {
    return 0;
  }
  free(checker->refine_failure);
  checker->refine_failure = NULL;
  if (getenv("METTLE_TRUST_REFINEMENTS")) {
    expr->proven_refinement = refined;
    return 1;
  }
  memset(&ctx, 0, sizeof(ctx));
  ctx.checker = checker;
  ctx.expr = expr;
  ctx.steps = checker->proof_steps;
  snprintf(ctx.route, sizeof(ctx.route), "the value's own type admits it");
  checker->proofs_attempted++;
  ctx.have_range = range_of(checker, expr, &ctx.value_range, 0);
  for (layer = refined; layer && layer->refined_base; layer = layer->refined_base) {
    if (expr->resolved_type &&
        type_checker_types_equal(expr->resolved_type, layer)) {
      break;
    }
    if (!layer->refinement) {
      continue;
    }
    ctx.binding = layer->refine_binding ? layer->refine_binding : "value";
    visit_atoms(layer->refinement, 0, prove_visit, &ctx, 0);
    if (ctx.failed) {
      char atom_text[160] = "";
      char expr_text[160] = "";
      char message[640];
      char range_text[96] = "";
      describe(ctx.failed_atom, atom_text, sizeof(atom_text), 0);
      describe(expr, expr_text, sizeof(expr_text), 0);
      if (ctx.have_range && ctx.value_range.has_min && ctx.value_range.has_max) {
        snprintf(range_text, sizeof(range_text), " (its range here is %lld..%lld)",
                 ctx.value_range.min, ctx.value_range.max);
      } else if (ctx.have_range && ctx.value_range.has_min) {
        snprintf(range_text, sizeof(range_text), " (all that is known is that it is at least %lld)",
                 ctx.value_range.min);
      } else if (ctx.have_range && ctx.value_range.has_max) {
        snprintf(range_text, sizeof(range_text), " (all that is known is that it is at most %lld)",
                 ctx.value_range.max);
      }
      snprintf(message, sizeof(message),
               "cannot prove `%s%s` for `%s`, which '%s' requires%s",
               ctx.failed_negated ? "!" : "", atom_text, expr_text,
               layer->name ? layer->name : "?", range_text);
      set_failure(checker, message);
      checker->proofs_refused++;
      proof_log(checker, layer, expr, message, 0,
                checker->proof_steps - ctx.steps);
      return 0;
    }
    proof_log(checker, layer, expr, ctx.route, 1,
              checker->proof_steps - ctx.steps);
    {
      char expr_text[160] = "";
      describe(expr, expr_text, sizeof(expr_text), 0);
      ir_explain_proof_held(layer->name ? layer->name : "?", expr_text,
                            expr->location.line, ctx.route,
                            "the type checker, and by lowering where it "
                            "decides whether an access needs a check");
    }
  }
  expr->proven_refinement = refined;
  checker->proofs_proven++;
  return 1;
}

void type_checker_compute_refinement_range(TypeChecker *checker,
                                           Type *refined) {
  Range r;
  Type *base;
  if (!checker || !refined || !refined->refined_base) {
    return;
  }
  base = refined->refined_base;
  r = range_of_type(base);
  if (!type_checker_is_integer_type(base) || base->kind == TYPE_ENUM) {
    return;
  }
  if (refined->refinement) {
    NarrowContext ctx;
    ctx.checker = checker;
    ctx.name = refined->refine_binding ? refined->refine_binding : "value";
    ctx.range = &r;
    ctx.depth = 0;
    visit_atoms(refined->refinement, 0, narrow_visit, &ctx, 0);
  }
  if (r.has_min && r.has_max) {
    refined->refine_has_range = 1;
    refined->refine_min = r.min;
    refined->refine_max = r.max;
  }
}

int type_checker_refined_index_fits(const Type *index_type, size_t length) {
  if (!index_type || !index_type->refined_base || !index_type->refine_has_range) {
    return 0;
  }
  return index_type->refine_min >= 0 &&
         index_type->refine_max >= 0 &&
         (unsigned long long)index_type->refine_max < (unsigned long long)length;
}

void type_checker_report_refinement_failure(TypeChecker *checker,
                                            const ASTNode *expr,
                                            SourceLocation location) {
  char help[256];
  if (!checker || !checker->refine_failure) {
    return;
  }
  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(checker->refine_failure);
  if (checker->error_reporter) {
    SourceLocation where = expr ? expr->location : location;
    size_t span_length = expr ? type_checker_node_span_length(expr) : 1;
    SourceSpan span = source_span_from_location(where, span_length);
    snprintf(help, sizeof(help),
             "guard the value where it is converted, for example `if (x >= 0 "
             "&& x <= 100) { ... }`; the compiler proves what a dominating "
             "test, a constant, or a narrower type establishes, and refuses "
             "to guess past that");
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_TYPE, span, checker->refine_failure,
        help);
    error_reporter_set_last_code(checker->error_reporter, "P0001");
    error_reporter_set_last_label(checker->error_reporter, "unproven here");
  }
  free(checker->refine_failure);
  checker->refine_failure = NULL;
}
