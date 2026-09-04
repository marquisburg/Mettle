#include "ir_deadline.h"
#include "ir_pgo.h"
#include "../common.h"
#include <stdlib.h>
#include <string.h>

#define DEADLINE_UNBOUNDED (-1)
#define DEADLINE_MAX_BLOCKS 4096

IRFunction *ir_program_find_function(IRProgram *program, const char *name);

typedef struct {
  IRProgram *program;
  ErrorReporter *reporter;
  const IRDeadlineCosts *costs;
  long long *cost;
  int *state;
  int *evidence;
  size_t count;
  int errors;
  FILE *report;
  const IRFunction *report_for;
} Ctx;

static long long op_cost(const IRDeadlineCosts *costs,
                        const IRInstruction *insn) {
  switch (insn->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_DECLARE_LOCAL:
    return 0;
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_RETURN:
    return costs->branch;
  case IR_OP_LOAD:
    return costs->load;
  case IR_OP_STORE:
    return costs->store;
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_CAST:
  case IR_OP_UNARY:
    return costs->op;
  case IR_OP_ROTATE_ADD:
    return costs->op * 2;
  case IR_OP_BINARY:
    if (!insn->text) {
      return costs->op;
    }
    if (strcmp(insn->text, "*") == 0) {
      return insn->is_float ? costs->multiply_float : costs->multiply;
    }
    if (strcmp(insn->text, "/") == 0) {
      return insn->is_float ? costs->divide_float : costs->divide;
    }
    if (strcmp(insn->text, "%") == 0) {
      return costs->divide;
    }
    return costs->op;
  case IR_OP_NEW:
    return costs->allocate;
  case IR_OP_INLINE_ASM:
    return DEADLINE_UNBOUNDED;
  case IR_OP_CALL_INDIRECT:
  case IR_OP_GPU_LAUNCH:
    return DEADLINE_UNBOUNDED;
  default:
    return costs->op * 2;
  }
}

static long long function_cost(Ctx *ctx, IRFunction *fn);

static long long call_cost(Ctx *ctx, const IRInstruction *insn) {
  IRFunction *callee = NULL;
  if (!insn->text) {
    return DEADLINE_UNBOUNDED;
  }
  callee = ir_program_find_function(ctx->program, insn->text);
  if (!callee || !callee->instruction_count) {
    return DEADLINE_UNBOUNDED;
  }
  {
    long long inner = function_cost(ctx, callee);
    if (inner == DEADLINE_UNBOUNDED) {
      return DEADLINE_UNBOUNDED;
    }
    return inner + ctx->costs->call;
  }
}

static int reaches(const IRBasicBlock *blocks, size_t count, size_t from,
                   size_t to, const int *back, char *seen) {
  size_t stack[DEADLINE_MAX_BLOCKS];
  size_t depth = 0;
  memset(seen, 0, count);
  stack[depth++] = from;
  seen[from] = 1;
  while (depth) {
    size_t at = stack[--depth];
    if (at == to) {
      return 1;
    }
    for (size_t e = 0; e < blocks[at].successor_count; e++) {
      size_t next = blocks[at].successors[e];
      if (next >= count || back[at * count + next] || seen[next]) {
        continue;
      }
      seen[next] = 1;
      if (depth < DEADLINE_MAX_BLOCKS) {
        stack[depth++] = next;
      }
    }
  }
  return 0;
}

static long long longest_path(const IRBasicBlock *blocks, size_t count,
                              const long long *weight, const int *back,
                              const char *member, size_t start, size_t stop,
                              size_t *prev, size_t *end_out) {
  long long *best = calloc(count, sizeof(long long));
  size_t *indegree = calloc(count, sizeof(size_t));
  size_t *queue = calloc(count, sizeof(size_t));
  size_t head = 0;
  size_t tail = 0;
  long long answer = DEADLINE_UNBOUNDED;
  if (!best || !indegree || !queue) {
    free(best);
    free(indegree);
    free(queue);
    return DEADLINE_UNBOUNDED;
  }
  for (size_t i = 0; i < count; i++) {
    best[i] = DEADLINE_UNBOUNDED;
    if (prev) {
      prev[i] = (size_t)-1;
    }
    if (member && !member[i]) {
      continue;
    }
    for (size_t e = 0; e < blocks[i].successor_count; e++) {
      size_t next = blocks[i].successors[e];
      if (next >= count || back[i * count + next]) {
        continue;
      }
      if (member && !member[next]) {
        continue;
      }
      indegree[next]++;
    }
  }
  for (size_t i = 0; i < count; i++) {
    if (member && !member[i]) {
      continue;
    }
    if (indegree[i] == 0) {
      queue[tail++] = i;
    }
  }
  best[start] = weight[start];
  while (head < tail) {
    size_t at = queue[head++];
    for (size_t e = 0; e < blocks[at].successor_count; e++) {
      size_t next = blocks[at].successors[e];
      if (next >= count || back[at * count + next]) {
        continue;
      }
      if (member && !member[next]) {
        continue;
      }
      if (best[at] != DEADLINE_UNBOUNDED &&
          (best[next] == DEADLINE_UNBOUNDED ||
           best[at] + weight[next] > best[next])) {
        best[next] = best[at] + weight[next];
        if (prev) {
          prev[next] = at;
        }
      }
      if (indegree[next] > 0 && --indegree[next] == 0 && tail < count) {
        queue[tail++] = next;
      }
    }
  }
  if (stop != (size_t)-1) {
    answer = best[stop];
    if (end_out) {
      *end_out = stop;
    }
  } else {
    for (size_t i = 0; i < count; i++) {
      if (member && !member[i]) {
        continue;
      }
      if (best[i] != DEADLINE_UNBOUNDED &&
          (answer == DEADLINE_UNBOUNDED || best[i] > answer)) {
        answer = best[i];
        if (end_out) {
          *end_out = i;
        }
      }
    }
  }
  free(best);
  free(indegree);
  free(queue);
  return answer;
}

static void mark_back_edges(const IRBasicBlock *blocks, size_t count,
                            int *back) {
  char *on_stack = calloc(count, 1);
  char *done = calloc(count, 1);
  size_t *stack = calloc(count + 1, sizeof(size_t));
  size_t *edge = calloc(count + 1, sizeof(size_t));
  size_t depth = 0;
  if (!on_stack || !done || !stack || !edge || count == 0) {
    free(on_stack);
    free(done);
    free(stack);
    free(edge);
    return;
  }
  stack[depth] = 0;
  edge[depth] = 0;
  on_stack[0] = 1;
  while (1) {
    size_t at = stack[depth];
    if (edge[depth] < blocks[at].successor_count) {
      size_t next = blocks[at].successors[edge[depth]++];
      if (next >= count) {
        continue;
      }
      if (on_stack[next]) {
        back[at * count + next] = 1;
        continue;
      }
      if (done[next]) {
        continue;
      }
      depth++;
      stack[depth] = next;
      edge[depth] = 0;
      on_stack[next] = 1;
      continue;
    }
    on_stack[at] = 0;
    done[at] = 1;
    if (depth == 0) {
      break;
    }
    depth--;
  }
  free(on_stack);
  free(done);
  free(stack);
  free(edge);
}

static int operand_is(const IROperand *operand, const char *name) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         name && strcmp(operand->name, name) == 0;
}

static const IRInstruction *defines_temp(const IRBasicBlock *block,
                                         const IROperand *temp) {
  if (!temp || temp->kind != IR_OPERAND_TEMP || !temp->name) {
    return NULL;
  }
  for (size_t i = block->instruction_count; i-- > 0;) {
    const IRInstruction *insn = &block->instructions[i];
    if (insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name &&
        strcmp(insn->dest.name, temp->name) == 0) {
      return insn;
    }
  }
  return NULL;
}

static long long iv_step(const IRBasicBlock *blocks, size_t count,
                         const char *member, const char *iv, int *found) {
  long long step = 0;
  *found = 0;
  for (size_t b = 0; b < count; b++) {
    if (!member[b]) {
      continue;
    }
    for (size_t i = 0; i < blocks[b].instruction_count; i++) {
      const IRInstruction *insn = &blocks[b].instructions[i];
      const IRInstruction *source = NULL;
      if (!operand_is(&insn->dest, iv)) {
        continue;
      }
      source = insn->op == IR_OP_ASSIGN ? defines_temp(&blocks[b], &insn->lhs)
                                        : insn;
      while (source && source->op == IR_OP_CAST) {
        source = defines_temp(&blocks[b], &source->lhs);
      }
      if (!source || source->op != IR_OP_BINARY || !source->text ||
          source->is_float) {
        *found = 0;
        return 0;
      }
      if (strcmp(source->text, "+") == 0 && operand_is(&source->lhs, iv) &&
          source->rhs.kind == IR_OPERAND_INT) {
        step = source->rhs.int_value;
      } else if (strcmp(source->text, "-") == 0 &&
                 operand_is(&source->lhs, iv) &&
                 source->rhs.kind == IR_OPERAND_INT) {
        step = -source->rhs.int_value;
      } else {
        *found = 0;
        return 0;
      }
      if (*found) {
        *found = 0;
        return 0;
      }
      *found = 1;
    }
  }
  return step;
}

static int iv_entry(const IRFunction *fn, const IRBasicBlock *blocks,
                    size_t count, const char *member, size_t header,
                    const char *iv, long long *out) {
  size_t limit = blocks[header].first_instruction;
  int seen = 0;
  for (size_t i = 0; i < limit && i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (!operand_is(&insn->dest, iv)) {
      continue;
    }
    if (insn->op == IR_OP_ASSIGN && insn->lhs.kind == IR_OPERAND_INT) {
      *out = insn->lhs.int_value;
      seen = 1;
      continue;
    }
    seen = 0;
  }
  (void)member;
  (void)blocks;
  (void)count;
  return seen;
}

/* The constant a symbol holds where the loop starts. A limit written as a
 * literal is the easy case; a limit held in a binding the program set once
 * before the loop and never touches inside it is the same fact spelled
 * differently, and refusing that one would make a deadline something only
 * loops with a magic number in them could carry. */
static int symbol_constant_before(const IRFunction *fn,
                                  const IRBasicBlock *blocks, size_t count,
                                  const char *member, size_t header,
                                  const char *name, long long *out) {
  size_t limit = blocks[header].first_instruction;
  int seen = 0;
  for (size_t b = 0; b < count; b++) {
    if (!member[b]) {
      continue;
    }
    for (size_t i = 0; i < blocks[b].instruction_count; i++) {
      if (operand_is(&blocks[b].instructions[i].dest, name)) {
        return 0;
      }
    }
  }
  for (size_t i = 0; i < limit && i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (!operand_is(&insn->dest, name)) {
      continue;
    }
    if (insn->op == IR_OP_ASSIGN && insn->lhs.kind == IR_OPERAND_INT) {
      *out = insn->lhs.int_value;
      seen = 1;
      continue;
    }
    seen = 0;
  }
  return seen;
}

static long long loop_trip_count(const IRFunction *fn,
                                 const IRBasicBlock *blocks, size_t count,
                                 const char *member, size_t header) {
  const IRBasicBlock *block = &blocks[header];
  const IRInstruction *branch = NULL;
  const IRInstruction *test = NULL;
  long long limit = 0;
  long long entry = 0;
  long long step = 0;
  long long span = 0;
  int found = 0;
  const char *iv = NULL;
  if (block->instruction_count == 0) {
    return DEADLINE_UNBOUNDED;
  }
  branch = &block->instructions[block->instruction_count - 1];
  if (branch->op != IR_OP_BRANCH_ZERO) {
    return DEADLINE_UNBOUNDED;
  }
  test = defines_temp(block, &branch->lhs);
  if (!test || test->op != IR_OP_BINARY || !test->text || test->is_float) {
    return DEADLINE_UNBOUNDED;
  }
  if (test->lhs.kind != IR_OPERAND_SYMBOL || !test->lhs.name) {
    return DEADLINE_UNBOUNDED;
  }
  if (test->rhs.kind == IR_OPERAND_INT) {
    limit = test->rhs.int_value;
  } else if (test->rhs.kind != IR_OPERAND_SYMBOL || !test->rhs.name ||
             !symbol_constant_before(fn, blocks, count, member, header,
                                     test->rhs.name, &limit)) {
    return DEADLINE_UNBOUNDED;
  }
  iv = test->lhs.name;
  if (strcmp(test->text, "<") == 0) {
    /* the bound is exclusive as written */
  } else if (strcmp(test->text, "<=") == 0) {
    limit += 1;
  } else if (strcmp(test->text, ">") == 0) {
    limit -= 1;
  } else if (strcmp(test->text, ">=") != 0) {
    return DEADLINE_UNBOUNDED;
  }
  step = iv_step(blocks, count, member, iv, &found);
  if (!found || step == 0) {
    return DEADLINE_UNBOUNDED;
  }
  if (!iv_entry(fn, blocks, count, member, header, iv, &entry)) {
    return DEADLINE_UNBOUNDED;
  }
  span = step > 0 ? limit - entry : entry - limit;
  if (span <= 0) {
    return 1;
  }
  {
    long long magnitude = step > 0 ? step : -step;
    return (span + magnitude - 1) / magnitude + 1;
  }
}

static long long trips_for(Ctx *ctx, IRFunction *fn,
                           const IRBasicBlock *blocks, size_t count,
                           const char *member, size_t header,
                           SourceLocation location, int *from_evidence) {
  long long bound = loop_trip_count(fn, blocks, count, member, header);
  (void)ctx;
  if (bound > 0) {
    return bound;
  }
  if (ir_pgo_enabled()) {
    long long measured = ir_pgo_site_count(fn->name, location);
    if (measured > 0) {
      *from_evidence = 1;
      return measured;
    }
  }
  return DEADLINE_UNBOUNDED;
}

static long long compute_cost(Ctx *ctx, IRFunction *fn, int *evidence,
                              FILE *report) {
  size_t count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(fn, &count);
  long long *weight = NULL;
  int *back = NULL;
  char *member = NULL;
  char *seen = NULL;
  long long total = DEADLINE_UNBOUNDED;
  if (!blocks || count == 0 || count > DEADLINE_MAX_BLOCKS) {
    return DEADLINE_UNBOUNDED;
  }
  weight = calloc(count, sizeof(long long));
  back = calloc(count * count, sizeof(int));
  member = calloc(count, 1);
  seen = calloc(count, 1);
  if (!weight || !back || !member || !seen) {
    free(weight);
    free(back);
    free(member);
    free(seen);
    return DEADLINE_UNBOUNDED;
  }
  for (size_t b = 0; b < count; b++) {
    for (size_t i = 0; i < blocks[b].instruction_count; i++) {
      const IRInstruction *insn = &blocks[b].instructions[i];
      long long one = insn->op == IR_OP_CALL ? call_cost(ctx, insn)
                                             : op_cost(ctx->costs, insn);
      if (one == DEADLINE_UNBOUNDED) {
        free(weight);
        free(back);
        free(member);
        free(seen);
        return DEADLINE_UNBOUNDED;
      }
      weight[b] += one;
    }
  }
  mark_back_edges(blocks, count, back);
  for (size_t rounds = 0; rounds < count; rounds++) {
    size_t best_source = (size_t)-1;
    size_t best_header = (size_t)-1;
    size_t best_size = (size_t)-1;
    for (size_t u = 0; u < count; u++) {
      for (size_t h = 0; h < count; h++) {
        size_t size = 0;
        if (back[u * count + h] != 1) {
          continue;
        }
        for (size_t b = 0; b < count; b++) {
          if (reaches(blocks, count, h, b, back, seen) &&
              reaches(blocks, count, b, u, back, seen)) {
            size++;
          }
        }
        if (best_size == (size_t)-1 || size < best_size) {
          best_size = size;
          best_source = u;
          best_header = h;
        }
      }
    }
    if (best_source == (size_t)-1) {
      break;
    }
    {
      long long body = 0;
      long long trips = 0;
      int measured = 0;
      SourceLocation header = blocks[best_header].instruction_count
                                  ? blocks[best_header].instructions[0].location
                                  : fn->location;
      memset(member, 0, count);
      for (size_t b = 0; b < count; b++) {
        if (reaches(blocks, count, best_header, b, back, seen) &&
            reaches(blocks, count, b, best_source, back, seen)) {
          member[b] = 1;
        }
      }
      body = longest_path(blocks, count, weight, back, member, best_header,
                          best_source, NULL, NULL);
      trips = trips_for(ctx, fn, blocks, count, member, best_header,
                        header, &measured);
      if (body == DEADLINE_UNBOUNDED || trips == DEADLINE_UNBOUNDED ||
          (body > 0 && trips > (long long)1 << 40)) {
        free(weight);
        free(back);
        free(member);
        free(seen);
        return DEADLINE_UNBOUNDED;
      }
      if (measured) {
        *evidence = 1;
      }
      if (getenv("METTLE_TRUST_DEADLINES")) {
        trips = 1;
      }
      weight[best_header] += body * (trips - 1);
      back[best_source * count + best_header] = 2;
    }
  }
  {
    size_t *prev = calloc(count, sizeof(size_t));
    size_t last = 0;
    total = longest_path(blocks, count, weight, back, NULL, 0, (size_t)-1,
                         prev, &last);
    if (report && prev && total != DEADLINE_UNBOUNDED) {
      size_t chain[DEADLINE_MAX_BLOCKS];
      size_t depth = 0;
      size_t at = last;
      while (at != (size_t)-1 && depth < DEADLINE_MAX_BLOCKS) {
        chain[depth++] = at;
        if (at == 0) {
          break;
        }
        at = prev[at];
      }
      fprintf(report, "  longest path through %s:\n",
              fn->name ? fn->name : "?");
      while (depth-- > 0) {
        const IRBasicBlock *block = &blocks[chain[depth]];
        size_t line = block->instruction_count
                          ? block->instructions[0].location.line
                          : fn->location.line;
        fprintf(report, "    %s at line %zu costs %lld\n",
                block->label ? block->label : "<anon>", line,
                weight[chain[depth]]);
      }
    }
    free(prev);
  }
  free(weight);
  free(back);
  free(member);
  free(seen);
  return total;
}

static long long function_cost(Ctx *ctx, IRFunction *fn) {
  size_t index = 0;
  int evidence = 0;
  for (index = 0; index < ctx->count; index++) {
    if (ctx->program->functions[index] == fn) {
      break;
    }
  }
  if (index == ctx->count) {
    return DEADLINE_UNBOUNDED;
  }
  if (ctx->state[index] == 1) {
    return DEADLINE_UNBOUNDED;
  }
  if (ctx->state[index] == 2) {
    return ctx->cost[index];
  }
  ctx->state[index] = 1;
  ctx->cost[index] = compute_cost(ctx, fn, &evidence, ctx->report_for == fn
                                                          ? ctx->report
                                                          : NULL);
  ctx->evidence[index] = evidence;
  ctx->state[index] = 2;
  return ctx->cost[index];
}

static void register_helper(IRProgram *program, const char *name,
                            size_t params) {
  IRModuleSymbol entry;
  MtlcType *types[3];
  MtlcType *cstring = ir_program_lookup_type(program, "cstring");
  MtlcType *word = ir_program_lookup_type(program, "int64");
  if (ir_program_lookup_symbol(program, name)) {
    return;
  }
  memset(&entry, 0, sizeof(entry));
  entry.name = (char *)name;
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.has_body = 0;
  entry.return_type = ir_program_lookup_type(program, "void");
  if (cstring && word && params > 0) {
    types[0] = params == 1 ? word : cstring;
    types[1] = word;
    types[2] = word;
    entry.param_types = types;
    entry.param_count = params;
  }
  ir_program_add_symbol(program, &entry);
}

static int emit_call(IRFunction *fn, size_t at, const char *helper,
                     const IROperand *arguments, size_t count,
                     SourceLocation location) {
  IRInstruction call = {0};
  int ok = 0;
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = (char *)helper;
  call.argument_count = count;
  call.arguments = calloc(count ? count : 1, sizeof(IROperand));
  if (!call.arguments) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    call.arguments[i] = arguments[i];
  }
  ok = ir_function_insert_instruction(fn, at, &call);
  free(call.arguments);
  return ok;
}

static int instrument_function(Ctx *ctx, IRFunction *fn, long long proven) {
  size_t at = 0;
  IROperand entry[3];
  while (at < fn->instruction_count &&
         (fn->instructions[at].op == IR_OP_LABEL ||
          fn->instructions[at].op == IR_OP_DECLARE_LOCAL)) {
    at++;
  }
  entry[0] = ir_operand_string(fn->name ? fn->name : "?");
  entry[1] = ir_operand_int(fn->deadline_cycles);
  entry[2] = ir_operand_int(proven);
  if (!emit_call(fn, at, "mettle_safety_deadline_enter", entry, 3,
                 fn->location)) {
    ir_operand_destroy(&entry[0]);
    return 0;
  }
  ir_operand_destroy(&entry[0]);
  for (size_t i = fn->instruction_count; i-- > 0;) {
    if (fn->instructions[i].op != IR_OP_RETURN) {
      continue;
    }
    if (!emit_call(fn, i, "mettle_safety_deadline_leave", NULL, 0,
                   fn->instructions[i].location)) {
      return 0;
    }
  }
  {
    size_t count = 0;
    const IRBasicBlock *blocks = ir_function_blocks(fn, &count);
    long long *weight = NULL;
    if (!blocks || count == 0) {
      return 1;
    }
    weight = calloc(count, sizeof(long long));
    if (!weight) {
      return 0;
    }
    for (size_t b = 0; b < count; b++) {
      for (size_t i = 0; i < blocks[b].instruction_count; i++) {
        long long one = op_cost(ctx->costs, &blocks[b].instructions[i]);
        weight[b] += one == DEADLINE_UNBOUNDED ? 0 : one;
      }
    }
    for (size_t b = count; b-- > 0;) {
      IROperand argument = ir_operand_int(weight[b]);
      size_t site = blocks[b].first_instruction;
      if (weight[b] <= 0 || site > fn->instruction_count) {
        continue;
      }
      if (fn->instructions[site].op == IR_OP_LABEL) {
        site++;
      }
      if (!emit_call(fn, site, "mettle_safety_deadline_step", &argument, 1,
                     fn->instructions[site < fn->instruction_count
                                          ? site
                                          : fn->instruction_count - 1]
                         .location)) {
        free(weight);
        return 0;
      }
    }
    free(weight);
  }
  return 1;
}

int ir_deadline_run(IRProgram *program, ErrorReporter *reporter,
                    const IRDeadlineCosts *costs, int instrument, FILE *report,
                    IRDeadlineStats *stats) {
  Ctx ctx;
  IRDeadlineStats local;
  int ok = 1;
  if (!program || !costs) {
    return 1;
  }
  memset(&ctx, 0, sizeof(ctx));
  memset(&local, 0, sizeof(local));
  if (!stats) {
    stats = &local;
  }
  memset(stats, 0, sizeof(*stats));
  ctx.program = program;
  ctx.reporter = reporter;
  ctx.costs = costs;
  ctx.count = program->function_count;
  ctx.cost = calloc(ctx.count ? ctx.count : 1, sizeof(long long));
  ctx.state = calloc(ctx.count ? ctx.count : 1, sizeof(int));
  ctx.evidence = calloc(ctx.count ? ctx.count : 1, sizeof(int));
  if (!ctx.cost || !ctx.state || !ctx.evidence) {
    free(ctx.cost);
    free(ctx.state);
    free(ctx.evidence);
    return 0;
  }
  for (size_t i = 0; i < ctx.count; i++) {
    IRFunction *fn = program->functions[i];
    long long proven = 0;
    long long limit = 0;
    if (!fn || !fn->has_deadline) {
      continue;
    }
    stats->declared++;
    ctx.report = report;
    ctx.report_for = fn;
    proven = function_cost(&ctx, fn);
    ctx.report_for = NULL;
    limit = fn->deadline_inclusive ? fn->deadline_cycles
                                   : fn->deadline_cycles - 1;
    if (proven == DEADLINE_UNBOUNDED) {
      char message[512];
      snprintf(message, sizeof(message),
               "'%s' declares a deadline of %lld cycles, and its longest path "
               "cannot be bounded: a loop with no trip count the compiler can "
               "prove, a call it cannot see into, or an asm block",
               fn->name ? fn->name : "?", fn->deadline_cycles);
      if (reporter) {
        error_reporter_add_error_with_suggestion(
            reporter, ERROR_SEMANTIC, fn->location.line ? fn->location
                                                        : fn->location,
            message,
            "bound the loop, or build with --pgo so a measured trip count "
            "stands in and the deadline is reported as evidence");
        error_reporter_set_last_code(reporter, "D0002");
      } else {
        fprintf(stderr, "error[D0002]: %s\n", message);
      }
      ctx.errors++;
      ok = 0;
      continue;
    }
    if (proven > limit) {
      char message[512];
      snprintf(message, sizeof(message),
               "'%s' declares a deadline of %lld cycles, and its longest path "
               "costs %lld on this target",
               fn->name ? fn->name : "?", fn->deadline_cycles, proven);
      if (reporter) {
        error_reporter_add_error_with_suggestion(
            reporter, ERROR_SEMANTIC, fn->location, message,
            "the deadline is a contract: raise it to what the path costs, or "
            "take work off the path");
        error_reporter_set_last_code(reporter, "D0001");
      } else {
        fprintf(stderr, "error[D0001]: %s\n", message);
      }
      ctx.errors++;
      ok = 0;
      continue;
    }
    stats->proven++;
    if (ctx.evidence[i]) {
      stats->on_evidence++;
    }
    if (stats->worst_function == NULL || limit - proven < stats->worst_slack) {
      stats->worst_slack = limit - proven;
      stats->worst_function = fn->name;
    }
    if (report) {
      fprintf(report, "deadline %s: %lld of %lld cycles, %lld to spare, %s\n",
              fn->name ? fn->name : "?", proven, fn->deadline_cycles,
              limit - proven,
              ctx.evidence[i] ? "held on a measured trip count"
                              : "proven from the cost model");
    }
  }
  if (report && stats->declared == 0) {
    fprintf(report, "deadlines: none declared\n");
  } else if (report) {
    fprintf(report,
            "cost model: op %lld, load %lld, store %lld, branch %lld, "
            "multiply %lld/%lld, divide %lld/%lld, call %lld, allocate %lld "
            "(%s)\n",
            costs->op, costs->load, costs->store, costs->branch,
            costs->multiply, costs->multiply_float, costs->divide,
            costs->divide_float, costs->call, costs->allocate,
            costs->described ? "from the target description"
                             : "the target's own");
    fprintf(report,
            "deadlines: %zu declared, %zu proven, %zu held on evidence\n",
            stats->declared, stats->proven, stats->on_evidence);
  }
  if (ok && instrument && stats->declared > 0) {
    register_helper(program, "mettle_safety_deadline_enter", 3);
    register_helper(program, "mettle_safety_deadline_leave", 0);
    register_helper(program, "mettle_safety_deadline_step", 1);
    for (size_t i = 0; i < ctx.count; i++) {
      IRFunction *fn = program->functions[i];
      if (!fn || !fn->has_deadline) {
        continue;
      }
      if (!instrument_function(&ctx, fn, ctx.cost[i])) {
        ok = 0;
        break;
      }
    }
  }
  free(ctx.cost);
  free(ctx.state);
  free(ctx.evidence);
  return ok;
}
