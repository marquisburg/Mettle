#include "ir_rules.h"
#include "ir_explain_ledger.h"
#include "ir_interp.h"
#include <stdlib.h>
#include <string.h>

#define IR_RULE_DEFAULT_FUEL 50000000LL

void ir_rule_image_free(IRRuleImage *image) {
  if (!image) {
    return;
  }
  free(image->bytes);
  free(image->pointer_offsets);
  free(image->sites);
  memset(image, 0, sizeof(*image));
}

/* Which image a rule asks for. A rule takes exactly one argument and its type
 * is the question: `Program` is the checked program, `Machine` is what became
 * of it once it was code, `Trace` is what happened when it ran. The parameter
 * type is the whole dispatch; there is no second keyword and no decorator to
 * write. */
IRRuleKind ir_rule_kind(const IRFunction *rule) {
  const char *type;
  if (!rule || !rule->is_rule || rule->parameter_count != 1 ||
      !rule->parameter_types || !rule->parameter_types[0]) {
    return IR_RULE_OVER_PROGRAM;
  }
  type = rule->parameter_types[0];
  {
    const char *base = strrchr(type, '.');
    if (base) {
      type = base + 1;
    }
  }
  if (strcmp(type, "Machine") == 0) {
    return IR_RULE_OVER_MACHINE;
  }
  if (strcmp(type, "Trace") == 0) {
    return IR_RULE_OVER_TRACE;
  }
  return IR_RULE_OVER_PROGRAM;
}

int ir_program_has_rules_of(const IRProgram *program, IRRuleKind kind) {
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *fn = program->functions[i];
    if (fn && fn->is_rule && ir_rule_kind(fn) == kind) {
      return 1;
    }
  }
  return 0;
}

int ir_program_has_rules(const IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->is_rule) {
      return 1;
    }
  }
  return 0;
}

static const char *rule_display_name(const char *name) {
  const char *p = name ? name : "?";
  while (strncmp(p, "__import_", 9) == 0) {
    const char *rest = p + 9;
    while (*rest && *rest != '_') {
      rest++;
    }
    if (*rest != '_') {
      break;
    }
    p = rest + 1;
  }
  return p;
}

static int type_has_field(const MtlcType *type, const char *name) {
  if (!type || type->kind != MTLC_TYPE_STRUCT || !type->field_names) {
    return 0;
  }
  for (size_t i = 0; i < type->field_count; i++) {
    if (type->field_names[i] && strcmp(type->field_names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static size_t type_field_offset(const MtlcType *type, const char *name) {
  for (size_t i = 0; i < type->field_count; i++) {
    if (type->field_names[i] && strcmp(type->field_names[i], name) == 0) {
      return type->field_offsets[i];
    }
  }
  return 0;
}

static const MtlcType *type_field_type(const MtlcType *type,
                                       const char *name) {
  for (size_t i = 0; i < type->field_count; i++) {
    if (type->field_names[i] && strcmp(type->field_names[i], name) == 0) {
      return type->field_types[i];
    }
  }
  return NULL;
}

static const MtlcType *find_verdict_type(IRProgram *program,
                                         const IRFunction *rule) {
  const MtlcType *type =
      rule->return_type_name
          ? ir_program_lookup_type(program, rule->return_type_name)
          : NULL;
  if (type_has_field(type, "outcome") && type_has_field(type, "site") &&
      type_has_field(type, "message")) {
    return type;
  }
  for (size_t i = 0; i < program->type_registry_count; i++) {
    const MtlcType *candidate = program->type_registry[i].type;
    if (type_has_field(candidate, "outcome") &&
        type_has_field(candidate, "site") &&
        type_has_field(candidate, "message")) {
      return candidate;
    }
  }
  return NULL;
}

typedef struct {
  long long outcome;
  char file[512];
  long long line;
  long long column;
  char message[1024];
  char fix[1024];
  int readable;
} RuleVerdict;

static int read_u64(IRInterpMachine *machine, unsigned long long address,
                    unsigned long long *out) {
  unsigned char bytes[8];
  if (ir_interp_read_bytes(machine, address, bytes, 8) != 8) {
    return 0;
  }
  memcpy(out, bytes, 8);
  return 1;
}

static int read_string(IRInterpMachine *machine, unsigned long long address,
                       char *out, size_t capacity) {
  unsigned long long chars = 0;
  unsigned long long length = 0;
  out[0] = '\0';
  if (!read_u64(machine, address, &chars) ||
      !read_u64(machine, address + 8, &length)) {
    return 0;
  }
  if (length == 0) {
    return 1;
  }
  if (length >= capacity) {
    length = capacity - 1;
  }
  if (ir_interp_read_bytes(machine, chars, (unsigned char *)out,
                           (size_t)length) != (long long)length) {
    out[0] = '\0';
    return 0;
  }
  out[length] = '\0';
  return 1;
}

static void decode_verdict(IRInterpMachine *machine, const MtlcType *type,
                           unsigned long long address, RuleVerdict *verdict) {
  memset(verdict, 0, sizeof(*verdict));
  const MtlcType *site_type = type_field_type(type, "site");
  unsigned long long outcome = 0;
  if (!read_u64(machine, address + type_field_offset(type, "outcome"),
                &outcome)) {
    return;
  }
  verdict->outcome = (long long)outcome;
  if (!site_type || site_type->kind != MTLC_TYPE_STRUCT) {
    return;
  }
  unsigned long long site = address + type_field_offset(type, "site");
  unsigned long long line = 0;
  unsigned long long column = 0;
  if (!read_string(machine, site + type_field_offset(site_type, "file"),
                   verdict->file, sizeof(verdict->file)) ||
      !read_u64(machine, site + type_field_offset(site_type, "line"), &line) ||
      !read_u64(machine, site + type_field_offset(site_type, "column"),
                &column)) {
    return;
  }
  verdict->line = (long long)line;
  verdict->column = (long long)column;
  if (!read_string(machine, address + type_field_offset(type, "message"),
                   verdict->message, sizeof(verdict->message))) {
    return;
  }
  if (type_has_field(type, "fix")) {
    read_string(machine, address + type_field_offset(type, "fix"),
                verdict->fix, sizeof(verdict->fix));
  }
  verdict->readable = 1;
}

static const IRRuleSite *match_site(const IRRuleImage *image,
                                    const RuleVerdict *verdict) {
  for (size_t i = 0; i < image->site_count; i++) {
    const IRRuleSite *site = &image->sites[i];
    if ((long long)site->line == verdict->line &&
        (long long)site->column == verdict->column && site->file &&
        strcmp(site->file, verdict->file) == 0) {
      return site;
    }
  }
  return NULL;
}

/* What a proposing rule asked for, kept until the build is done. Applying it
 * while the compiler is still reading the file would rewrite the ground under
 * the diagnostic that is printing. Nothing is written unless --fix asked. */
typedef struct {
  char file[512];
  size_t line;
  char replacement[1024];
  char rule[128];
} IRRuleProposal;

static IRRuleProposal *g_proposals;
static size_t g_proposal_count;
static size_t g_proposal_capacity;
static int g_apply_proposals;

void ir_rules_set_apply_fixes(int on) { g_apply_proposals = on; }

size_t ir_rules_proposal_count(void) { return g_proposal_count; }

static int ir_rules_propose(const char *file, size_t line,
                            const char *replacement, const char *rule) {
  IRRuleProposal *entry;
  if (!file || !replacement || line == 0) {
    return 1;
  }
  for (size_t i = 0; i < g_proposal_count; i++) {
    if (g_proposals[i].line == line &&
        strcmp(g_proposals[i].file, file) == 0) {
      return 1;
    }
  }
  if (g_proposal_count == g_proposal_capacity) {
    size_t grown = g_proposal_capacity ? g_proposal_capacity * 2 : 8;
    IRRuleProposal *table =
        realloc(g_proposals, grown * sizeof(IRRuleProposal));
    if (!table) {
      return 1;
    }
    g_proposals = table;
    g_proposal_capacity = grown;
  }
  entry = &g_proposals[g_proposal_count++];
  snprintf(entry->file, sizeof(entry->file), "%s", file);
  entry->line = line;
  snprintf(entry->replacement, sizeof(entry->replacement), "%s", replacement);
  snprintf(entry->rule, sizeof(entry->rule), "%s", rule ? rule : "?");
  return 1;
}

/* Apply what the rules proposed, one line each, and say what changed. A
 * proposal is ordinary Mettle the program wrote: the compiler puts it where the
 * rule said and compiles the result from scratch, with nothing exempted. */
int ir_rules_apply_fixes(FILE *out) {
  int applied = 0;
  if (!g_apply_proposals || g_proposal_count == 0) {
    return 0;
  }
  for (size_t i = 0; i < g_proposal_count; i++) {
    IRRuleProposal *p = &g_proposals[i];
    FILE *in = fopen(p->file, "rb");
    char *text = NULL;
    long size = 0;
    size_t at = 0;
    size_t line = 1;
    size_t start = 0;
    size_t end = 0;
    if (!in) {
      continue;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    text = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!text || fread(text, 1, (size_t)size, in) != (size_t)size) {
      free(text);
      fclose(in);
      continue;
    }
    text[size] = '\0';
    fclose(in);
    for (at = 0; at <= (size_t)size; at++) {
      if (line == p->line && start == 0 && !(p->line == 1)) {
        start = at;
      }
      if (p->line == 1) {
        start = 0;
      }
      if (at == (size_t)size || text[at] == '\n') {
        if (line == p->line) {
          end = at;
          break;
        }
        line++;
        if (line == p->line) {
          start = at + 1;
        }
      }
    }
    if (end >= start) {
      FILE *w = fopen(p->file, "wb");
      if (w) {
        fwrite(text, 1, start, w);
        fwrite(p->replacement, 1, strlen(p->replacement), w);
        fwrite(text + end, 1, (size_t)size - end, w);
        fclose(w);
        applied++;
        if (out) {
          fprintf(out, "fix: %s:%zu rewritten by rule %s\n", p->file, p->line,
                  p->rule);
          fprintf(out, "  %s\n", p->replacement);
        }
      }
    }
    free(text);
  }
  return applied;
}

static void report_at_rule_code(ErrorReporter *reporter, const IRFunction *rule,
                                const char *message, const char *label,
                                int is_error, const char *code) {
  SourceSpan span = source_span_from_location(rule->location, 4);
  span = error_reporter_span_snap_to_token(reporter, span, "rule");
  if (is_error) {
    error_reporter_add_error_with_span(reporter, ERROR_SEMANTIC, span, message);
  } else {
    error_reporter_add_warning_with_span(reporter, ERROR_SEMANTIC, span,
                                         message);
  }
  if (label) {
    error_reporter_set_last_label(reporter, label);
  }
  error_reporter_set_last_code(reporter, code);
}

static void report_at_rule(ErrorReporter *reporter, const IRFunction *rule,
                           const char *message, const char *label,
                           int is_error) {
  report_at_rule_code(reporter, rule, message, label, is_error, "R0001");
}

static void report_leaks(ErrorReporter *reporter, IRInterpMachine *machine,
                         const IRFunction *rule) {
  size_t buffers = ir_interp_buffer_count(machine);
  for (size_t i = 0; i < buffers; i++) {
    size_t line = ir_interp_buffer_alloc_line(machine, i);
    if (line == 0 || ir_interp_buffer_freed(machine, i)) {
      continue;
    }
    long long size = 0;
    ir_interp_buffer_data(machine, i, &size);
    char message[256];
    snprintf(message, sizeof(message),
             "rule '%s' leaked %lld bytes: this allocation is never freed",
             rule_display_name(rule->name), size);
    SourceLocation location = source_location_create(line, 1);
    error_reporter_add_warning_with_suggestion(
        reporter, ERROR_SEMANTIC, location, message,
        "free it before the rule returns");
  }
}

static int run_one_rule(IRProgram *program, IRFunction *rule,
                        const IRRuleImage *image, ErrorReporter *reporter,
                        FILE *report, long long fuel, IRRuleStats *stats,
                        int *failed_build) {
  const char *display = rule_display_name(rule->name);
  const MtlcType *verdict_type = find_verdict_type(program, rule);
  IRInterpMachine *machine = ir_interp_create(program);
  if (!machine || !verdict_type) {
    if (machine) {
      ir_interp_destroy(machine);
    }
    report_at_rule(reporter, rule,
                   "the compiler could not prepare the interpreter for this "
                   "rule",
                   NULL, 1);
    *failed_build = 1;
    stats->malformed++;
    return 0;
  }
  unsigned char *bytes = malloc(image->size ? image->size : 1);
  if (!bytes) {
    ir_interp_destroy(machine);
    return 0;
  }
  memcpy(bytes, image->bytes, image->size);
  unsigned long long base = ir_interp_next_buffer_address(machine);
  for (size_t i = 0; i < image->pointer_count; i++) {
    size_t offset = image->pointer_offsets[i];
    unsigned long long value = 0;
    if (offset + 8 > image->size) {
      continue;
    }
    memcpy(&value, bytes + offset, 8);
    value += base;
    memcpy(bytes + offset, &value, 8);
  }
  unsigned long long address =
      ir_interp_add_buffer(machine, bytes, (long long)image->size);
  free(bytes);
  if (address != base) {
    ir_interp_destroy(machine);
    report_at_rule(reporter, rule,
                   "the compiler could not place the program image for this "
                   "rule",
                   NULL, 1);
    *failed_build = 1;
    stats->malformed++;
    return 0;
  }
  IRInterpValue arg;
  memset(&arg, 0, sizeof(arg));
  arg.i = (long long)address;
  IRInterpValue result;
  memset(&result, 0, sizeof(result));
  IRInterpStatus status = ir_interp_run(machine, rule, &arg, 1, &result, fuel);
  long long steps = fuel - ir_interp_fuel_remaining(machine);
  stats->steps += steps;
  stats->rules++;

  char message[1600];
  if (status != IR_INTERP_OK) {
    const char *why = status == IR_INTERP_FUEL
                          ? "did not finish within its step budget"
                          : status == IR_INTERP_UNSUPPORTED
                                ? "uses something the compile-time "
                                  "interpreter cannot run"
                                : "trapped";
    if (status == IR_INTERP_FUEL) {
      snprintf(message, sizeof(message), "rule '%s' %s of %lld steps", display,
               why, fuel);
    } else {
      snprintf(message, sizeof(message), "rule '%s' %s: %s", display, why,
               ir_interp_status_detail(machine));
    }
    report_at_rule(reporter, rule, message, "this rule gave no verdict", 1);
    if (report) {
      fprintf(report, "rule %s: no verdict (%s), %lld steps\n", display, why,
              steps);
    }
    ir_explain_rule_ran(display, "no verdict", steps);
    ir_interp_destroy(machine);
    *failed_build = 1;
    stats->malformed++;
    return 1;
  }

  RuleVerdict verdict;
  decode_verdict(machine, verdict_type, (unsigned long long)result.i,
                 &verdict);
  report_leaks(reporter, machine, rule);
  if (!verdict.readable || verdict.outcome < 0 || verdict.outcome > 2) {
    snprintf(message, sizeof(message),
             "rule '%s' returned a verdict the compiler could not read; "
             "answer with verdict_pass(), verdict_fail(site, message) or "
             "verdict_gap(site, message)",
             display);
    report_at_rule(reporter, rule, message, "malformed verdict", 1);
    if (report) {
      fprintf(report, "rule %s: malformed verdict, %lld steps\n", display,
              steps);
    }
    ir_explain_rule_ran(display, "malformed verdict", steps);
    ir_interp_destroy(machine);
    *failed_build = 1;
    stats->malformed++;
    return 1;
  }
  if (verdict.outcome == 0) {
    stats->passed++;
    if (report) {
      fprintf(report, "rule %s: pass, %lld steps\n", display, steps);
    }
    ir_explain_rule_ran(display, "pass", steps);
    ir_interp_destroy(machine);
    return 1;
  }

  const IRRuleSite *site = NULL;
  int anonymous = verdict.line == 0 && verdict.column == 0 &&
                  verdict.file[0] == '\0';
  if (!anonymous) {
    site = match_site(image, &verdict);
    if (!site) {
      snprintf(message, sizeof(message),
               "rule '%s' named a site that is not in the program: %s:%lld:%lld",
               display, verdict.file, verdict.line, verdict.column);
      report_at_rule(reporter, rule, message,
                     "a verdict's site must come from the Program the rule "
                     "was given",
                     1);
      if (report) {
        fprintf(report, "rule %s: malformed site, %lld steps\n", display,
                steps);
      }
      ir_interp_destroy(machine);
      *failed_build = 1;
      stats->malformed++;
      return 1;
    }
  }

  if (verdict.outcome == 1) {
    snprintf(message, sizeof(message), "rule '%s' failed: %s", display,
             verdict.message[0] ? verdict.message : "(no message)");
  } else {
    snprintf(message, sizeof(message),
             "rule '%s' could not decide here: %s", display,
             verdict.message[0] ? verdict.message : "(no message)");
  }
  if (site) {
    SourceLocation location;
    location.line = site->line;
    location.column = site->column;
    location.filename = site->file;
    SourceSpan span = source_span_from_location(location, 1);
    if (verdict.outcome == 1) {
      error_reporter_add_error_with_span(reporter, ERROR_SEMANTIC, span,
                                         message);
    } else {
      error_reporter_add_warning_with_span(reporter, ERROR_SEMANTIC, span,
                                           message);
    }
    error_reporter_set_last_label(reporter, verdict.outcome == 1
                                                ? "the rule points here"
                                                : "unproven here");
    error_reporter_set_last_code(reporter,
                                 rule->explain_code
                                     ? rule->explain_code
                                     : (verdict.outcome == 1 ? "R0002"
                                                             : "R0003"));
    SourceSpan declared = source_span_from_location(rule->location, 4);
    declared = error_reporter_span_snap_to_token(reporter, declared, "rule");
    error_reporter_add_note_of_span(reporter, declared,
                                    verdict.outcome == 1
                                        ? "the rule that failed the build"
                                        : "the rule that announced the gap");
    if (rule->explain_text) {
      error_reporter_add_note_of_span(reporter, declared, rule->explain_text);
    }
    if (verdict.fix[0]) {
      char note[1200];
      snprintf(note, sizeof(note),
               "the rule proposes this line instead, as ordinary Mettle: %s",
               verdict.fix);
      error_reporter_add_note_of_span(reporter, span, note);
      if (!ir_rules_propose(verdict.file, (size_t)verdict.line, verdict.fix,
                            display)) {
        *failed_build = 1;
      }
    }
  } else {
    report_at_rule_code(reporter, rule, message,
                        "this rule speaks about the program as a whole",
                        verdict.outcome == 1,
                        rule->explain_code
                            ? rule->explain_code
                            : (verdict.outcome == 1 ? "R0002" : "R0003"));
    if (rule->explain_text && reporter) {
      SourceSpan at_rule = source_span_from_location(rule->location, 4);
      at_rule = error_reporter_span_snap_to_token(reporter, at_rule, "rule");
      error_reporter_add_note_of_span(reporter, at_rule, rule->explain_text);
    }
  }
  if (report) {
    fprintf(report, "rule %s: %s, %lld steps\n", display,
            verdict.outcome == 1 ? "fail" : "gap", steps);
  }
  ir_explain_rule_ran(display, verdict.outcome == 1 ? "fail" : "gap", steps);
  if (verdict.outcome == 1) {
    stats->failed++;
    *failed_build = 1;
  } else {
    stats->gaps++;
  }
  ir_interp_destroy(machine);
  return 1;
}

/* A rule is code the compiler does not trust, and a rule that answers
 * differently on two runs over the same program decided by accident. Under
 * `mettle test` every rule is run a second time in a fresh machine over a
 * freshly placed image, and a verdict that moved is reported. */
static int rule_verdict_again(IRProgram *program, IRFunction *rule,
                              const IRRuleImage *image, long long fuel,
                              RuleVerdict *out, IRInterpStatus *out_status) {
  const MtlcType *verdict_type = find_verdict_type(program, rule);
  IRInterpMachine *machine = ir_interp_create(program);
  unsigned char *bytes;
  unsigned long long base;
  unsigned long long address;
  IRInterpValue arg;
  IRInterpValue result;
  if (!machine || !verdict_type) {
    if (machine) {
      ir_interp_destroy(machine);
    }
    return 0;
  }
  bytes = malloc(image->size ? image->size : 1);
  if (!bytes) {
    ir_interp_destroy(machine);
    return 0;
  }
  memcpy(bytes, image->bytes, image->size);
  base = ir_interp_next_buffer_address(machine);
  for (size_t i = 0; i < image->pointer_count; i++) {
    size_t offset = image->pointer_offsets[i];
    unsigned long long value = 0;
    if (offset + 8 > image->size) {
      continue;
    }
    memcpy(&value, bytes + offset, 8);
    value += base;
    memcpy(bytes + offset, &value, 8);
  }
  address = ir_interp_add_buffer(machine, bytes, (long long)image->size);
  free(bytes);
  if (address != base) {
    ir_interp_destroy(machine);
    return 0;
  }
  memset(&arg, 0, sizeof(arg));
  arg.i = (long long)address;
  memset(&result, 0, sizeof(result));
  *out_status = ir_interp_run(machine, rule, &arg, 1, &result, fuel);
  memset(out, 0, sizeof(*out));
  if (*out_status == IR_INTERP_OK) {
    decode_verdict(machine, verdict_type, (unsigned long long)result.i, out);
  }
  ir_interp_destroy(machine);
  return 1;
}

static void rule_cross_check(IRProgram *program, IRFunction *rule,
                             const IRRuleImage *image, ErrorReporter *reporter,
                             FILE *report, long long fuel,
                             IRRuleStats *stats) {
  RuleVerdict first;
  RuleVerdict second;
  IRInterpStatus first_status = IR_INTERP_OK;
  IRInterpStatus second_status = IR_INTERP_OK;
  const char *display = rule_display_name(rule->name);
  char message[512];
  if (!rule_verdict_again(program, rule, image, fuel, &first,
                          &first_status) ||
      !rule_verdict_again(program, rule, image, fuel, &second,
                          &second_status)) {
    return;
  }
  if (first_status == second_status && first.outcome == second.outcome &&
      first.line == second.line && first.column == second.column &&
      strcmp(first.message, second.message) == 0) {
    if (report) {
      fprintf(report, "rule %s: verdict held on a second run\n", display);
    }
    return;
  }
  snprintf(message, sizeof(message),
           "rule '%s' answered differently on a second run over the same "
           "program: first %s, then %s. A rule that is not a function of the "
           "program it reads decided by accident",
           display, first.outcome == 0 ? "pass" : first.outcome == 1 ? "fail"
                                                                     : "gap",
           second.outcome == 0 ? "pass"
                               : second.outcome == 1 ? "fail" : "gap");
  report_at_rule_code(reporter, rule, message, "unstable verdict", 1, "R0005");
  stats->malformed++;
  if (report) {
    fprintf(report, "rule %s: verdict moved between runs\n", display);
  }
}

int ir_rules_run(IRProgram *program, const IRRuleImage *image,
                 ErrorReporter *reporter, FILE *report, long long budget,
                 IRRuleStats *stats) {
  return ir_rules_run_checked(program, image, reporter, report, budget, 0,
                              stats);
}

int ir_rules_run_checked(IRProgram *program, const IRRuleImage *image,
                         ErrorReporter *reporter, FILE *report,
                         long long budget, int cross_check,
                         IRRuleStats *stats) {
  return ir_rules_run_kind(program, image, reporter, report, budget,
                           cross_check, IR_RULE_OVER_PROGRAM, stats);
}

int ir_rules_run_kind(IRProgram *program, const IRRuleImage *image,
                      ErrorReporter *reporter, FILE *report, long long budget,
                      int cross_check, IRRuleKind kind, IRRuleStats *stats) {
  IRRuleStats local;
  int failed_build = 0;
  if (!stats) {
    stats = &local;
  }
  memset(stats, 0, sizeof(*stats));
  if (!program || !image || !reporter) {
    return 0;
  }
  long long fuel = budget > 0 ? budget : IR_RULE_DEFAULT_FUEL;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *rule = program->functions[i];
    if (!rule || !rule->is_rule || ir_rule_kind(rule) != kind) {
      continue;
    }
    if (!run_one_rule(program, rule, image, reporter, report, fuel, stats,
                      &failed_build)) {
      return 0;
    }
    if (cross_check) {
      size_t before = stats->malformed;
      rule_cross_check(program, rule, image, reporter, report, fuel, stats);
      if (stats->malformed != before) {
        failed_build = 1;
      }
    }
  }
  if (budget > 0 && stats->steps > budget) {
    char message[256];
    snprintf(message, sizeof(message),
             "rules spent %lld interpreter steps, over the budget of %lld",
             stats->steps, budget);
    error_reporter_add_error(reporter, ERROR_SEMANTIC,
                             source_location_create(0, 0), message);
    error_reporter_set_last_code(reporter, "R0004");
    failed_build = 1;
  }
  if (report) {
    fprintf(report, "rules: %zu run, %zu passed, %zu failed, %zu gaps, %lld "
                    "steps\n",
            stats->rules, stats->passed, stats->failed, stats->gaps,
            stats->steps);
  }
  return failed_build ? 0 : 1;
}
