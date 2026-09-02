#include "ir_optimize_internal.h"
#include "../ir_interp.h"
#include "../ir_verify.h"
#include "../../common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RW_MAX_RULES 64
#define RW_MAX_NODES 48
#define RW_MAX_VARS 8
#define RW_MAX_CHILDREN 8
#define RW_MAX_APPLICATIONS_PER_FUNCTION 64
#define RW_GUARD_FUEL 200000LL

typedef enum { RW_NODE_VAR, RW_NODE_INT, RW_NODE_FLOAT, RW_NODE_OP } RwNodeKind;

typedef struct {
  RwNodeKind kind;
  int var;
  long long int_value;
  double float_value;
  int float_bits;
  const IRInstruction *insn;
  int children[RW_MAX_CHILDREN];
  int child_count;
} RwNode;

typedef struct {
  IRFunction *fn;
  RwNode nodes[RW_MAX_NODES];
  int node_count;
  int root;
  unsigned param_mask;
} RwSide;

typedef struct {
  const char *name;
  RwSide from;
  RwSide to;
  IRFunction *where;
  unsigned where_mask;
  int checked_runs;
  int guard_hits;
  long applications;
  long guard_undecided;
  long guard_false;
  const char *undecided_param;
  SourceLocation location;
} RwRule;

typedef struct {
  const IROperand *ops[RW_MAX_VARS];
  size_t use_index[RW_MAX_VARS];
  unsigned bound;
  size_t producers[RW_MAX_NODES];
  int producer_count;
  size_t lowest;
} RwMatch;

static RwRule g_rules[RW_MAX_RULES];
static size_t g_rule_count = 0;
static IRProgram *g_program = NULL;
static int g_failed = 0;
static unsigned long g_serial = 0;

static void rw_report_error(const IRFunction *fn, const char *format, ...) {
  va_list args;
  const char *file = fn && fn->location.filename ? fn->location.filename : "?";
  fprintf(stderr, "%s:%zu:%zu: error: ", file, fn ? fn->location.line : 0,
          fn ? fn->location.column : 0);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
  ir_optimize_note_user_error();
  g_failed = 1;
}

static const char *rw_rule_name_of(const IRFunction *fn) {
  static const struct {
    const char *prefix;
    int role;
  } prefixes[] = {
      {IR_REWRITE_FROM_PREFIX, IR_REWRITE_ROLE_FROM},
      {IR_REWRITE_TO_PREFIX, IR_REWRITE_ROLE_TO},
      {IR_REWRITE_WHERE_PREFIX, IR_REWRITE_ROLE_WHERE},
  };
  if (!fn || !fn->name) {
    return NULL;
  }
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    size_t n = strlen(prefixes[i].prefix);
    if (fn->rewrite_role == prefixes[i].role &&
        strncmp(fn->name, prefixes[i].prefix, n) == 0) {
      return fn->name + n;
    }
  }
  return NULL;
}

static int rw_param_index(const IRFunction *fn, const char *name) {
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (fn->parameter_names[i] && strcmp(fn->parameter_names[i], name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static int rw_operand_slot_count(const IRInstruction *insn) {
  switch (insn->op) {
  case IR_OP_BINARY:
    return 2;
  case IR_OP_UNARY:
  case IR_OP_CAST:
    return 1;
  case IR_OP_CALL:
    return (int)insn->argument_count;
  default:
    return -1;
  }
}

static const IROperand *rw_operand_slot(const IRInstruction *insn, int slot) {
  switch (insn->op) {
  case IR_OP_BINARY:
    return slot == 0 ? &insn->lhs : &insn->rhs;
  case IR_OP_UNARY:
  case IR_OP_CAST:
    return &insn->lhs;
  case IR_OP_CALL:
    return &insn->arguments[slot];
  default:
    return NULL;
  }
}

static IROperand *rw_operand_slot_mut(IRInstruction *insn, int slot) {
  return (IROperand *)rw_operand_slot(insn, slot);
}

static const IRInstruction *rw_return_of(const IRFunction *fn) {
  for (size_t i = fn->instruction_count; i > 0;) {
    i--;
    if (fn->instructions[i].op == IR_OP_RETURN) {
      return &fn->instructions[i];
    }
  }
  return NULL;
}

static size_t rw_index_of(const IRFunction *fn, const IRInstruction *insn) {
  return (size_t)(insn - fn->instructions);
}

static int rw_compile_operand(RwSide *side, const IROperand *operand,
                              size_t use_index, char *why, size_t why_cap);

static const IRInstruction *rw_side_producer(const IRFunction *fn,
                                             size_t before, const char *name) {
  for (size_t i = before; i > 0;) {
    i--;
    const IRInstruction *insn = &fn->instructions[i];
    if (insn->op != IR_OP_NOP && insn->dest.kind == IR_OPERAND_TEMP &&
        insn->dest.name && strcmp(insn->dest.name, name) == 0) {
      return insn;
    }
  }
  return NULL;
}

static int rw_compile_instruction(RwSide *side, const IRInstruction *insn,
                                  char *why, size_t why_cap) {
  int slots = rw_operand_slot_count(insn);
  if (slots < 0) {
    snprintf(why, why_cap,
             "it uses a construct a rule cannot express (only arithmetic, "
             "casts and calls can be matched)");
    return -1;
  }
  if (slots > RW_MAX_CHILDREN) {
    snprintf(why, why_cap, "a call with more than %d arguments",
             RW_MAX_CHILDREN);
    return -1;
  }
  if (side->node_count >= RW_MAX_NODES) {
    snprintf(why, why_cap, "it has more than %d operations", RW_MAX_NODES);
    return -1;
  }
  int index = side->node_count++;
  RwNode *node = &side->nodes[index];
  memset(node, 0, sizeof(*node));
  node->kind = RW_NODE_OP;
  node->insn = insn;
  node->child_count = slots;
  size_t at = rw_index_of(side->fn, insn);
  for (int s = 0; s < slots; s++) {
    int child = rw_compile_operand(side, rw_operand_slot(insn, s), at, why,
                                   why_cap);
    if (child < 0) {
      return -1;
    }
    side->nodes[index].children[s] = child;
  }
  return index;
}

static int rw_compile_operand(RwSide *side, const IROperand *operand,
                              size_t use_index, char *why, size_t why_cap) {
  if (side->node_count >= RW_MAX_NODES) {
    snprintf(why, why_cap, "it has more than %d operations", RW_MAX_NODES);
    return -1;
  }
  switch (operand->kind) {
  case IR_OPERAND_TEMP: {
    const IRInstruction *producer =
        rw_side_producer(side->fn, use_index, operand->name);
    if (!producer) {
      snprintf(why, why_cap, "the value `%s` has no producer", operand->name);
      return -1;
    }
    if (producer->op == IR_OP_ASSIGN) {
      return rw_compile_operand(side, &producer->lhs,
                                rw_index_of(side->fn, producer), why, why_cap);
    }
    return rw_compile_instruction(side, producer, why, why_cap);
  }
  case IR_OPERAND_SYMBOL: {
    int param = rw_param_index(side->fn, operand->name);
    if (param < 0) {
      snprintf(why, why_cap,
               "it reads `%s`, which is not one of the rule's parameters",
               operand->name);
      return -1;
    }
    if (param >= RW_MAX_VARS) {
      snprintf(why, why_cap, "it has more than %d parameters", RW_MAX_VARS);
      return -1;
    }
    int index = side->node_count++;
    RwNode *node = &side->nodes[index];
    memset(node, 0, sizeof(*node));
    node->kind = RW_NODE_VAR;
    node->var = param;
    side->param_mask |= 1u << param;
    return index;
  }
  case IR_OPERAND_INT: {
    int index = side->node_count++;
    RwNode *node = &side->nodes[index];
    memset(node, 0, sizeof(*node));
    node->kind = RW_NODE_INT;
    node->int_value = operand->int_value;
    return index;
  }
  case IR_OPERAND_FLOAT: {
    int index = side->node_count++;
    RwNode *node = &side->nodes[index];
    memset(node, 0, sizeof(*node));
    node->kind = RW_NODE_FLOAT;
    node->float_value = operand->float_value;
    node->float_bits = operand->float_bits;
    return index;
  }
  default:
    snprintf(why, why_cap, "it uses a string or label operand");
    return -1;
  }
}

static int rw_compile_side(IRFunction *fn, RwSide *side, char *why,
                           size_t why_cap) {
  const IRInstruction *ret = rw_return_of(fn);
  memset(side, 0, sizeof(*side));
  side->fn = fn;
  if (!ret || ret->lhs.kind == IR_OPERAND_NONE) {
    snprintf(why, why_cap, "it produces no value");
    return 0;
  }
  side->root = rw_compile_operand(side, &ret->lhs, rw_index_of(fn, ret), why,
                                  why_cap);
  return side->root >= 0;
}

static unsigned rw_symbol_reads(const IRFunction *fn) {
  unsigned mask = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    const IROperand *ops[2] = {&insn->lhs, &insn->rhs};
    for (int k = 0; k < 2; k++) {
      if (ops[k]->kind == IR_OPERAND_SYMBOL && ops[k]->name) {
        int p = rw_param_index(fn, ops[k]->name);
        if (p >= 0 && p < RW_MAX_VARS) {
          mask |= 1u << p;
        }
      }
    }
    for (size_t a = 0; a < insn->argument_count; a++) {
      const IROperand *arg = &insn->arguments[a];
      if (arg->kind == IR_OPERAND_SYMBOL && arg->name) {
        int p = rw_param_index(fn, arg->name);
        if (p >= 0 && p < RW_MAX_VARS) {
          mask |= 1u << p;
        }
      }
    }
  }
  return mask;
}

static const char *rw_first_param_in(const IRFunction *fn, unsigned mask) {
  for (size_t i = 0; i < fn->parameter_count && i < RW_MAX_VARS; i++) {
    if (mask & (1u << i)) {
      return fn->parameter_names[i];
    }
  }
  return "?";
}

static RwRule *rw_rule_named(const char *name) {
  for (size_t i = 0; i < g_rule_count; i++) {
    if (strcmp(g_rules[i].name, name) == 0) {
      return &g_rules[i];
    }
  }
  if (g_rule_count >= RW_MAX_RULES) {
    return NULL;
  }
  RwRule *rule = &g_rules[g_rule_count++];
  memset(rule, 0, sizeof(*rule));
  rule->name = name;
  return rule;
}

static const long long RW_INT_PROBES[] = {
    -1LL,
    -2LL,
    -7LL,
    -63LL,
    -64LL,
    63LL,
    64LL,
    255LL,
    256LL,
    1023LL,
    65535LL,
    65536LL,
    2147483647LL,
    -2147483648LL,
    4294967295LL,
    4294967296LL,
    9223372036854775807LL,
    (-9223372036854775807LL - 1LL),
};

static void rw_strip_side_prefix(char *text) {
  static const char *const prefixes[] = {
      IR_REWRITE_FROM_PREFIX, IR_REWRITE_TO_PREFIX, IR_REWRITE_WHERE_PREFIX};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    char *at = strstr(text, prefixes[i]);
    if (at) {
      memmove(at, at + strlen(prefixes[i]), strlen(at + strlen(prefixes[i])) + 1);
    }
  }
}

static int rw_self_check(RwRule *rule) {
  IRFunction *from = rule->from.fn;
  IRFunction *to = rule->to.fn;
  char why[192];
  char cex[288];
  char skip[160];
  IRVerifySnapshot *before = ir_verify_snapshot_capture(from);
  if (!before) {
    rw_report_error(from, "rewrite `%s` could not be checked: its `from` side "
                          "is not executable",
                    rule->name);
    return 0;
  }
  IRVerifyRewriteVerdict verdict = ir_verify_check_rewrite_probed(
      g_program, to, before, rule->where, &rule->guard_hits, RW_INT_PROBES,
      (int)(sizeof(RW_INT_PROBES) / sizeof(RW_INT_PROBES[0])), why,
      sizeof(why), cex, sizeof(cex), skip, sizeof(skip));
  ir_verify_snapshot_free(before);
  rule->checked_runs = ir_verify_last_input_run_count();
  switch (verdict) {
  case IR_VERIFY_REWRITE_VALIDATED:
    return 1;
  case IR_VERIFY_REWRITE_DIVERGED: {
    rw_strip_side_prefix(cex);
    rw_report_error(from,
                    "rewrite `%s` changes meaning: its `from` and `to` sides "
                    "disagree",
                    rule->name);
    fprintf(stderr, "  counterexample %s\n", cex);
    fprintf(stderr, "  divergence: %s\n", why);
    fprintf(stderr,
            "  a rule is applied only after `from` and `to` agree on every "
            "generated input; fix the rule or narrow it with `where`\n");
    return 0;
  }
  default:
    rw_report_error(from,
                    "rewrite `%s` could not be checked (%s); a rule is applied "
                    "only after the compiler has run both of its sides",
                    rule->name, skip);
    return 0;
  }
}

int ir_user_rewrite_begin(IRProgram *program) {
  g_rule_count = 0;
  g_program = program;
  g_failed = 0;
  if (!program) {
    return 1;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    const char *name = rw_rule_name_of(fn);
    if (!name) {
      continue;
    }
    RwRule *rule = rw_rule_named(name);
    if (!rule) {
      rw_report_error(fn, "more than %d rewrite rules in one program",
                      RW_MAX_RULES);
      return 0;
    }
    if (fn->rewrite_role == IR_REWRITE_ROLE_FROM) {
      rule->from.fn = fn;
      rule->location = fn->location;
    } else if (fn->rewrite_role == IR_REWRITE_ROLE_TO) {
      rule->to.fn = fn;
    } else {
      rule->where = fn;
    }
  }
  for (size_t r = 0; r < g_rule_count; r++) {
    RwRule *rule = &g_rules[r];
    char why[192] = "";
    if (!rule->from.fn || !rule->to.fn) {
      rw_report_error(rule->from.fn ? rule->from.fn : rule->to.fn,
                      "rewrite `%s` is missing one of its sides", rule->name);
      return 0;
    }
    if (!rw_compile_side(rule->from.fn, &rule->from, why, sizeof(why))) {
      rw_report_error(rule->from.fn, "rewrite `%s`: its `from` side cannot be "
                                     "used as a pattern: %s",
                      rule->name, why);
      return 0;
    }
    if (rule->from.nodes[rule->from.root].kind != RW_NODE_OP) {
      rw_report_error(rule->from.fn,
                      "rewrite `%s`: its `from` side must apply an operator, "
                      "a cast or a call; a bare parameter or constant matches "
                      "everything",
                      rule->name);
      return 0;
    }
    if (!rw_compile_side(rule->to.fn, &rule->to, why, sizeof(why))) {
      rw_report_error(rule->to.fn, "rewrite `%s`: its `to` side cannot be "
                                   "used as a replacement: %s",
                      rule->name, why);
      return 0;
    }
    unsigned unbound = rule->to.param_mask & ~rule->from.param_mask;
    if (unbound) {
      rw_report_error(rule->to.fn,
                      "rewrite `%s`: `to` reads parameter `%s`, which `from` "
                      "never binds",
                      rule->name, rw_first_param_in(rule->to.fn, unbound));
      return 0;
    }
    if (rule->where) {
      rule->where_mask = rw_symbol_reads(rule->where);
      unbound = rule->where_mask & ~rule->from.param_mask;
      if (unbound) {
        rw_report_error(rule->where,
                        "rewrite `%s`: `where` reads parameter `%s`, which "
                        "`from` never binds",
                        rule->name, rw_first_param_in(rule->where, unbound));
        return 0;
      }
    }
    if (!rw_self_check(rule)) {
      return 0;
    }
    if (ir_explain_enabled()) {
      char verified[192];
      if (rule->where) {
        snprintf(verified, sizeof(verified),
                 "`from` and `to` agree on the %d generated input sets that "
                 "satisfy `where` (%d generated in all)",
                 rule->guard_hits, rule->checked_runs);
      } else {
        snprintf(verified, sizeof(verified),
                 "`from` and `to` agree on all %d generated input sets",
                 rule->checked_runs);
      }
      ir_explain_remark(rule->name, "rewrite rule", rule->location, 1,
                        "rule checked", NULL, NULL, verified);
      ir_explain_remark_code("rewrite-rule-checked");
    }
  }
  return 1;
}

static int rw_op_commutative(const IRInstruction *insn) {
  if (insn->op != IR_OP_BINARY || !insn->text) {
    return 0;
  }
  return strcmp(insn->text, "+") == 0 || strcmp(insn->text, "*") == 0 ||
         strcmp(insn->text, "&") == 0 || strcmp(insn->text, "|") == 0 ||
         strcmp(insn->text, "^") == 0 || strcmp(insn->text, "==") == 0 ||
         strcmp(insn->text, "!=") == 0;
}

static int rw_types_agree(const MtlcType *a, const MtlcType *b) {
  if (!a || !b) {
    return 1;
  }
  return a->kind == b->kind && a->size == b->size;
}

static int rw_same_shape(const IRInstruction *pattern,
                         const IRInstruction *candidate) {
  if (pattern->op != candidate->op || pattern->is_float != candidate->is_float ||
      pattern->argument_count != candidate->argument_count) {
    return 0;
  }
  if (pattern->is_float &&
      (pattern->float_bits ? pattern->float_bits : 64) !=
          (candidate->float_bits ? candidate->float_bits : 64)) {
    return 0;
  }
  if (pattern->op == IR_OP_BINARY && pattern->is_unsigned != candidate->is_unsigned) {
    return 0;
  }
  if ((pattern->text == NULL) != (candidate->text == NULL)) {
    return 0;
  }
  if (pattern->text && strcmp(pattern->text, candidate->text) != 0) {
    return 0;
  }
  if (pattern->op == IR_OP_CALL && pattern->intrinsic != candidate->intrinsic) {
    return 0;
  }
  return rw_types_agree(pattern->value_type, candidate->value_type);
}

static int rw_writes_name(const IRInstruction *insn, IROperandKind kind,
                          const char *name) {
  return insn->op != IR_OP_NOP && insn->dest.kind == kind && insn->dest.name &&
         strcmp(insn->dest.name, name) == 0;
}

static int rw_unchanged_between(const IRFunction *fn, size_t from, size_t to,
                                const IROperand *operand) {
  if (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL) {
    return 1;
  }
  int is_global = operand->kind == IR_OPERAND_SYMBOL &&
                  !ir_function_symbol_is_parameter(fn, operand->name) &&
                  !ir_function_local_declared_type(fn, operand->name);
  for (size_t i = from; i < to; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (rw_writes_name(insn, operand->kind, operand->name)) {
      return 0;
    }
    if (is_global &&
        (insn->op == IR_OP_CALL || insn->op == IR_OP_CALL_INDIRECT ||
         insn->op == IR_OP_STORE || insn->op == IR_OP_INLINE_ASM)) {
      return 0;
    }
  }
  return 1;
}

static int rw_match_node(const IRFunction *fn, const RwSide *side, int index,
                         const IROperand *operand, size_t use_index,
                         RwMatch *m);

static int rw_match_children(const IRFunction *fn, const RwSide *side,
                             const RwNode *node, const IRInstruction *insn,
                             size_t at, int swapped, RwMatch *m) {
  for (int s = 0; s < node->child_count; s++) {
    int slot = swapped ? (s == 0 ? 1 : 0) : s;
    if (!rw_match_node(fn, side, node->children[s], rw_operand_slot(insn, slot),
                       at, m)) {
      return 0;
    }
  }
  return 1;
}

static int rw_match_node(const IRFunction *fn, const RwSide *side, int index,
                         const IROperand *operand, size_t use_index,
                         RwMatch *m) {
  const RwNode *node = &side->nodes[index];
  switch (node->kind) {
  case RW_NODE_VAR:
    if (operand->kind == IR_OPERAND_NONE || operand->kind == IR_OPERAND_LABEL ||
        operand->kind == IR_OPERAND_STRING) {
      return 0;
    }
    if (m->bound & (1u << node->var)) {
      if (!ir_operand_equals(m->ops[node->var], operand)) {
        return 0;
      }
      if (use_index < m->use_index[node->var]) {
        m->use_index[node->var] = use_index;
      }
      return 1;
    }
    m->bound |= 1u << node->var;
    m->ops[node->var] = operand;
    m->use_index[node->var] = use_index;
    return 1;
  case RW_NODE_INT:
    return operand->kind == IR_OPERAND_INT &&
           operand->int_value == node->int_value;
  case RW_NODE_FLOAT:
    return operand->kind == IR_OPERAND_FLOAT &&
           memcmp(&operand->float_value, &node->float_value,
                  sizeof(double)) == 0 &&
           (operand->float_bits ? operand->float_bits : 64) ==
               (node->float_bits ? node->float_bits : 64);
  case RW_NODE_OP: {
    if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
      return 0;
    }
    const IRInstruction *producer =
        ir_find_temp_producer_before(fn, use_index, operand->name);
    if (!producer) {
      return 0;
    }
    size_t at = rw_index_of(fn, producer);
    if (!rw_unchanged_between(fn, at + 1, use_index, operand)) {
      return 0;
    }
    if (producer->op == IR_OP_ASSIGN) {
      return rw_match_node(fn, side, index, &producer->lhs, at, m);
    }
    if (!rw_same_shape(node->insn, producer)) {
      return 0;
    }
    if (m->producer_count >= RW_MAX_NODES) {
      return 0;
    }
    RwMatch saved = *m;
    m->producers[m->producer_count++] = at;
    if (at < m->lowest) {
      m->lowest = at;
    }
    if (rw_match_children(fn, side, node, producer, at, 0, m)) {
      return 1;
    }
    *m = saved;
    if (!rw_op_commutative(node->insn)) {
      return 0;
    }
    m->producers[m->producer_count++] = at;
    if (at < m->lowest) {
      m->lowest = at;
    }
    if (rw_match_children(fn, side, node, producer, at, 1, m)) {
      return 1;
    }
    *m = saved;
    return 0;
  }
  }
  return 0;
}

static int rw_match_root(const IRFunction *fn, const RwRule *rule,
                         size_t root_index, RwMatch *m) {
  const IRInstruction *root = &fn->instructions[root_index];
  const RwNode *node = &rule->from.nodes[rule->from.root];
  memset(m, 0, sizeof(*m));
  m->lowest = root_index;
  if (!rw_same_shape(node->insn, root)) {
    return 0;
  }
  if (rw_match_children(fn, &rule->from, node, root, root_index, 0, m)) {
    return 1;
  }
  memset(m, 0, sizeof(*m));
  m->lowest = root_index;
  if (!rw_op_commutative(node->insn)) {
    return 0;
  }
  return rw_match_children(fn, &rule->from, node, root, root_index, 1, m);
}

static int rw_bindings_stable(const IRFunction *fn, const RwMatch *m,
                              size_t root_index) {
  for (int v = 0; v < RW_MAX_VARS; v++) {
    if (!(m->bound & (1u << v))) {
      continue;
    }
    if (!rw_unchanged_between(fn, m->use_index[v], root_index, m->ops[v])) {
      return 0;
    }
  }
  return 1;
}

static int rw_guard_admits(RwRule *rule, const RwMatch *m) {
  IRInterpValue args[RW_MAX_VARS];
  IRInterpValue verdict = {0, 0, 0, 0};
  memset(args, 0, sizeof(args));
  if (!rule->where) {
    return 1;
  }
  for (size_t p = 0; p < rule->where->parameter_count && p < RW_MAX_VARS; p++) {
    if (!(rule->where_mask & (1u << p))) {
      continue;
    }
    const IROperand *op = m->ops[p];
    if (op->kind == IR_OPERAND_INT) {
      args[p].i = op->int_value;
    } else if (op->kind == IR_OPERAND_FLOAT) {
      args[p].f = op->float_value;
      args[p].is_float = 1;
    } else {
      rule->guard_undecided++;
      rule->undecided_param = rule->where->parameter_names[p];
      return 0;
    }
  }
  IRInterpMachine *machine = ir_interp_create(g_program);
  if (!machine) {
    return 0;
  }
  IRInterpStatus status =
      ir_interp_run(machine, rule->where, args, rule->where->parameter_count,
                    &verdict, RW_GUARD_FUEL);
  ir_interp_destroy(machine);
  if (status != IR_INTERP_OK || verdict.undefined) {
    rule->guard_false++;
    return 0;
  }
  if (verdict.is_float ? (verdict.f == 0.0) : (verdict.i == 0)) {
    rule->guard_false++;
    return 0;
  }
  return 1;
}

static int rw_temp_has_reader(const IRFunction *fn, const char *name,
                              size_t except) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (i == except || insn->op == IR_OP_NOP) {
      continue;
    }
    const IROperand *ops[2] = {&insn->lhs, &insn->rhs};
    for (int k = 0; k < 2; k++) {
      if (ops[k]->kind == IR_OPERAND_TEMP && ops[k]->name &&
          strcmp(ops[k]->name, name) == 0) {
        return 1;
      }
    }
    for (size_t a = 0; a < insn->argument_count; a++) {
      if (insn->arguments[a].kind == IR_OPERAND_TEMP &&
          insn->arguments[a].name &&
          strcmp(insn->arguments[a].name, name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

typedef struct {
  const IRInstruction *source;
  char *fresh_dest;
} RwCloneName;

static const char *rw_fresh_for(const RwCloneName *names, size_t count,
                                const char *original) {
  for (size_t i = 0; i < count; i++) {
    if (names[i].source->dest.name &&
        strcmp(names[i].source->dest.name, original) == 0) {
      return names[i].fresh_dest;
    }
  }
  return NULL;
}

static int rw_substitute(IROperand *operand, const RwRule *rule,
                         const RwMatch *m, const RwCloneName *names,
                         size_t name_count) {
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    int p = rw_param_index(rule->to.fn, operand->name);
    if (p >= 0) {
      IROperand bound = ir_operand_copy(m->ops[p]);
      if (m->ops[p]->name && !bound.name) {
        return 0;
      }
      ir_operand_destroy(operand);
      *operand = bound;
    }
    return 1;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    const char *fresh = rw_fresh_for(names, name_count, operand->name);
    if (fresh) {
      IROperand renamed = ir_operand_temp(fresh);
      if (!renamed.name) {
        return 0;
      }
      ir_operand_destroy(operand);
      *operand = renamed;
    }
    return 1;
  }
  return 1;
}

static int rw_collect_needed(const RwSide *side, int index,
                             const IRInstruction **needed, size_t *count) {
  const RwNode *node = &side->nodes[index];
  if (node->kind != RW_NODE_OP) {
    return 1;
  }
  for (size_t i = 0; i < *count; i++) {
    if (needed[i] == node->insn) {
      return 1;
    }
  }
  if (*count >= RW_MAX_NODES) {
    return 0;
  }
  needed[(*count)++] = node->insn;
  for (int c = 0; c < node->child_count; c++) {
    if (!rw_collect_needed(side, node->children[c], needed, count)) {
      return 0;
    }
  }
  return 1;
}

static int rw_clone_instruction(IRInstruction *clone, const IRInstruction *src,
                                SourceLocation location) {
  *clone = *src;
  clone->tensor = NULL;
  clone->arguments = NULL;
  clone->argument_types = NULL;
  clone->argument_count = 0;
  clone->text = NULL;
  clone->ast_ref = NULL;
  clone->expansion_note = NULL;
  clone->location = location;
  clone->dest = ir_operand_copy(&src->dest);
  clone->lhs = ir_operand_copy(&src->lhs);
  clone->rhs = ir_operand_copy(&src->rhs);
  if (!ir_instruction_tensor_copy(clone, src)) {
    return 0;
  }
  if (src->text) {
    clone->text = mettle_strdup(src->text);
    if (!clone->text) {
      return 0;
    }
  }
  if (src->argument_count > 0) {
    clone->arguments = calloc(src->argument_count, sizeof(IROperand));
    if (!clone->arguments) {
      return 0;
    }
    for (size_t a = 0; a < src->argument_count; a++) {
      clone->arguments[a] = ir_operand_copy(&src->arguments[a]);
    }
    clone->argument_count = src->argument_count;
    if (src->argument_types) {
      clone->argument_types =
          malloc(src->argument_count * sizeof(*clone->argument_types));
      if (!clone->argument_types) {
        return 0;
      }
      memcpy(clone->argument_types, src->argument_types,
             src->argument_count * sizeof(*clone->argument_types));
    }
  }
  return 1;
}

static int rw_apply(IRFunction *fn, const RwRule *rule, size_t root_index,
                    const RwMatch *m, int *changed) {
  const IRInstruction *needed[RW_MAX_NODES];
  size_t needed_count = 0;
  RwCloneName names[RW_MAX_NODES];
  size_t name_count = 0;
  IRInstruction clones[RW_MAX_NODES];
  size_t clone_count = 0;
  SourceLocation location = fn->instructions[root_index].location;
  int ok = 1;

  if (!rw_collect_needed(&rule->to, rule->to.root, needed, &needed_count)) {
    return 0;
  }
  unsigned long serial = ++g_serial;
  for (size_t i = 0; i < rule->to.fn->instruction_count && ok; i++) {
    const IRInstruction *src = &rule->to.fn->instructions[i];
    int wanted = 0;
    for (size_t n = 0; n < needed_count; n++) {
      if (needed[n] == src) {
        wanted = 1;
        break;
      }
    }
    if (!wanted) {
      continue;
    }
    char fresh[96];
    snprintf(fresh, sizeof(fresh), "rw%lu_%s", serial,
             src->dest.name ? src->dest.name : "v");
    names[name_count].source = src;
    names[name_count].fresh_dest = mettle_strdup(fresh);
    if (!names[name_count].fresh_dest) {
      ok = 0;
      break;
    }
    name_count++;
    memset(&clones[clone_count], 0, sizeof(IRInstruction));
    if (!rw_clone_instruction(&clones[clone_count], src, location)) {
      ok = 0;
      clone_count++;
      break;
    }
    clone_count++;
  }
  for (size_t c = 0; c < clone_count && ok; c++) {
    IRInstruction *clone = &clones[c];
    int slots = rw_operand_slot_count(clone);
    for (int s = 0; s < slots && ok; s++) {
      ok = rw_substitute(rw_operand_slot_mut(clone, s), rule, m, names,
                         name_count);
    }
    if (ok && clone->dest.kind == IR_OPERAND_TEMP && clone->dest.name) {
      const char *fresh = rw_fresh_for(names, name_count, clone->dest.name);
      IROperand renamed = fresh ? ir_operand_temp(fresh) : ir_operand_none();
      if (!renamed.name) {
        ok = 0;
      } else {
        ir_operand_destroy(&clone->dest);
        clone->dest = renamed;
      }
    }
  }

  IROperand result = ir_operand_none();
  const RwNode *to_root = &rule->to.nodes[rule->to.root];
  if (ok) {
    switch (to_root->kind) {
    case RW_NODE_VAR:
      result = ir_operand_copy(m->ops[to_root->var]);
      ok = !m->ops[to_root->var]->name || result.name != NULL;
      break;
    case RW_NODE_INT:
      result = ir_operand_int(to_root->int_value);
      break;
    case RW_NODE_FLOAT:
      result = ir_operand_float_sized(to_root->float_value,
                                      to_root->float_bits ? to_root->float_bits
                                                          : 64);
      break;
    case RW_NODE_OP: {
      const char *fresh =
          rw_fresh_for(names, name_count, to_root->insn->dest.name);
      result = ir_operand_temp(fresh ? fresh : "");
      ok = fresh && result.name;
      break;
    }
    }
  }

  if (ok) {
    size_t at = root_index;
    for (size_t c = 0; c < clone_count; c++) {
      if (!ir_instruction_insert_move(fn, at, &clones[c])) {
        ok = 0;
        break;
      }
      at++;
    }
    if (ok) {
      IRInstruction *root = &fn->instructions[at];
      ok = ir_rewrite_to_assign_operand(root, &result, changed);
      if (ok && to_root->kind == RW_NODE_OP && to_root->insn->is_float) {
        root->is_float = 1;
        root->float_bits = to_root->insn->float_bits;
      }
    }
    if (ok) {
      for (int p = m->producer_count; p > 0;) {
        p--;
        size_t index = m->producers[p];
        IRInstruction *producer = &fn->instructions[index];
        if (producer->op == IR_OP_NOP ||
            producer->dest.kind != IR_OPERAND_TEMP || !producer->dest.name) {
          continue;
        }
        if (rw_temp_has_reader(fn, producer->dest.name, index)) {
          continue;
        }
        ir_instruction_make_nop(producer);
      }
    }
  }

  ir_operand_destroy(&result);
  for (size_t c = 0; c < clone_count; c++) {
    ir_instruction_destroy_storage(&clones[c]);
  }
  for (size_t n = 0; n < name_count; n++) {
    free(names[n].fresh_dest);
  }
  return ok;
}

static int rw_validate_function(IRFunction *fn, IRVerifySnapshot *before,
                                const char *rules_applied, int *changed) {
  char why[192];
  char cex[288];
  char skip[160];
  if (!before) {
    return 1;
  }
  IRVerifyRewriteVerdict verdict = ir_verify_check_rewrite_probed(
      g_program, fn, before, NULL, NULL, RW_INT_PROBES,
      (int)(sizeof(RW_INT_PROBES) / sizeof(RW_INT_PROBES[0])), why,
      sizeof(why), cex, sizeof(cex), skip, sizeof(skip));
  if (verdict != IR_VERIFY_REWRITE_DIVERGED) {
    return 1;
  }
  rw_report_error(fn,
                  "applying rewrite %s inside `%s` changed its behavior; the "
                  "rewrite was undone",
                  rules_applied, fn->name ? fn->name : "?");
  fprintf(stderr, "  counterexample %s\n", cex);
  fprintf(stderr, "  divergence: %s\n", why);
  ir_verify_snapshot_restore(fn, before);
  *changed = 0;
  return 0;
}

int ir_user_rewrite_pass(IRFunction *function, int *changed) {
  if (!function || g_rule_count == 0 || g_failed || function->rewrite_role) {
    return 1;
  }
  IRVerifySnapshot *before = NULL;
  int applied_here = 0;
  int local_changed = 0;
  char applied_names[256] = "";
  size_t applied_len = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (applied_here >= RW_MAX_APPLICATIONS_PER_FUNCTION) {
      break;
    }
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op != IR_OP_BINARY && insn->op != IR_OP_UNARY &&
        insn->op != IR_OP_CAST && insn->op != IR_OP_CALL) {
      continue;
    }
    for (size_t r = 0; r < g_rule_count; r++) {
      RwRule *rule = &g_rules[r];
      RwMatch m;
      if (!rw_match_root(function, rule, i, &m)) {
        continue;
      }
      if (!rw_bindings_stable(function, &m, i)) {
        continue;
      }
      if (!rw_guard_admits(rule, &m)) {
        continue;
      }
      if (!before) {
        before = ir_verify_snapshot_capture(function);
      }
      SourceLocation location = function->instructions[i].location;
      if (!rw_apply(function, rule, i, &m, &local_changed)) {
        ir_verify_snapshot_free(before);
        return 0;
      }
      rule->applications++;
      applied_here++;
      if (applied_len < sizeof(applied_names) - 8 &&
          !strstr(applied_names, rule->name)) {
        applied_len += (size_t)snprintf(
            applied_names + applied_len, sizeof(applied_names) - applied_len,
            "%s`%s`", applied_len ? ", " : "", rule->name);
      }
      if (ir_explain_enabled()) {
        char headline[160];
        snprintf(headline, sizeof(headline), "rewritten by rule `%s`",
                 rule->name);
        ir_explain_remark(function->name, "expression", location, 1, headline,
                          NULL, NULL, NULL);
        ir_explain_remark_code("rewrite-applied");
      }
      break;
    }
  }

  if (local_changed) {
    *changed = 1;
    if (!rw_validate_function(function, before, applied_names, changed)) {
      ir_verify_snapshot_free(before);
      return 1;
    }
  }
  ir_verify_snapshot_free(before);
  return 1;
}

int ir_user_rewrite_end(IRProgram *program) {
  (void)program;
  if (ir_explain_enabled()) {
    for (size_t r = 0; r < g_rule_count; r++) {
      const RwRule *rule = &g_rules[r];
      if (rule->applications > 0) {
        char headline[128];
        snprintf(headline, sizeof(headline), "applied %ld time%s",
                 rule->applications, rule->applications == 1 ? "" : "s");
        ir_explain_remark(rule->name, "rewrite rule", rule->location, 1,
                          headline, NULL, NULL, NULL);
        ir_explain_remark_code("rewrite-rule-applied");
      } else if (rule->guard_undecided > 0) {
        char headline[160];
        char reason[256];
        snprintf(headline, sizeof(headline),
                 "matched %ld expression%s, none rewritten: `where` could not "
                 "be decided",
                 rule->guard_undecided, rule->guard_undecided == 1 ? "" : "s");
        snprintf(reason, sizeof(reason),
                 "`where` reads `%s`, and at every match `%s` was a runtime "
                 "value; a guard is evaluated at compile time, so it applies "
                 "only where the parameters it reads are constants",
                 rule->undecided_param ? rule->undecided_param : "?",
                 rule->undecided_param ? rule->undecided_param : "?");
        ir_explain_remark(rule->name, "rewrite rule", rule->location, 0,
                          headline, reason,
                          "drop the guard if the rule holds for every value, "
                          "or keep it and accept that only constant arguments "
                          "qualify",
                          NULL);
        ir_explain_remark_code("rewrite-rule-guard-undecided");
      } else if (rule->guard_false > 0) {
        char headline[160];
        snprintf(headline, sizeof(headline),
                 "matched %ld expression%s, none rewritten: `where` was false "
                 "at each",
                 rule->guard_false, rule->guard_false == 1 ? "" : "s");
        ir_explain_remark(rule->name, "rewrite rule", rule->location, 0,
                          headline, NULL, NULL, NULL);
        ir_explain_remark_code("rewrite-rule-guard-false");
        ir_explain_remark_advisory();
      } else {
        ir_explain_remark(
            rule->name, "rewrite rule", rule->location, 0,
            "matched nowhere in this program",
            "no expression had the shape of its `from` side, before or after "
            "inlining, at any point the compiler looked",
            "the rule costs nothing; check its operand types against the code "
            "it was written for",
            NULL);
        ir_explain_remark_code("rewrite-rule-unused");
        ir_explain_remark_advisory();
      }
    }
  }
  int ok = !g_failed;
  g_rule_count = 0;
  g_program = NULL;
  g_failed = 0;
  return ok;
}
