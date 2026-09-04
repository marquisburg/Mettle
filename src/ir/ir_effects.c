#include "ir_effects.h"
#include "ir_explain_ledger.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long Word;

#define EFFECT_NAME_UNKNOWN "unknown"
#define NO_SITE ((size_t)-1)

typedef struct {
  IRFunction *fn;
  Word *sources;
  Word *performs;
  Word *needs;
  Word *with;
  Word *forbids;
  Word *requires;
  Word *provides;
  size_t *callees;
  size_t *callee_sites;
  size_t callee_count;
  size_t callee_capacity;
  size_t *source_sites;
  size_t *indirect_sites;
  Word **indirect_requires;
  size_t indirect_count;
  size_t indirect_capacity;
} EFn;

typedef struct {
  const char *name;
  size_t index;
} NameEntry;

typedef struct {
  const char *name;
  size_t *writers;
  size_t *sites;
  size_t writer_count;
  size_t writer_capacity;
} EGlobal;

typedef struct {
  IRProgram *program;
  const IREffectInput *input;
  ErrorReporter *reporter;
  size_t bit_count;
  size_t words;
  EFn *fns;
  size_t fn_count;
  NameEntry *index;
  size_t index_count;
  int failed;
  int errors;
  long long steps;
  size_t rounds;
  EGlobal *globals;
  size_t global_count;
  size_t global_capacity;
  char **owned;
  size_t owned_count;
  size_t owned_capacity;
} Ctx;

struct IREffectResults {
  char **names;
  const char ***performs;
  size_t *perform_counts;
  const char ***needs;
  size_t *need_counts;
  size_t count;
};

int ir_effects_name_is_allocator(const char *name) {
  static const char *const allocators[] = {
      "malloc",        "calloc",          "realloc",  "aligned_alloc",
      "_aligned_malloc", "strdup",        "_strdup",  "wcsdup",
      "HeapAlloc",     "VirtualAlloc",    "mmap",     "mettle_heap_zeroed",
      NULL,
  };
  if (!name) {
    return 0;
  }
  for (size_t i = 0; allocators[i]; i++) {
    if (strcmp(name, allocators[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_effects_name_is_known_clean(const char *name) {
  static const char *const clean[] = {
      "sqrt",   "sqrtf", "sin",    "sinf",   "cos",    "cosf",  "tan",
      "tanf",   "exp",   "expf",   "log",    "logf",   "log2",  "log2f",
      "pow",    "powf",  "fabs",   "fabsf",  "floor",  "floorf", "ceil",
      "ceilf",  "round", "roundf", "fmod",   "fmodf",  "atan",  "atanf",
      "atan2",  "atan2f", "memcpy", "memset", "memmove", "memcmp",
      "strlen", "strcmp", "strncmp", "abs",   "labs",   "llabs", "free",
      NULL,
  };
  if (!name) {
    return 0;
  }
  /* The compiler's own helpers: the traps a check branches to, the effect
   * frames, the refinement re-checks, the shadow map behind `--safe`, and the
   * lines `--record-trace` writes. It put them there and knows what they do,
   * so a `@noalloc` proof does not have to fall over because a checked build
   * added one. */
  if (strstr(name, "crash_trap") != NULL ||
      strncmp(name, "mettle_effects_", 15) == 0 ||
      strncmp(name, "mettle_refine_", 14) == 0 ||
      strncmp(name, "mettle_safety_", 14) == 0 ||
      strncmp(name, "mettle_trace_", 13) == 0) {
    return 1;
  }
  for (size_t i = 0; clean[i]; i++) {
    if (strcmp(name, clean[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_program_declares_effects(const IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *fn = program->functions[i];
    if (!fn) {
      continue;
    }
    if (fn->effects_with_count || fn->effects_forbids_count ||
        fn->effects_requires_count || fn->effects_provides_count) {
      return 1;
    }
    for (size_t k = 0; k < fn->instruction_count; k++) {
      if (fn->instructions[k].op == IR_OP_CALL_INDIRECT &&
          fn->instructions[k].effect_signature) {
        return 1;
      }
    }
  }
  return 0;
}

static const char *display_name(const char *name) {
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

static int bit_test(const Word *set, size_t bit) {
  return (set[bit / 64] >> (bit % 64)) & 1ull;
}

static void bit_set(Word *set, size_t bit) {
  set[bit / 64] |= 1ull << (bit % 64);
}

static int words_or(Word *into, const Word *from, size_t words) {
  int changed = 0;
  for (size_t w = 0; w < words; w++) {
    Word next = into[w] | from[w];
    if (next != into[w]) {
      into[w] = next;
      changed = 1;
    }
  }
  return changed;
}

static int words_any(const Word *set, size_t words) {
  for (size_t w = 0; w < words; w++) {
    if (set[w]) {
      return 1;
    }
  }
  return 0;
}

static Word *words_new(const Ctx *ctx) {
  return calloc(ctx->words, sizeof(Word));
}

static int effect_bit(const Ctx *ctx, const char *name) {
  if (!name) {
    return -1;
  }
  if (strcmp(name, EFFECT_NAME_UNKNOWN) == 0) {
    return (int)(ctx->bit_count - 1);
  }
  for (size_t i = 0; i < ctx->input->effect_count; i++) {
    if (strcmp(ctx->input->effects[i].name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static const char *effect_name(const Ctx *ctx, size_t bit) {
  if (bit + 1 == ctx->bit_count) {
    return EFFECT_NAME_UNKNOWN;
  }
  return ctx->input->effects[bit].name;
}

static int name_entry_compare(const void *left, const void *right) {
  return strcmp(((const NameEntry *)left)->name,
                ((const NameEntry *)right)->name);
}

static EFn *find_fn(Ctx *ctx, const char *name) {
  NameEntry key;
  NameEntry *hit;
  if (!name || ctx->index_count == 0) {
    return NULL;
  }
  key.name = name;
  key.index = 0;
  hit = bsearch(&key, ctx->index, ctx->index_count, sizeof(NameEntry),
                name_entry_compare);
  return hit ? &ctx->fns[hit->index] : NULL;
}

static int add_callee(EFn *efn, size_t callee, size_t site) {
  for (size_t i = 0; i < efn->callee_count; i++) {
    if (efn->callees[i] == callee) {
      return 1;
    }
  }
  if (efn->callee_count == efn->callee_capacity) {
    size_t next = efn->callee_capacity ? efn->callee_capacity * 2 : 8;
    size_t *grown = realloc(efn->callees, next * sizeof(size_t));
    size_t *grown_sites = realloc(efn->callee_sites, next * sizeof(size_t));
    if (!grown || !grown_sites) {
      free(grown);
      free(grown_sites);
      return 0;
    }
    efn->callees = grown;
    efn->callee_sites = grown_sites;
    efn->callee_capacity = next;
  }
  efn->callees[efn->callee_count] = callee;
  efn->callee_sites[efn->callee_count] = site;
  efn->callee_count++;
  return 1;
}

static int add_indirect(Ctx *ctx, EFn *efn, size_t site, Word *requires) {
  if (efn->indirect_count == efn->indirect_capacity) {
    size_t next = efn->indirect_capacity ? efn->indirect_capacity * 2 : 4;
    size_t *grown = realloc(efn->indirect_sites, next * sizeof(size_t));
    Word **grown_req = realloc(efn->indirect_requires, next * sizeof(Word *));
    if (!grown || !grown_req) {
      free(grown);
      free(grown_req);
      return 0;
    }
    efn->indirect_sites = grown;
    efn->indirect_requires = grown_req;
    efn->indirect_capacity = next;
  }
  (void)ctx;
  efn->indirect_sites[efn->indirect_count] = site;
  efn->indirect_requires[efn->indirect_count] = requires;
  efn->indirect_count++;
  return 1;
}

static void note_source(EFn *efn, size_t bit, size_t site) {
  bit_set(efn->sources, bit);
  if (efn->source_sites[bit] == NO_SITE) {
    efn->source_sites[bit] = site;
  }
}

static int parse_name_list(Ctx *ctx, const char *text, const char *end,
                           Word *into, int *saw_none) {
  const char *p = text;
  while (p < end) {
    const char *comma = p;
    char name[128];
    size_t take;
    int bit;
    while (comma < end && *comma != ',') {
      comma++;
    }
    take = (size_t)(comma - p);
    if (take == 0 || take >= sizeof(name)) {
      return 0;
    }
    memcpy(name, p, take);
    name[take] = '\0';
    if (strcmp(name, "none") == 0) {
      *saw_none = 1;
    } else {
      bit = effect_bit(ctx, name);
      if (bit < 0) {
        return 0;
      }
      bit_set(into, (size_t)bit);
    }
    p = comma < end ? comma + 1 : end;
  }
  return 1;
}

static int parse_signature(Ctx *ctx, const char *signature, Word *with,
                           int *closed, Word *requires) {
  const char *p = signature ? signature : "";
  *closed = 0;
  if (strcmp(p, "open") == 0) {
    return 1;
  }
  while (*p) {
    int is_with;
    const char *end;
    int saw_none = 0;
    while (*p == ' ') {
      p++;
    }
    if (!*p) {
      break;
    }
    is_with = strncmp(p, "with ", 5) == 0;
    if (!is_with && strncmp(p, "requires ", 9) != 0) {
      return 0;
    }
    p += is_with ? 5 : 9;
    end = p;
    while (*end && *end != ' ') {
      end++;
    }
    if (!parse_name_list(ctx, p, end, is_with ? with : requires, &saw_none)) {
      return 0;
    }
    if (is_with) {
      *closed = 1;
    }
    p = end;
  }
  return 1;
}

static int clause_bits(Ctx *ctx, const char **names, size_t count,
                       Word *into) {
  for (size_t i = 0; i < count; i++) {
    int bit = effect_bit(ctx, names[i]);
    if (bit < 0) {
      if (names[i] && strcmp(names[i], "none") == 0) {
        continue;
      }
      return 0;
    }
    bit_set(into, (size_t)bit);
  }
  return 1;
}

static int callee_is_defined(const EFn *callee) {
  return callee && callee->fn && callee->fn->instruction_count > 0;
}

static void believe_extern(const char *name, const char *clause) {
  char what[192];
  char why[320];
  snprintf(what, sizeof(what), "extern `%s`", name ? name : "?");
  if (clause) {
    snprintf(why, sizeof(why),
             "its `with %s` clause is taken as written; nothing outside the "
             "program was analysed",
             clause);
  } else {
    snprintf(why, sizeof(why),
             "it is on the compiler's known-clean list, so it was assumed to "
             "perform nothing");
  }
  ir_explain_belief(what, why);
}

static int extern_performs(Ctx *ctx, EFn *efn, const char *name,
                           const EFn *callee, size_t site) {
  int alloc_bit = effect_bit(ctx, "alloc");
  const IRModuleSymbol *symbol = ir_program_lookup_symbol(ctx->program, name);
  if (callee && callee->fn->effects_with_count > 0) {
    believe_extern(name, callee->fn->effects_with[0]);
    Word *with = words_new(ctx);
    if (!with) {
      return 0;
    }
    if (!clause_bits(ctx, callee->fn->effects_with,
                     callee->fn->effects_with_count, with)) {
      free(with);
      return 0;
    }
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      if (bit_test(with, bit)) {
        note_source(efn, bit, site);
      }
    }
    free(with);
    return 1;
  }
  if (symbol && symbol->effect_clause) {
    Word *with = words_new(ctx);
    int saw_none = 0;
    const char *text = symbol->effect_clause;
    believe_extern(name, text);
    if (!with) {
      return 0;
    }
    if (!parse_name_list(ctx, text, text + strlen(text), with, &saw_none)) {
      free(with);
      return 0;
    }
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      if (bit_test(with, bit)) {
        note_source(efn, bit, site);
      }
    }
    free(with);
    return 1;
  }
  if (ir_effects_name_is_known_clean(name)) {
    believe_extern(name, NULL);
    return 1;
  }
  {
    char what[192];
    char why[320];
    snprintf(what, sizeof(what), "extern `%s`", name ? name : "?");
    snprintf(why, sizeof(why),
             "it declares no effects, so it was taken to allocate and to "
             "perform no effect this program declared; write `with ...` on "
             "it to say what it does");
    ir_explain_belief(what, why);
  }
  if (alloc_bit >= 0) {
    note_source(efn, (size_t)alloc_bit, site);
  }
  return 1;
}

int ir_function_symbol_is_parameter(const IRFunction *function,
                                    const char *symbol_name);
int ir_instruction_writes_symbol(const IRInstruction *instruction);
const char *ir_function_local_declared_type(const IRFunction *function,
                                            const char *name);

/* An object named `*g` has no name in the program, so the pass owns the
 * string. One copy per distinct object, freed with the rest of the table. */
static const char *intern_object(Ctx *ctx, const char *name) {
  for (size_t i = 0; i < ctx->global_count; i++) {
    if (strcmp(ctx->globals[i].name, name) == 0) {
      return ctx->globals[i].name;
    }
  }
  if (name[0] != '*') {
    return name;
  }
  {
    size_t length = strlen(name) + 1;
    char *copy = malloc(length);
    if (!copy) {
      return name;
    }
    memcpy(copy, name, length);
    if (ctx->owned_count == ctx->owned_capacity) {
      size_t grown = ctx->owned_capacity ? ctx->owned_capacity * 2 : 8;
      char **table = realloc(ctx->owned, grown * sizeof(char *));
      if (!table) {
        free(copy);
        return name;
      }
      ctx->owned = table;
      ctx->owned_capacity = grown;
    }
    ctx->owned[ctx->owned_count++] = copy;
    return copy;
  }
}

static int note_global_writer(Ctx *ctx, const char *name, size_t writer,
                              size_t site) {
  EGlobal *entry = NULL;
  for (size_t i = 0; i < ctx->global_count; i++) {
    if (strcmp(ctx->globals[i].name, name) == 0) {
      entry = &ctx->globals[i];
      break;
    }
  }
  if (!entry) {
    if (ctx->global_count == ctx->global_capacity) {
      size_t grown = ctx->global_capacity ? ctx->global_capacity * 2 : 16;
      EGlobal *table = realloc(ctx->globals, grown * sizeof(EGlobal));
      if (!table) {
        return 0;
      }
      ctx->globals = table;
      ctx->global_capacity = grown;
    }
    entry = &ctx->globals[ctx->global_count++];
    memset(entry, 0, sizeof(*entry));
    entry->name = name;
  }
  for (size_t i = 0; i < entry->writer_count; i++) {
    if (entry->writers[i] == writer) {
      return 1;
    }
  }
  if (entry->writer_count == entry->writer_capacity) {
    size_t grown = entry->writer_capacity ? entry->writer_capacity * 2 : 4;
    size_t *writers = realloc(entry->writers, grown * sizeof(size_t));
    size_t *sites = NULL;
    if (!writers) {
      return 0;
    }
    entry->writers = writers;
    sites = realloc(entry->sites, grown * sizeof(size_t));
    if (!sites) {
      return 0;
    }
    entry->sites = sites;
    entry->writer_capacity = grown;
  }
  entry->sites[entry->writer_count] = site;
  entry->writers[entry->writer_count++] = writer;
  return 1;
}

static int symbol_is_global(const IRFunction *fn, const char *name) {
  return name && !ir_function_symbol_is_parameter(fn, name) &&
         !ir_function_local_declared_type(fn, name);
}

/* The global an address was computed from. A store through a pointer is a
 * write to whatever that pointer names, and the compiler can say what that is
 * exactly when the address came out of a global: `g_buf[i] = v` writes the
 * block `g_buf` points at, and `g_jobs[i] = v` writes `g_jobs` itself. Both
 * are objects two threads can share, and neither was visible while only the
 * symbol a write names was counted. */
static const char *store_base_global(const IRProgram *program,
                                     const IRFunction *fn, size_t at) {
  IROperand address = fn->instructions[at].dest;
  int guard = 0;
  while (guard++ < 16) {
    if (address.kind == IR_OPERAND_SYMBOL) {
      return symbol_is_global(fn, address.name) ? address.name : NULL;
    }
    if (address.kind != IR_OPERAND_TEMP || !address.name) {
      return NULL;
    }
    {
      const IRInstruction *source = NULL;
      for (size_t i = at; i-- > 0;) {
        const IRInstruction *candidate = &fn->instructions[i];
        if (candidate->dest.kind == IR_OPERAND_TEMP && candidate->dest.name &&
            strcmp(candidate->dest.name, address.name) == 0) {
          source = candidate;
          break;
        }
      }
      if (!source) {
        return NULL;
      }
      if (source->op == IR_OP_BINARY || source->op == IR_OP_ASSIGN ||
          source->op == IR_OP_CAST || source->op == IR_OP_ADDRESS_OF) {
        address = source->lhs;
        continue;
      }
      return NULL;
    }
  }
  (void)program;
  return NULL;
}

static const char *store_object_name(Ctx *ctx, const IRFunction *fn, size_t at,
                                     char *storage, size_t capacity) {
  const char *base = store_base_global(ctx->program, fn, at);
  const IRModuleSymbol *symbol = NULL;
  if (!base) {
    return NULL;
  }
  symbol = ir_program_lookup_symbol(ctx->program, base);
  if (symbol && symbol->type && symbol->type->kind == MTLC_TYPE_POINTER) {
    snprintf(storage, capacity, "*%s", base);
    return storage;
  }
  return base;
}

static int scan_globals(Ctx *ctx, size_t index) {
  const IRFunction *fn = ctx->fns[index].fn;
  if (fn->is_rule || fn->rewrite_role) {
    return 1;
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (insn->op == IR_OP_STORE) {
      char storage[128];
      const char *object = store_object_name(ctx, fn, i, storage,
                                             sizeof(storage));
      if (object && !note_global_writer(ctx, intern_object(ctx, object), index,
                                        i)) {
        return 0;
      }
      continue;
    }
    if (!ir_instruction_writes_symbol(insn) || !insn->dest.name) {
      continue;
    }
    if (!symbol_is_global(fn, insn->dest.name)) {
      continue;
    }
    if (!note_global_writer(ctx, insn->dest.name, index, i)) {
      return 0;
    }
  }
  return 1;
}

static int scan_function(Ctx *ctx, size_t index) {
  EFn *efn = &ctx->fns[index];
  IRFunction *fn = efn->fn;
  ctx->steps += (long long)fn->instruction_count;
  int alloc_bit = effect_bit(ctx, "alloc");
  int asm_bit = effect_bit(ctx, "asm");
  int syscall_bit = effect_bit(ctx, "syscall");
  size_t unknown_bit = ctx->bit_count - 1;
  if (!clause_bits(ctx, fn->effects_with, fn->effects_with_count, efn->with) ||
      !clause_bits(ctx, fn->effects_forbids, fn->effects_forbids_count,
                   efn->forbids) ||
      !clause_bits(ctx, fn->effects_requires, fn->effects_requires_count,
                   efn->requires) ||
      !clause_bits(ctx, fn->effects_provides, fn->effects_provides_count,
                   efn->provides)) {
    return 0;
  }
  for (size_t bit = 0; bit < ctx->bit_count; bit++) {
    if (bit_test(efn->with, bit)) {
      note_source(efn, bit, NO_SITE);
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    switch (insn->op) {
    case IR_OP_NEW:
      if (alloc_bit >= 0) {
        note_source(efn, (size_t)alloc_bit, i);
      }
      break;
    case IR_OP_INLINE_ASM:
      if (asm_bit >= 0) {
        note_source(efn, (size_t)asm_bit, i);
      }
      break;
    case IR_OP_CALL: {
      EFn *callee;
      if (!insn->text) {
        break;
      }
      if (strcmp(insn->text, IR_SYSCALL_CALL_NAME) == 0) {
        if (syscall_bit >= 0) {
          note_source(efn, (size_t)syscall_bit, i);
        }
        break;
      }
      callee = find_fn(ctx, insn->text);
      if (callee_is_defined(callee)) {
        if (!add_callee(efn, (size_t)(callee - ctx->fns), i)) {
          return 0;
        }
        break;
      }
      if (ir_effects_name_is_allocator(insn->text)) {
        if (alloc_bit >= 0) {
          note_source(efn, (size_t)alloc_bit, i);
        }
        break;
      }
      if (!extern_performs(ctx, efn, insn->text, callee, i)) {
        return 0;
      }
      break;
    }
    case IR_OP_CALL_INDIRECT: {
      Word *with = words_new(ctx);
      Word *requires = words_new(ctx);
      int closed = 0;
      if (!with || !requires) {
        free(with);
        free(requires);
        return 0;
      }
      if (!parse_signature(ctx, insn->effect_signature, with, &closed,
                           requires)) {
        free(with);
        free(requires);
        return 0;
      }
      if (closed) {
        for (size_t bit = 0; bit < ctx->bit_count; bit++) {
          if (bit_test(with, bit)) {
            note_source(efn, bit, i);
          }
        }
      } else {
        note_source(efn, unknown_bit, i);
      }
      free(with);
      if (!add_indirect(ctx, efn, i, requires)) {
        free(requires);
        return 0;
      }
      break;
    }
    default:
      break;
    }
    if (insn->allocates && alloc_bit >= 0) {
      note_source(efn, (size_t)alloc_bit, i);
    }
  }
  memcpy(efn->performs, efn->sources, ctx->words * sizeof(Word));
  return 1;
}

static void propagate(Ctx *ctx) {
  int changed = 1;
  while (changed) {
    changed = 0;
    ctx->rounds++;
    for (size_t i = 0; i < ctx->fn_count; i++) {
      EFn *efn = &ctx->fns[i];
      ctx->steps++;
      for (size_t c = 0; c < efn->callee_count; c++) {
        ctx->steps++;
        changed |= words_or(efn->performs, ctx->fns[efn->callees[c]].performs,
                            ctx->words);
      }
    }
  }
  changed = 1;
  while (changed) {
    changed = 0;
    ctx->rounds++;
    for (size_t i = 0; i < ctx->fn_count; i++) {
      EFn *efn = &ctx->fns[i];
      ctx->steps++;
      Word *gathered = words_new(ctx);
      if (!gathered) {
        ctx->failed = 1;
        return;
      }
      memcpy(gathered, efn->requires, ctx->words * sizeof(Word));
      for (size_t c = 0; c < efn->callee_count; c++) {
        const Word *callee_needs = ctx->fns[efn->callees[c]].needs;
        ctx->steps++;
        for (size_t w = 0; w < ctx->words; w++) {
          gathered[w] |= callee_needs[w] & ~efn->provides[w];
        }
      }
      for (size_t k = 0; k < efn->indirect_count; k++) {
        for (size_t w = 0; w < ctx->words; w++) {
          gathered[w] |= efn->indirect_requires[k][w] & ~efn->provides[w];
        }
      }
      changed |= words_or(efn->needs, gathered, ctx->words);
      free(gathered);
    }
  }
}

typedef struct {
  size_t function;
  size_t via_site;
  size_t parent;
} Hop;

static int trace_chain(Ctx *ctx, size_t start, size_t bit, int follow_needs,
                       Hop **out_hops, size_t *out_length) {
  size_t count = ctx->fn_count;
  Hop *queue = calloc(count ? count : 1, sizeof(Hop));
  unsigned char *seen = calloc(count ? count : 1, 1);
  size_t head = 0;
  size_t tail = 0;
  size_t found = NO_SITE;
  if (!queue || !seen) {
    free(queue);
    free(seen);
    return 0;
  }
  queue[tail].function = start;
  queue[tail].via_site = NO_SITE;
  queue[tail].parent = NO_SITE;
  tail++;
  seen[start] = 1;
  while (head < tail) {
    size_t at = head++;
    EFn *efn = &ctx->fns[queue[at].function];
    int terminal = follow_needs ? bit_test(efn->requires, bit)
                                : bit_test(efn->sources, bit);
    if (terminal && (at != 0 || !follow_needs || bit_test(efn->requires, bit))) {
      found = at;
      break;
    }
    for (size_t c = 0; c < efn->callee_count && tail < count; c++) {
      size_t next = efn->callees[c];
      EFn *callee = &ctx->fns[next];
      int carries = follow_needs
                        ? bit_test(callee->needs, bit) && !bit_test(efn->provides, bit)
                        : bit_test(callee->performs, bit);
      if (seen[next] || !carries) {
        continue;
      }
      seen[next] = 1;
      queue[tail].function = next;
      queue[tail].via_site = efn->callee_sites[c];
      queue[tail].parent = at;
      tail++;
    }
  }
  free(seen);
  if (found == NO_SITE) {
    free(queue);
    *out_hops = NULL;
    *out_length = 0;
    return 1;
  }
  {
    size_t length = 0;
    size_t walk = found;
    Hop *path;
    while (walk != NO_SITE) {
      length++;
      walk = queue[walk].parent;
    }
    path = calloc(length, sizeof(Hop));
    if (!path) {
      free(queue);
      return 0;
    }
    walk = found;
    for (size_t i = length; i > 0; i--) {
      path[i - 1] = queue[walk];
      walk = queue[walk].parent;
    }
    free(queue);
    *out_hops = path;
    *out_length = length;
    return 1;
  }
}

static SourceSpan function_span(const IRFunction *fn) {
  return source_span_from_location(fn->location, 2);
}

static SourceSpan function_name_span(Ctx *ctx, const IRFunction *fn) {
  SourceSpan span = function_span(fn);
  if (ctx->reporter) {
    span = error_reporter_span_snap_to_token(ctx->reporter, span,
                                             display_name(fn->name));
  }
  return span;
}

static SourceSpan instruction_span(const EFn *efn, size_t site) {
  if (site == NO_SITE || site >= efn->fn->instruction_count) {
    return function_span(efn->fn);
  }
  return source_span_from_location(efn->fn->instructions[site].location, 1);
}

static void describe_chain(char *buffer, size_t size, Ctx *ctx, Hop *hops,
                           size_t length) {
  size_t used = 0;
  buffer[0] = '\0';
  for (size_t i = 0; i < length && used + 4 < size; i++) {
    const char *name = display_name(ctx->fns[hops[i].function].fn->name);
    int wrote = snprintf(buffer + used, size - used, "%s%s", i ? " -> " : "",
                         name);
    if (wrote < 0) {
      break;
    }
    used += (size_t)wrote;
  }
}

static void note_hops(Ctx *ctx, Hop *hops, size_t length) {
  if (!ctx->reporter) {
    return;
  }
  for (size_t i = 1; i < length; i++) {
    EFn *caller = &ctx->fns[hops[i - 1].function];
    char text[256];
    snprintf(text, sizeof(text), "`%s` calls `%s` here",
             display_name(caller->fn->name),
             display_name(ctx->fns[hops[i].function].fn->name));
    error_reporter_add_note_of_span(ctx->reporter,
                                    instruction_span(caller, hops[i].via_site),
                                    text);
  }
}

static void note_source_site(Ctx *ctx, EFn *efn, size_t bit) {
  char text[320];
  size_t site = efn->source_sites[bit];
  const char *name = display_name(efn->fn->name);
  const char *effect = effect_name(ctx, bit);
  if (!ctx->reporter) {
    return;
  }
  if (bit + 1 == ctx->bit_count) {
    snprintf(text, sizeof(text),
             "`%s` calls through a function type with no `with` clause here, "
             "and nothing bounds what that call performs; write `fn(...) "
             "with ...` on the type",
             name);
  } else if (site == NO_SITE) {
    snprintf(text, sizeof(text), "`%s` is declared `with %s`", name, effect);
  } else {
    const IRInstruction *insn = &efn->fn->instructions[site];
    if (insn->op == IR_OP_CALL && insn->text && !find_fn(ctx, insn->text)) {
      snprintf(text, sizeof(text),
               "`%s` calls `%s` here, which is outside the program and "
               "declares no effects, so it may perform '%s'; declare it "
               "`extern fn %s(...) with none` if it does not",
               name, insn->text, effect, insn->text);
    } else if (insn->op == IR_OP_CALL && insn->text) {
      snprintf(text, sizeof(text), "`%s` calls `%s` here, which performs '%s'",
               name, display_name(insn->text), effect);
    } else {
      snprintf(text, sizeof(text), "`%s` performs '%s' here", name, effect);
    }
  }
  error_reporter_add_note_of_span(ctx->reporter, instruction_span(efn, site),
                                  text);
}

static void report(Ctx *ctx, SourceSpan span, const char *message,
                   const char *label, const char *code) {
  ctx->errors++;
  if (!ctx->reporter) {
    fprintf(stderr, "error[%s]: %s\n", code, message);
    return;
  }
  error_reporter_add_error_with_span(ctx->reporter, ERROR_SEMANTIC, span,
                                     message);
  if (label) {
    error_reporter_set_last_label(ctx->reporter, label);
  }
  error_reporter_set_last_code(ctx->reporter, code);
}

static int check_forbids(Ctx *ctx) {
  for (size_t i = 0; i < ctx->fn_count; i++) {
    EFn *efn = &ctx->fns[i];
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      Hop *hops = NULL;
      size_t length = 0;
      char chain[512];
      char message[768];
      const char *name = display_name(efn->fn->name);
      int unknown = bit + 1 == ctx->bit_count;
      int forbidden = 0;
      if (unknown) {
        forbidden = words_any(efn->forbids, ctx->words) &&
                    bit_test(efn->performs, bit);
      } else {
        forbidden = bit_test(efn->forbids, bit) && bit_test(efn->performs, bit);
      }
      if (!forbidden) {
        continue;
      }
      if (!trace_chain(ctx, i, bit, 0, &hops, &length)) {
        return 0;
      }
      describe_chain(chain, sizeof(chain), ctx, hops, length);
      if (unknown) {
        snprintf(message, sizeof(message),
                 "'%s' forbids an effect but reaches a call the compiler "
                 "cannot follow: %s",
                 name, chain);
      } else if (length <= 1) {
        snprintf(message, sizeof(message), "'%s' forbids '%s' and performs it",
                 name, effect_name(ctx, bit));
      } else {
        snprintf(message, sizeof(message),
                 "'%s' forbids '%s' but reaches it: %s", name,
                 effect_name(ctx, bit), chain);
      }
      report(ctx, function_name_span(ctx, efn->fn),
             message, "the forbidding function", "F0001");
      note_hops(ctx, hops, length);
      if (length > 0) {
        note_source_site(ctx, &ctx->fns[hops[length - 1].function], bit);
      }
      free(hops);
    }
  }
  return 1;
}

static int is_root(Ctx *ctx, const EFn *efn) {
  const IRFunction *fn = efn->fn;
  if (fn->instruction_count == 0 || fn->is_rule) {
    return 0;
  }
  if (fn->name && strcmp(fn->name, "main") == 0) {
    return 1;
  }
  if (fn->is_interrupt || fn->is_naked || fn->is_kernel || fn->is_test) {
    return 1;
  }
  return ctx->input->library_build && fn->is_exported;
}

static const char *root_reason(const IRFunction *fn) {
  if (fn->name && strcmp(fn->name, "main") == 0) {
    return "the program starts here with nothing provided";
  }
  if (fn->is_interrupt) {
    return "the CPU enters an interrupt handler with nothing provided";
  }
  if (fn->is_naked) {
    return "a naked function is entered from outside with nothing provided";
  }
  if (fn->is_kernel) {
    return "a kernel is launched with nothing provided";
  }
  if (fn->is_test) {
    return "a test runs with nothing provided";
  }
  return "a shared library's export is called from outside with nothing "
         "provided";
}

static int check_roots(Ctx *ctx) {
  for (size_t i = 0; i < ctx->fn_count; i++) {
    EFn *efn = &ctx->fns[i];
    if (!is_root(ctx, efn)) {
      continue;
    }
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      Hop *hops = NULL;
      size_t length = 0;
      char chain[512];
      char message[768];
      const char *name = display_name(efn->fn->name);
      if (!bit_test(efn->needs, bit)) {
        continue;
      }
      if (!trace_chain(ctx, i, bit, 1, &hops, &length)) {
        return 0;
      }
      describe_chain(chain, sizeof(chain), ctx, hops, length);
      if (length <= 1) {
        snprintf(message, sizeof(message),
                 "'%s' requires '%s' and nothing provides it: %s", name,
                 effect_name(ctx, bit), root_reason(efn->fn));
      } else {
        snprintf(message, sizeof(message),
                 "'%s' reaches a function that requires '%s', and nothing on "
                 "the way provides it: %s",
                 name, effect_name(ctx, bit), chain);
      }
      report(ctx, function_name_span(ctx, efn->fn),
             message, root_reason(efn->fn), "F0002");
      note_hops(ctx, hops, length);
      if (length > 0 && ctx->reporter) {
        EFn *last = &ctx->fns[hops[length - 1].function];
        char text[256];
        snprintf(text, sizeof(text), "`%s` is declared `requires %s`; a "
                                     "caller must provide it: `fn f() "
                                     "provides %s`",
                 display_name(last->fn->name), effect_name(ctx, bit),
                 effect_name(ctx, bit));
        error_reporter_add_note_of_span(ctx->reporter, function_span(last->fn),
                                        text);
      }
      free(hops);
    }
  }
  return 1;
}

static int check_obligations(Ctx *ctx) {
  for (size_t o = 0; o < ctx->input->obligation_count; o++) {
    const IREffectObligation *ob = &ctx->input->obligations[o];
    EFn *efn = find_fn(ctx, ob->function);
    Word *with;
    Word *requires;
    int closed = 0;
    size_t index;
    if (!efn) {
      continue;
    }
    index = (size_t)(efn - ctx->fns);
    with = words_new(ctx);
    requires = words_new(ctx);
    if (!with || !requires ||
        !parse_signature(ctx, ob->signature, with, &closed, requires)) {
      free(with);
      free(requires);
      return 0;
    }
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      Hop *hops = NULL;
      size_t length = 0;
      char chain[512];
      char message[768];
      const char *name = display_name(efn->fn->name);
      if (closed && bit_test(efn->performs, bit) && !bit_test(with, bit)) {
        if (!trace_chain(ctx, index, bit, 0, &hops, &length)) {
          free(with);
          free(requires);
          return 0;
        }
        describe_chain(chain, sizeof(chain), ctx, hops, length);
        if (bit + 1 == ctx->bit_count) {
          snprintf(message, sizeof(message),
                   "'%s' is handed to a type declaring `%s`, but it calls "
                   "through a function type the compiler cannot follow: %s",
                   name, ob->signature, chain);
        } else {
          snprintf(message, sizeof(message),
                   "'%s' is handed to a type declaring `%s`, but it may "
                   "perform '%s': %s",
                   name, ob->signature, effect_name(ctx, bit), chain);
        }
        report(ctx, source_span_from_location(ob->location, strlen(name)),
               message, "its effects do not fit the type", "F0003");
        note_hops(ctx, hops, length);
        if (length > 0) {
          note_source_site(ctx, &ctx->fns[hops[length - 1].function], bit);
        }
        free(hops);
      }
      if (bit + 1 < ctx->bit_count && bit_test(efn->needs, bit) &&
          !bit_test(requires, bit)) {
        if (!trace_chain(ctx, index, bit, 1, &hops, &length)) {
          free(with);
          free(requires);
          return 0;
        }
        describe_chain(chain, sizeof(chain), ctx, hops, length);
        snprintf(message, sizeof(message),
                 "'%s' requires '%s', and the type it is handed to does not "
                 "declare `requires %s`, so whoever calls through it would not "
                 "know to provide it: %s",
                 name, effect_name(ctx, bit), effect_name(ctx, bit), chain);
        if (strcmp(ob->signature, "open") == 0) {
          snprintf(message, sizeof(message),
                   "'%s' requires '%s', and it is handed to a function type "
                   "with no `requires` clause, so whoever calls through it "
                   "would not know to provide it; write `fn(...) requires "
                   "%s` on the type: %s",
                   name, effect_name(ctx, bit), effect_name(ctx, bit), chain);
        }
        report(ctx, source_span_from_location(ob->location, strlen(name)),
               message, "its requirements do not fit the type", "F0003");
        note_hops(ctx, hops, length);
        free(hops);
      }
    }
    free(with);
    free(requires);
  }
  return 1;
}

static int words_meet(const Word *a, const Word *b, size_t words) {
  for (size_t i = 0; i < words; i++) {
    if (a[i] & b[i]) {
      return 1;
    }
  }
  return 0;
}

static size_t first_placed_bit(Ctx *ctx, const Word *needs,
                               const Word *placed) {
  for (size_t bit = 0; bit + 1 < ctx->bit_count; bit++) {
    if (bit_test(needs, bit) && bit_test(placed, bit)) {
      return bit;
    }
  }
  return ctx->bit_count;
}

static int check_races(Ctx *ctx) {
  Word *placed = words_new(ctx);
  if (!placed) {
    return 0;
  }
  for (size_t i = 0; i < ctx->fn_count; i++) {
    for (size_t w = 0; w < ctx->words; w++) {
      placed[w] |= ctx->fns[i].provides[w];
    }
  }
  if (!words_any(placed, ctx->words)) {
    free(placed);
    return 1;
  }
  for (size_t g = 0; g < ctx->global_count; g++) {
    EGlobal *entry = &ctx->globals[g];
    for (size_t a = 0; a + 1 < entry->writer_count; a++) {
      EFn *left = &ctx->fns[entry->writers[a]];
      size_t left_bit = first_placed_bit(ctx, left->needs, placed);
      if (left_bit == ctx->bit_count) {
        continue;
      }
      for (size_t b = a + 1; b < entry->writer_count; b++) {
        EFn *right = &ctx->fns[entry->writers[b]];
        size_t right_bit = first_placed_bit(ctx, right->needs, placed);
        char message[768];
        if (right_bit == ctx->bit_count ||
            words_meet(left->needs, right->needs, ctx->words)) {
          continue;
        }
        snprintf(message, sizeof(message),
                 "'%s' is written by '%s', which runs where '%s' is provided, "
                 "and by '%s', which runs where '%s' is provided, and nothing "
                 "either one needs orders the two writes",
                 display_name(entry->name), display_name(right->fn->name),
                 effect_name(ctx, right_bit), display_name(left->fn->name),
                 effect_name(ctx, left_bit));
        report(ctx, instruction_span(right, entry->sites[b]), message,
               "two threads write this", "F0006");
        if (ctx->reporter) {
          char note[320];
          snprintf(note, sizeof(note),
                   "`%s` writes `%s` here, and it requires `%s`",
                   display_name(left->fn->name), display_name(entry->name),
                   effect_name(ctx, left_bit));
          error_reporter_add_note_of_span(
              ctx->reporter, instruction_span(left, entry->sites[a]), note);
          snprintf(note, sizeof(note),
                   "an effect both writers require would order them: give "
                   "each `requires <lock>` and provide it around both");
          error_reporter_add_note_of_span(
              ctx->reporter, function_span(right->fn), note);
        }
      }
    }
  }
  free(placed);
  return 1;
}

static int emit_helper_call(IRFunction *fn, size_t index, const char *helper,
                            const char *const *texts, size_t text_count,
                            SourceLocation location) {
  IRInstruction call = {0};
  int ok;
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = (char *)helper;
  call.argument_count = text_count;
  call.arguments = calloc(text_count ? text_count : 1, sizeof(IROperand));
  if (!call.arguments) {
    return 0;
  }
  for (size_t i = 0; i < text_count; i++) {
    call.arguments[i] = ir_operand_string(texts[i]);
  }
  ok = ir_function_insert_instruction(fn, index, &call);
  for (size_t i = 0; i < text_count; i++) {
    ir_operand_destroy(&call.arguments[i]);
  }
  free(call.arguments);
  return ok;
}

static void join_names(char *buffer, size_t size, const char **names,
                       size_t count) {
  size_t used = 0;
  buffer[0] = '\0';
  for (size_t i = 0; i < count && used + 2 < size; i++) {
    int wrote = snprintf(buffer + used, size - used, "%s%s", i ? "," : "",
                         names[i]);
    if (wrote < 0) {
      break;
    }
    used += (size_t)wrote;
  }
}

static size_t entry_index(const IRFunction *fn) {
  size_t i = 0;
  while (i < fn->instruction_count &&
         (fn->instructions[i].op == IR_OP_LABEL ||
          fn->instructions[i].op == IR_OP_NOP ||
          fn->instructions[i].op == IR_OP_DECLARE_LOCAL)) {
    i++;
  }
  return i;
}

static int instrument_function(Ctx *ctx, EFn *efn) {
  IRFunction *fn = efn->fn;
  const char *name = display_name(fn->name);
  int alloc_bit = effect_bit(ctx, "alloc");
  int asm_bit = effect_bit(ctx, "asm");
  int syscall_bit = effect_bit(ctx, "syscall");
  int framed = fn->effects_with_count || fn->effects_forbids_count ||
               fn->effects_requires_count || fn->effects_provides_count;
  if (fn->instruction_count == 0) {
    return 1;
  }
  for (size_t i = fn->instruction_count; i > 0; i--) {
    const IRInstruction *insn = &fn->instructions[i - 1];
    const char *effect = NULL;
    const char *texts[2];
    if (insn->op == IR_OP_NEW || insn->allocates ||
        (insn->op == IR_OP_CALL && insn->text &&
         ir_effects_name_is_allocator(insn->text))) {
      effect = alloc_bit >= 0 ? "alloc" : NULL;
    } else if (insn->op == IR_OP_INLINE_ASM) {
      effect = asm_bit >= 0 ? "asm" : NULL;
    } else if (insn->op == IR_OP_CALL && insn->text &&
               strcmp(insn->text, IR_SYSCALL_CALL_NAME) == 0) {
      effect = syscall_bit >= 0 ? "syscall" : NULL;
    }
    if (!effect) {
      continue;
    }
    texts[0] = effect;
    texts[1] = name;
    if (!emit_helper_call(fn, i - 1, "mettle_effects_perform", texts, 2,
                          insn->location)) {
      return 0;
    }
  }
  if (!framed) {
    return 1;
  }
  for (size_t i = fn->instruction_count; i > 0; i--) {
    if (fn->instructions[i - 1].op == IR_OP_RETURN &&
        !emit_helper_call(fn, i - 1, "mettle_effects_leave", NULL, 0,
                          fn->instructions[i - 1].location)) {
      return 0;
    }
  }
  {
    char with[512];
    char forbids[512];
    char requires[512];
    char provides[512];
    const char *texts[5];
    join_names(with, sizeof(with), fn->effects_with, fn->effects_with_count);
    join_names(forbids, sizeof(forbids), fn->effects_forbids,
               fn->effects_forbids_count);
    join_names(requires, sizeof(requires), fn->effects_requires,
               fn->effects_requires_count);
    join_names(provides, sizeof(provides), fn->effects_provides,
               fn->effects_provides_count);
    texts[0] = name;
    texts[1] = with;
    texts[2] = forbids;
    texts[3] = requires;
    texts[4] = provides;
    return emit_helper_call(fn, entry_index(fn), "mettle_effects_enter", texts,
                            5, fn->location);
  }
}

static void register_helper(IRProgram *program, const char *name,
                            size_t cstring_params) {
  IRModuleSymbol entry;
  MtlcType *cstring = ir_program_lookup_type(program, "cstring");
  MtlcType *params[5];
  if (ir_program_lookup_symbol(program, name)) {
    return;
  }
  memset(&entry, 0, sizeof(entry));
  entry.name = (char *)name;
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.has_body = 0;
  entry.return_type = ir_program_lookup_type(program, "void");
  if (cstring && cstring_params > 0) {
    for (size_t i = 0; i < cstring_params && i < 5; i++) {
      params[i] = cstring;
    }
    entry.param_types = params;
    entry.param_count = cstring_params;
  }
  ir_program_add_symbol(program, &entry);
}

static int instrument(Ctx *ctx) {
  register_helper(ctx->program, "mettle_effects_enter", 5);
  register_helper(ctx->program, "mettle_effects_leave", 0);
  register_helper(ctx->program, "mettle_effects_perform", 2);
  for (size_t i = 0; i < ctx->fn_count; i++) {
    if (ctx->fns[i].fn->is_rule || ctx->fns[i].fn->rewrite_role) {
      continue;
    }
    if (!instrument_function(ctx, &ctx->fns[i])) {
      return 0;
    }
  }
  return 1;
}

static IREffectResults *collect_results(Ctx *ctx) {
  IREffectResults *results = calloc(1, sizeof(IREffectResults));
  if (!results) {
    return NULL;
  }
  results->count = ctx->fn_count;
  results->names = calloc(ctx->fn_count ? ctx->fn_count : 1, sizeof(char *));
  results->performs =
      calloc(ctx->fn_count ? ctx->fn_count : 1, sizeof(const char **));
  results->perform_counts =
      calloc(ctx->fn_count ? ctx->fn_count : 1, sizeof(size_t));
  results->needs =
      calloc(ctx->fn_count ? ctx->fn_count : 1, sizeof(const char **));
  results->need_counts =
      calloc(ctx->fn_count ? ctx->fn_count : 1, sizeof(size_t));
  if (!results->names || !results->performs || !results->perform_counts ||
      !results->needs || !results->need_counts) {
    ir_effect_results_free(results);
    return NULL;
  }
  for (size_t i = 0; i < ctx->fn_count; i++) {
    EFn *efn = &ctx->fns[i];
    const char **performs = calloc(ctx->bit_count, sizeof(const char *));
    const char **needs = calloc(ctx->bit_count, sizeof(const char *));
    size_t perform_count = 0;
    size_t need_count = 0;
    if (!performs || !needs) {
      free(performs);
      free(needs);
      ir_effect_results_free(results);
      return NULL;
    }
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      if (bit_test(efn->performs, bit)) {
        performs[perform_count++] = mettle_strdup(effect_name(ctx, bit));
      }
      if (bit_test(efn->needs, bit)) {
        needs[need_count++] = mettle_strdup(effect_name(ctx, bit));
      }
    }
    results->names[i] = efn->fn->name ? strdup(efn->fn->name) : strdup("");
    results->performs[i] = performs;
    results->perform_counts[i] = perform_count;
    results->needs[i] = needs;
    results->need_counts[i] = need_count;
  }
  return results;
}

void ir_effect_results_free(IREffectResults *results) {
  if (!results) {
    return;
  }
  for (size_t i = 0; i < results->count; i++) {
    free(results->names ? results->names[i] : NULL);
    if (results->performs && results->performs[i]) {
      for (size_t k = 0; k < results->perform_counts[i]; k++) {
        free((char *)results->performs[i][k]);
      }
      free((void *)results->performs[i]);
    }
    if (results->needs && results->needs[i]) {
      for (size_t k = 0; k < results->need_counts[i]; k++) {
        free((char *)results->needs[i][k]);
      }
      free((void *)results->needs[i]);
    }
  }
  free(results->names);
  free(results->performs);
  free(results->perform_counts);
  free(results->needs);
  free(results->need_counts);
  free(results);
}

int ir_effect_results_lookup(const IREffectResults *results,
                             const char *function, const char ***performs,
                             size_t *perform_count, const char ***needs,
                             size_t *need_count) {
  if (!results || !function) {
    return 0;
  }
  for (size_t i = 0; i < results->count; i++) {
    if (results->names[i] && strcmp(results->names[i], function) == 0) {
      *performs = results->performs[i];
      *perform_count = results->perform_counts[i];
      *needs = results->needs[i];
      *need_count = results->need_counts[i];
      return 1;
    }
  }
  return 0;
}

static void ctx_free(Ctx *ctx) {
  for (size_t i = 0; i < ctx->fn_count; i++) {
    EFn *efn = &ctx->fns[i];
    free(efn->sources);
    free(efn->performs);
    free(efn->needs);
    free(efn->with);
    free(efn->forbids);
    free(efn->requires);
    free(efn->provides);
    free(efn->callees);
    free(efn->callee_sites);
    free(efn->source_sites);
    free(efn->indirect_sites);
    for (size_t k = 0; k < efn->indirect_count; k++) {
      free(efn->indirect_requires[k]);
    }
    free(efn->indirect_requires);
  }
  for (size_t i = 0; i < ctx->global_count; i++) {
    free(ctx->globals[i].writers);
    free(ctx->globals[i].sites);
  }
  for (size_t i = 0; i < ctx->owned_count; i++) {
    free(ctx->owned[i]);
  }
  free(ctx->owned);
  free(ctx->globals);
  free(ctx->fns);
  free(ctx->index);
}

static int why_effect(Ctx *ctx, const char *function, const char *effect,
                      FILE *out) {
  EFn *efn = find_fn(ctx, function);
  int bit = effect_bit(ctx, effect);
  Hop *hops = NULL;
  size_t length = 0;
  if (!efn) {
    for (size_t i = 0; i < ctx->fn_count; i++) {
      if (ctx->fns[i].fn && ctx->fns[i].fn->name &&
          strcmp(display_name(ctx->fns[i].fn->name), function) == 0) {
        efn = &ctx->fns[i];
        break;
      }
    }
  }
  if (!efn) {
    fprintf(out, "`%s` is not a function this program defines\n", function);
    return 0;
  }
  if (bit < 0) {
    fprintf(out, "`%s` is not an effect this program declares\n", effect);
    return 0;
  }
  if (!bit_test(efn->performs, (size_t)bit) &&
      !bit_test(efn->needs, (size_t)bit)) {
    fprintf(out,
            "`%s` neither performs nor needs `%s`. The pass walked every "
            "direct call it can reach and found no source.\n",
            function, effect);
    return 1;
  }
  if (bit_test(efn->performs, (size_t)bit)) {
    fprintf(out, "`%s` performs `%s`.\n", function, effect);
    if (!trace_chain(ctx, (size_t)(efn - ctx->fns), (size_t)bit, 0, &hops,
                     &length)) {
      return 0;
    }
    if (length == 0) {
      fprintf(out, "  the chain is empty, which means the pass lost it\n");
    } else {
      for (size_t i = 0; i < length; i++) {
        const char *name = display_name(ctx->fns[hops[i].function].fn->name);
        if (i == 0) {
          fprintf(out, "  %s\n", name);
        } else {
          const EFn *caller = &ctx->fns[hops[i - 1].function];
          SourceSpan span = instruction_span(caller, hops[i].via_site);
          fprintf(out, "  calls %s at %zu:%zu\n", name, span.line,
                  span.column);
        }
      }
      {
        EFn *last = &ctx->fns[hops[length - 1].function];
        size_t site = last->source_sites[bit];
        if (site == NO_SITE) {
          fprintf(out, "  and `%s` is declared `with %s`\n",
                  display_name(last->fn->name), effect);
        } else {
          SourceSpan span = instruction_span(last, site);
          fprintf(out, "  and performs it at %zu:%zu\n", span.line,
                  span.column);
        }
      }
    }
    free(hops);
  }
  if (bit_test(efn->needs, (size_t)bit)) {
    fprintf(out, "`%s` needs `%s`.\n", function, effect);
    hops = NULL;
    length = 0;
    if (!trace_chain(ctx, (size_t)(efn - ctx->fns), (size_t)bit, 1, &hops,
                     &length)) {
      return 0;
    }
    for (size_t i = 0; i < length; i++) {
      const char *name = display_name(ctx->fns[hops[i].function].fn->name);
      if (i == 0) {
        fprintf(out, "  %s\n", name);
      } else {
        const EFn *caller = &ctx->fns[hops[i - 1].function];
        SourceSpan span = instruction_span(caller, hops[i].via_site);
        fprintf(out, "  calls %s at %zu:%zu\n", name, span.line,
                span.column);
      }
    }
    if (length > 0) {
      fprintf(out, "  and `%s` is declared `requires %s`\n",
              display_name(ctx->fns[hops[length - 1].function].fn->name),
              effect);
    }
    free(hops);
  }
  fprintf(out,
          "This is the same chain the build would print if something "
          "forbade it.\n");
  return 1;
}

static void report_effects(Ctx *ctx, FILE *out) {
  size_t declared = 0;
  for (size_t i = 0; i < ctx->fn_count; i++) {
    EFn *efn = &ctx->fns[i];
    char performs[512];
    char needs[512];
    size_t used = 0;
    size_t need_used = 0;
    if (!efn->fn || !efn->fn->name) {
      continue;
    }
    performs[0] = '\0';
    needs[0] = '\0';
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      if (bit_test(efn->performs, bit)) {
        int wrote = snprintf(performs + used, sizeof(performs) - used, "%s%s",
                             used ? ", " : "", effect_name(ctx, bit));
        if (wrote > 0) {
          used += (size_t)wrote;
        }
      }
      if (bit_test(efn->needs, bit)) {
        int wrote = snprintf(needs + need_used, sizeof(needs) - need_used,
                             "%s%s", need_used ? ", " : "",
                             effect_name(ctx, bit));
        if (wrote > 0) {
          need_used += (size_t)wrote;
        }
      }
    }
    if (!used && !need_used) {
      continue;
    }
    declared++;
    fprintf(out, "effects %s: performs %s, needs %s\n",
            display_name(efn->fn->name), used ? performs : "nothing",
            need_used ? needs : "nothing");
  }
  {
    Word *placed = words_new(ctx);
    size_t shared = 0;
    size_t ordered = 0;
    for (size_t i = 0; placed && i < ctx->fn_count; i++) {
      for (size_t w = 0; w < ctx->words; w++) {
        placed[w] |= ctx->fns[i].provides[w];
      }
    }
    for (size_t g = 0; placed && g < ctx->global_count; g++) {
      EGlobal *entry = &ctx->globals[g];
      char writers[512];
      size_t used = 0;
      size_t counted = 0;
      int covered = 1;
      for (size_t a = 0; a < entry->writer_count; a++) {
        EFn *writer = &ctx->fns[entry->writers[a]];
        size_t bit = first_placed_bit(ctx, writer->needs, placed);
        int wrote;
        if (bit == ctx->bit_count) {
          continue;
        }
        counted++;
        wrote = snprintf(writers + used, sizeof(writers) - used, "%s%s (%s)",
                         used ? ", " : "", display_name(writer->fn->name),
                         effect_name(ctx, bit));
        if (wrote > 0) {
          used += (size_t)wrote;
        }
      }
      if (counted < 2) {
        continue;
      }
      shared++;
      for (size_t a = 0; a + 1 < entry->writer_count && covered; a++) {
        for (size_t b = a + 1; b < entry->writer_count && covered; b++) {
          EFn *left = &ctx->fns[entry->writers[a]];
          EFn *right = &ctx->fns[entry->writers[b]];
          if (first_placed_bit(ctx, left->needs, placed) == ctx->bit_count ||
              first_placed_bit(ctx, right->needs, placed) == ctx->bit_count) {
            continue;
          }
          covered = words_meet(left->needs, right->needs, ctx->words);
        }
      }
      ordered += covered ? 1 : 0;
      fprintf(out, "shared %s: %s, %s\n", display_name(entry->name), writers,
              covered ? "ordered by an effect both require"
                      : "nothing orders the writes");
    }
    fprintf(out, "shared globals: %zu written from more than one place, %zu "
                 "ordered\n",
            shared, ordered);
    free(placed);
  }
  fprintf(out,
          "effects: %zu functions, %zu with an effect, %zu fixpoint rounds, "
          "%lld steps\n",
          ctx->fn_count, declared, ctx->rounds, ctx->steps);
}

static void ledger_effects(Ctx *ctx) {
  for (size_t i = 0; i < ctx->fn_count; i++) {
    EFn *efn = &ctx->fns[i];
    char performs[256];
    char needs[256];
    size_t used = 0;
    size_t need_used = 0;
    if (!efn->fn || !efn->fn->name) {
      continue;
    }
    performs[0] = '\0';
    needs[0] = '\0';
    for (size_t bit = 0; bit < ctx->bit_count; bit++) {
      if (bit_test(efn->performs, bit)) {
        int wrote = snprintf(performs + used, sizeof(performs) - used, "%s%s",
                             used ? ", " : "", effect_name(ctx, bit));
        if (wrote > 0) {
          used += (size_t)wrote;
        }
      }
      if (bit_test(efn->needs, bit)) {
        int wrote = snprintf(needs + need_used, sizeof(needs) - need_used,
                             "%s%s", need_used ? ", " : "",
                             effect_name(ctx, bit));
        if (wrote > 0) {
          need_used += (size_t)wrote;
        }
      }
    }
    if (!used && !need_used) {
      continue;
    }
    ir_explain_effect_held(display_name(efn->fn->name),
                           used ? performs : NULL,
                           need_used ? needs : NULL);
  }
}

int ir_effects_run(IRProgram *program, const IREffectInput *input,
                   ErrorReporter *reporter, IREffectResults **out_results,
                   long long *out_steps) {
  Ctx ctx;
  int ok = 1;
  if (out_results) {
    *out_results = NULL;
  }
  if (!program || !input) {
    return 0;
  }
  memset(&ctx, 0, sizeof(ctx));
  ctx.program = program;
  ctx.input = input;
  ctx.reporter = reporter;
  ctx.bit_count = input->effect_count + 1;
  ctx.words = (ctx.bit_count + 63) / 64;
  ctx.fn_count = program->function_count;
  ctx.fns = calloc(ctx.fn_count ? ctx.fn_count : 1, sizeof(EFn));
  ctx.index = calloc(ctx.fn_count ? ctx.fn_count : 1, sizeof(NameEntry));
  if (!ctx.fns || !ctx.index) {
    ctx_free(&ctx);
    return 0;
  }
  for (size_t i = 0; i < ctx.fn_count; i++) {
    EFn *efn = &ctx.fns[i];
    efn->fn = program->functions[i];
    efn->sources = words_new(&ctx);
    efn->performs = words_new(&ctx);
    efn->needs = words_new(&ctx);
    efn->with = words_new(&ctx);
    efn->forbids = words_new(&ctx);
    efn->requires = words_new(&ctx);
    efn->provides = words_new(&ctx);
    efn->source_sites = malloc(ctx.bit_count * sizeof(size_t));
    if (!efn->sources || !efn->performs || !efn->needs || !efn->with ||
        !efn->forbids || !efn->requires || !efn->provides ||
        !efn->source_sites) {
      ctx_free(&ctx);
      return 0;
    }
    for (size_t bit = 0; bit < ctx.bit_count; bit++) {
      efn->source_sites[bit] = NO_SITE;
    }
    if (efn->fn && efn->fn->name) {
      ctx.index[ctx.index_count].name = efn->fn->name;
      ctx.index[ctx.index_count].index = i;
      ctx.index_count++;
    }
  }
  qsort(ctx.index, ctx.index_count, sizeof(NameEntry), name_entry_compare);
  for (size_t i = 0; i < ctx.fn_count && ok; i++) {
    if (!ctx.fns[i].fn) {
      continue;
    }
    ok = scan_function(&ctx, i) && scan_globals(&ctx, i);
  }
  if (ok) {
    propagate(&ctx);
    ok = !ctx.failed;
  }
  if (ok && !getenv("METTLE_TRUST_EFFECTS")) {
    ok = check_forbids(&ctx) && check_roots(&ctx) && check_obligations(&ctx) &&
         check_races(&ctx);
  }
  if (ok && ctx.errors == 0 && input->instrument) {
    ok = instrument(&ctx);
  }
  if (ok && out_results) {
    *out_results = collect_results(&ctx);
    ok = *out_results != NULL;
  }
  if (ok && ctx.errors > 0) {
    ok = 0;
  }
  if (input->report) {
    report_effects(&ctx, input->report);
  }
  if (input->why_out && input->why_function && input->why_effect) {
    ok = why_effect(&ctx, input->why_function, input->why_effect,
                    input->why_out);
  }
  ledger_effects(&ctx);
  if (out_steps) {
    *out_steps = ctx.steps;
  }
  ctx_free(&ctx);
  return ok;
}
