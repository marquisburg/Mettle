#include "ir_optimize_internal.h"
#include "../ir_optimize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Whole-program alias facts                                                    */
/*                                                                              */
/* Inside one function the redundancy pass already knows that `&a` and `&b`     */
/* name different variables and therefore never overlap. What it loses is that  */
/* fact crossing a call: both arguments arrive as opaque parameters, so a store  */
/* through one is assumed to reach anything read through the other. This pass   */
/* carries the fact across. A whole-program compile can see every call site, so  */
/* the set of allocations a parameter can hold is enumerable.                    */
/*                                                                              */
/* A root is one allocation the program can name: the address of a declared     */
/* variable, or one call site of an allocator (every execution of a `malloc`    */
/* returns storage distinct from every variable, which is all this needs). A    */
/* parameter's root set is the union, over every call site, of the roots its    */
/* argument can carry. Arguments are usually forwarded parameters, so the sets  */
/* are computed to a fixpoint.                                                  */
/*                                                                              */
/* Anything the walk cannot name poisons a set to "unknown", and an unknown set  */
/* proves nothing: a loaded pointer, arithmetic on an unresolved base, a call    */
/* the compiler cannot resolve, more roots than the set holds. A function        */
/* reachable from outside the program starts poisoned, because its callers are   */
/* not part of this compile: `export`, `main`, anything whose address is taken,  */
/* and every function in a program that calls through a pointer.                */
/*                                                                              */
/* Two parameters are distinct when both sets are known and disjoint. The same   */
/* comparison answers a parameter against a local variable's address, which is   */
/* what lets a caller's out-parameter stop killing a callee's cached fields.     */
/* -------------------------------------------------------------------------- */

#define IR_ALIAS_MAX_ROOTS 8
#define IR_ALIAS_MAX_DEPTH 6
#define IR_ALIAS_MAX_ROUNDS 8

typedef struct {
  int unknown;
  int count;
  int roots[IR_ALIAS_MAX_ROOTS]; /* ascending */
} IRAliasRootSet;

typedef struct {
  IRAliasRootSet *params;
  size_t param_count;
  int poisoned; /* callers outside the program */
} IRAliasFunction;

typedef struct {
  IRProgram *program;
  IRAliasFunction *functions;
  size_t function_count;
  /* Callee pointer -> its slot, open addressed. The propagation rounds resolve
   * one slot per call site per round; a scan of every function there is
   * quadratic in a program that is mostly functions. */
  const IRFunction **slot_keys;
  size_t *slot_values;
  size_t slot_count;
  /* Function name -> every slot holding it, open addressed with one entry per
   * function so a repeated name simply occupies several probes. Poisoning
   * consults this once per symbol operand in the program; scanning every
   * function there is quadratic in a program that is mostly functions. */
  const char **name_keys;
  size_t *name_values;
  size_t name_count;
  char **root_names;
  size_t root_count;
  size_t root_capacity;
  int valid;
} IRAliasFacts;

static IRAliasFacts g_alias;

/* ------------------------------------------------------------ root interning */

static int alias_root_intern(IRAliasFacts *facts, const char *owner,
                             const char *what) {
  char key[256];
  if (!owner || !what) {
    return -1;
  }
  if (snprintf(key, sizeof(key), "%s#%s", owner, what) >= (int)sizeof(key)) {
    return -1;
  }
  for (size_t i = 0; i < facts->root_count; i++) {
    if (strcmp(facts->root_names[i], key) == 0) {
      return (int)i;
    }
  }
  if (facts->root_count == facts->root_capacity) {
    size_t capacity = facts->root_capacity ? facts->root_capacity * 2 : 64;
    char **grown = realloc(facts->root_names, capacity * sizeof(char *));
    if (!grown) {
      return -1;
    }
    facts->root_names = grown;
    facts->root_capacity = capacity;
  }
  facts->root_names[facts->root_count] = mettle_strdup(key);
  if (!facts->root_names[facts->root_count]) {
    return -1;
  }
  return (int)facts->root_count++;
}

/* ---------------------------------------------------------------- root sets */

static void alias_set_poison(IRAliasRootSet *set) {
  set->unknown = 1;
  set->count = 0;
}

static int alias_set_add(IRAliasRootSet *set, int root) {
  if (set->unknown) {
    return 1;
  }
  if (root < 0) {
    alias_set_poison(set);
    return 1;
  }
  for (int i = 0; i < set->count; i++) {
    if (set->roots[i] == root) {
      return 0;
    }
    if (set->roots[i] > root) {
      if (set->count == IR_ALIAS_MAX_ROOTS) {
        alias_set_poison(set);
        return 1;
      }
      for (int k = set->count; k > i; k--) {
        set->roots[k] = set->roots[k - 1];
      }
      set->roots[i] = root;
      set->count++;
      return 1;
    }
  }
  if (set->count == IR_ALIAS_MAX_ROOTS) {
    alias_set_poison(set);
    return 1;
  }
  set->roots[set->count++] = root;
  return 1;
}

static int alias_set_merge(IRAliasRootSet *into, const IRAliasRootSet *from) {
  int changed = 0;
  if (into->unknown) {
    return 0;
  }
  if (from->unknown) {
    alias_set_poison(into);
    return 1;
  }
  for (int i = 0; i < from->count; i++) {
    changed |= alias_set_add(into, from->roots[i]);
  }
  return changed;
}

static int alias_sets_disjoint(const IRAliasRootSet *a,
                               const IRAliasRootSet *b) {
  if (a->unknown || b->unknown || a->count == 0 || b->count == 0) {
    return 0;
  }
  for (int i = 0, k = 0; i < a->count && k < b->count;) {
    if (a->roots[i] == b->roots[k]) {
      return 0;
    }
    if (a->roots[i] < b->roots[k]) {
      i++;
    } else {
      k++;
    }
  }
  return 1;
}

static int alias_set_contains(const IRAliasRootSet *set, int root) {
  for (int i = 0; i < set->count; i++) {
    if (set->roots[i] == root) {
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------- lookups */

static size_t alias_slot_hash(const IRFunction *function, size_t mask) {
  uintptr_t bits = (uintptr_t)function;
  bits ^= bits >> 33;
  bits *= (uintptr_t)0xff51afd7ed558ccdULL;
  bits ^= bits >> 33;
  return (size_t)bits & mask;
}

static void alias_slot_index_destroy(IRAliasFacts *facts) {
  free(facts->slot_keys);
  free(facts->slot_values);
  free(facts->name_keys);
  free(facts->name_values);
  facts->slot_keys = NULL;
  facts->slot_values = NULL;
  facts->name_keys = NULL;
  facts->name_values = NULL;
  facts->slot_count = 0;
  facts->name_count = 0;
}

static void alias_slot_index_build(IRAliasFacts *facts) {
  size_t want = 8;
  size_t mask;

  alias_slot_index_destroy(facts);
  while (want < facts->function_count * 2) {
    want *= 2;
  }
  facts->slot_keys = calloc(want, sizeof(*facts->slot_keys));
  facts->slot_values = calloc(want, sizeof(*facts->slot_values));
  if (!facts->slot_keys || !facts->slot_values) {
    alias_slot_index_destroy(facts);
    return; /* lookups fall back to the scan below */
  }
  facts->name_keys = calloc(want, sizeof(*facts->name_keys));
  facts->name_values = calloc(want, sizeof(*facts->name_values));
  if (!facts->name_keys || !facts->name_values) {
    alias_slot_index_destroy(facts);
    return;
  }
  facts->slot_count = want;
  facts->name_count = want;
  mask = want - 1;
  for (size_t f = 0; f < facts->function_count; f++) {
    const IRFunction *function = facts->program->functions[f];
    size_t i;
    if (!function) {
      continue;
    }
    i = alias_slot_hash(function, mask);
    while (facts->slot_keys[i]) {
      if (facts->slot_keys[i] == function) {
        break; /* first slot for a repeated pointer wins, as the scan did */
      }
      i = (i + 1) & mask;
    }
    if (!facts->slot_keys[i]) {
      facts->slot_keys[i] = function;
      facts->slot_values[i] = f;
    }
    if (!function->name) {
      continue;
    }
    /* One entry per function, never deduplicated: two functions sharing a name
     * must both be reachable, because the scan this replaces poisoned both. */
    i = mettle_fnv1a_hash(function->name) & mask;
    while (facts->name_keys[i]) {
      i = (i + 1) & mask;
    }
    facts->name_keys[i] = function->name;
    facts->name_values[i] = f;
  }
}

static size_t alias_function_slot(const IRAliasFacts *facts,
                                  const IRFunction *function) {
  if (facts->slot_count) {
    size_t mask = facts->slot_count - 1;
    size_t i = alias_slot_hash(function, mask);
    while (facts->slot_keys[i]) {
      if (facts->slot_keys[i] == function) {
        return facts->slot_values[i];
      }
      i = (i + 1) & mask;
    }
    return (size_t)-1;
  }
  for (size_t f = 0; f < facts->function_count; f++) {
    if (facts->program->functions[f] == function) {
      return f;
    }
  }
  return (size_t)-1;
}

static int alias_type_name_is_pointer(const IRProgram *program,
                                      const char *type_name) {
  size_t length;
  if (!type_name) {
    return 0;
  }
  length = strlen(type_name);
  if (length > 0 && type_name[length - 1] == '*') {
    return 1;
  }
  if (strcmp(type_name, "rawptr") == 0 || strcmp(type_name, "cstring") == 0) {
    return 1;
  }
  {
    MtlcType *type = ir_program_lookup_type(program, type_name);
    return type && type->kind == MTLC_TYPE_POINTER;
  }
}

static int alias_function_parameter_index(const IRFunction *function,
                                          const char *name) {
  if (!function || !name) {
    return -1;
  }
  for (size_t p = 0; p < function->parameter_count; p++) {
    if (function->parameter_names[p] &&
        strcmp(function->parameter_names[p], name) == 0) {
      return (int)p;
    }
  }
  return -1;
}

/* Allocators whose every call yields storage distinct from every variable and
 * from every other call. `realloc` is absent on purpose: it may hand back the
 * block it was given. */
static int alias_name_is_fresh_allocator(const char *name) {
  static const char *const fresh[] = {"malloc",   "calloc",
                                      "aligned_alloc", "_aligned_malloc",
                                      "strdup",   "_strdup",
                                      "mettle_heap_zeroed", NULL};
  for (size_t i = 0; fresh[i]; i++) {
    if (strcmp(name, fresh[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/* ---------------------------------------------------------- root resolution */

/* How many times this function writes `name` as a destination. A pointer
 * assigned once can be looked through; one reassigned in a loop cannot. */
/* Writers of every function, indexed by name.
 *
 * Asking who wrote a name by scanning the function is the shape this
 * repository has been retiring everywhere else, and the alias walk asks it for
 * every call-site argument on every propagation round. The IR does not move
 * while the facts are being built, so the answers are collected once per
 * function and read back from a table. Up to four writers are remembered; a
 * name written more often is not one the walk can settle anyway. */
#define ALIAS_DEF_MAX_WRITERS 4

typedef struct {
  const char *key; /* borrowed operand name, NULL when free */
  int kind;
  int count; /* > ALIAS_DEF_MAX_WRITERS means "more than it remembers" */
  int at[ALIAS_DEF_MAX_WRITERS];
} AliasDefSlot;

static AliasDefSlot **g_alias_def_tables;
static size_t *g_alias_def_masks;
static size_t g_alias_def_table_count;

static size_t alias_defs_probe(const char *name, int kind, size_t mask) {
  unsigned long long h = 1469598103934665603ULL ^ (unsigned long long)kind;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    h ^= (unsigned long long)*p;
    h *= 1099511628211ULL;
  }
  return (size_t)h & mask;
}

static void alias_defs_destroy(void) {
  for (size_t i = 0; i < g_alias_def_table_count; i++) {
    free(g_alias_def_tables[i]);
  }
  free(g_alias_def_tables);
  free(g_alias_def_masks);
  g_alias_def_tables = NULL;
  g_alias_def_masks = NULL;
  g_alias_def_table_count = 0;
}

/* The slot arrays are sized here; a function's own table is filled the first
 * time something asks about it. Most of a program's functions are never asked,
 * and indexing them all made a small compile pay for a large one. */
static void alias_defs_build_all(IRAliasFacts *facts) {
  IRProgram *program = facts->program;
  alias_defs_destroy();
  g_alias_def_tables = calloc(program->function_count,
                              sizeof(*g_alias_def_tables));
  g_alias_def_masks = calloc(program->function_count,
                             sizeof(*g_alias_def_masks));
  if (!g_alias_def_tables || !g_alias_def_masks) {
    alias_defs_destroy();
    return;
  }
  g_alias_def_table_count = program->function_count;
}

static AliasDefSlot *alias_defs_table_for(const IRFunction *function,
                                          size_t slot_index) {
  size_t want = 16;
  AliasDefSlot *table;
  if (g_alias_def_tables[slot_index]) {
    return g_alias_def_tables[slot_index];
  }
  while (want < function->instruction_count * 2) {
    want *= 2;
  }
  table = calloc(want, sizeof(AliasDefSlot));
  if (!table) {
    return NULL;
  }
  g_alias_def_tables[slot_index] = table;
  g_alias_def_masks[slot_index] = want - 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    size_t mask = want - 1;
    size_t at;
    if (ins->op == IR_OP_DECLARE_LOCAL || !ins->dest.name ||
        (ins->dest.kind != IR_OPERAND_TEMP &&
         ins->dest.kind != IR_OPERAND_SYMBOL)) {
      continue;
    }
    at = alias_defs_probe(ins->dest.name, (int)ins->dest.kind, mask);
    while (table[at].key &&
           !(table[at].kind == (int)ins->dest.kind &&
             strcmp(table[at].key, ins->dest.name) == 0)) {
      at = (at + 1) & mask;
    }
    if (!table[at].key) {
      table[at].key = ins->dest.name;
      table[at].kind = (int)ins->dest.kind;
    }
    if (table[at].count < ALIAS_DEF_MAX_WRITERS) {
      table[at].at[table[at].count] = (int)i;
    }
    table[at].count++;
  }
  return table;
}

static const AliasDefSlot *alias_defs_find(const IRFunction *function,
                                           IROperandKind kind,
                                           const char *name) {
  size_t slot_index;
  AliasDefSlot *table;
  size_t mask;
  size_t at;
  if (!g_alias_def_tables || !function || !name) {
    return NULL;
  }
  slot_index = alias_function_slot(&g_alias, function);
  if (slot_index == (size_t)-1 || slot_index >= g_alias_def_table_count) {
    return NULL;
  }
  table = alias_defs_table_for(function, slot_index);
  if (!table) {
    return NULL;
  }
  mask = g_alias_def_masks[slot_index];
  at = alias_defs_probe(name, (int)kind, mask);
  while (table[at].key) {
    if (table[at].kind == (int)kind && strcmp(table[at].key, name) == 0) {
      return &table[at];
    }
    at = (at + 1) & mask;
  }
  return NULL;
}

static int alias_symbol_def_count(const IRFunction *function,
                                  const char *name) {
  int count = 0;
  {
    const AliasDefSlot *slot =
        alias_defs_find(function, IR_OPERAND_SYMBOL, name);
    if (slot) {
      return slot->count;
    }
    if (g_alias_def_tables) {
      return 0; /* indexed, and this name writes nothing */
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      count++;
    }
  }
  return count;
}

static const IRInstruction *alias_unique_def(const IRFunction *function,
                                             IROperandKind kind,
                                             const char *name) {
  const IRInstruction *found = NULL;
  {
    const AliasDefSlot *slot = alias_defs_find(function, kind, name);
    if (slot) {
      return slot->count == 1 ? &function->instructions[slot->at[0]] : NULL;
    }
    if (g_alias_def_tables) {
      return NULL; /* indexed, and this name writes nothing */
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ins->dest.kind == kind && ins->dest.name &&
        strcmp(ins->dest.name, name) == 0) {
      if (found) {
        return NULL;
      }
      found = ins;
    }
  }
  return found;
}

static void alias_roots_of(IRAliasFacts *facts, IRFunction *owner,
                           const IROperand *operand, IRAliasRootSet *out,
                           int depth);

/* Which storage a name refers to. A local belongs to its function; a global is
 * one object no matter who spells its name, so it interns under a fixed owner
 * or two functions would each mint their own root for it and the sets would
 * look disjoint when they name the same memory. */
static const char *alias_symbol_owner(const IRFunction *function,
                                      const char *name) {
  if (ir_function_symbol_is_parameter(function, name) ||
      ir_function_local_declared_type(function, name) != NULL) {
    return function->name;
  }
  return "@global";
}

static void alias_roots_of_def(IRAliasFacts *facts, IRFunction *owner,
                               const IRInstruction *def, IRAliasRootSet *out,
                               int depth) {
  if (def->op == IR_OP_ADDRESS_OF && def->lhs.kind == IR_OPERAND_SYMBOL &&
      def->lhs.name) {
    alias_set_add(out,
                  alias_root_intern(facts,
                                    alias_symbol_owner(owner, def->lhs.name),
                                    def->lhs.name));
    return;
  }
  if (def->op == IR_OP_ASSIGN || def->op == IR_OP_CAST) {
    alias_roots_of(facts, owner, &def->lhs, out, depth + 1);
    return;
  }
  if ((def->op == IR_OP_CALL && def->text &&
       alias_name_is_fresh_allocator(def->text)) ||
      def->op == IR_OP_NEW) {
    /* One root per call site: two sites never collide, and one site compared
     * against itself stays "may alias", which is what a loop needs. */
    char site[64];
    snprintf(site, sizeof(site), "alloc@%zu",
             (size_t)(def - owner->instructions));
    alias_set_add(out, alias_root_intern(facts, owner->name, site));
    return;
  }
  alias_set_poison(out);
}

static void alias_roots_of(IRAliasFacts *facts, IRFunction *owner,
                           const IROperand *operand, IRAliasRootSet *out,
                           int depth) {
  if (!operand || depth > IR_ALIAS_MAX_DEPTH) {
    alias_set_poison(out);
    return;
  }
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    int index = alias_function_parameter_index(owner, operand->name);
    if (index >= 0) {
      size_t slot = alias_function_slot(facts, owner);
      if (slot == (size_t)-1 ||
          (size_t)index >= facts->functions[slot].param_count) {
        alias_set_poison(out);
        return;
      }
      alias_set_merge(out, &facts->functions[slot].params[index]);
      return;
    }
    if (alias_symbol_def_count(owner, operand->name) == 1) {
      const IRInstruction *def =
          alias_unique_def(owner, IR_OPERAND_SYMBOL, operand->name);
      if (def) {
        alias_roots_of_def(facts, owner, def, out, depth);
        return;
      }
    }
    alias_set_poison(out);
    return;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    const IRInstruction *def =
        alias_unique_def(owner, IR_OPERAND_TEMP, operand->name);
    if (def) {
      alias_roots_of_def(facts, owner, def, out, depth);
      return;
    }
  }
  alias_set_poison(out);
}

/* ------------------------------------------------------------------ building */

static void alias_poison_named_function(IRAliasFacts *facts,
                                        const IROperand *operand) {
  IRProgram *program = facts->program;
  if (!operand || operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return;
  }
  if (facts->name_count) {
    size_t mask = facts->name_count - 1;
    size_t i = mettle_fnv1a_hash(operand->name) & mask;
    while (facts->name_keys[i]) {
      if (strcmp(facts->name_keys[i], operand->name) == 0) {
        facts->functions[facts->name_values[i]].poisoned = 1;
      }
      i = (i + 1) & mask;
    }
    return;
  }
  for (size_t g = 0; g < program->function_count; g++) {
    if (program->functions[g] && program->functions[g]->name &&
        strcmp(program->functions[g]->name, operand->name) == 0) {
      facts->functions[g].poisoned = 1;
    }
  }
}

/* Which functions have callers this compile cannot see. `export` and `main`
 * are entered from outside by definition. Everything else gets in through a
 * function pointer, and a pointer to a function exists only where the program
 * spells its name somewhere other than the callee slot of a direct call, so
 * naming one as an operand is what poisons it. An indirect call reaches only
 * addresses that were taken, so it needs no separate rule. */
static void alias_poison_escaping_functions(IRAliasFacts *facts) {
  IRProgram *program = facts->program;

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      facts->functions[f].poisoned = 1;
      continue;
    }
    if (function->is_exported || function->is_swappable || function->is_kernel ||
        (function->name && strcmp(function->name, "main") == 0)) {
      facts->functions[f].poisoned = 1;
    }
  }

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      alias_poison_named_function(facts, &ins->dest);
      alias_poison_named_function(facts, &ins->lhs);
      alias_poison_named_function(facts, &ins->rhs);
      for (size_t a = 0; a < ins->argument_count; a++) {
        alias_poison_named_function(facts, &ins->arguments[a]);
      }
    }
  }
}

static int alias_seed_parameters(IRAliasFacts *facts) {
  IRProgram *program = facts->program;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    IRAliasFunction *info = &facts->functions[f];
    size_t count = function ? function->parameter_count : 0;
    info->param_count = count;
    info->params = NULL;
    if (count == 0) {
      continue;
    }
    info->params = calloc(count, sizeof(IRAliasRootSet));
    if (!info->params) {
      return 0;
    }
    for (size_t p = 0; p < count; p++) {
      if (info->poisoned ||
          !alias_type_name_is_pointer(program, function->parameter_types[p])) {
        alias_set_poison(&info->params[p]);
      }
    }
  }
  return 1;
}

static int alias_propagate_round(IRAliasFacts *facts) {
  IRProgram *program = facts->program;
  int changed = 0;

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *caller = program->functions[f];
    if (!caller) {
      continue;
    }
    for (size_t i = 0; i < caller->instruction_count; i++) {
      const IRInstruction *ins = &caller->instructions[i];
      IRFunction *callee;
      size_t slot;
      if (ins->op != IR_OP_CALL || !ins->text) {
        continue;
      }
      callee = ir_program_find_function(program, ins->text);
      if (!callee) {
        continue;
      }
      slot = alias_function_slot(facts, callee);
      if (slot == (size_t)-1) {
        continue;
      }
      for (size_t p = 0; p < facts->functions[slot].param_count; p++) {
        IRAliasRootSet roots = {0};
        if (facts->functions[slot].params[p].unknown) {
          continue;
        }
        if (p >= ins->argument_count) {
          alias_set_poison(&facts->functions[slot].params[p]);
          changed = 1;
          continue;
        }
        alias_roots_of(facts, caller, &ins->arguments[p], &roots, 0);
        changed |= alias_set_merge(&facts->functions[slot].params[p], &roots);
      }
    }
  }
  return changed;
}

void ir_alias_facts_reset(void) {
  for (size_t f = 0; f < g_alias.function_count; f++) {
    free(g_alias.functions[f].params);
  }
  free(g_alias.functions);
  free(g_alias.slot_keys);
  free(g_alias.slot_values);
  free(g_alias.name_keys);
  free(g_alias.name_values);
  for (size_t r = 0; r < g_alias.root_count; r++) {
    free(g_alias.root_names[r]);
  }
  free(g_alias.root_names);
  memset(&g_alias, 0, sizeof(g_alias));
  alias_defs_destroy();
}

/* ========================================================================== */
/* Type-based disambiguation                                                   */
/*                                                                             */
/* A store of an int32 cannot change a slot that holds a pointer, because no   */
/* correct program writes one over the other. That is the classic type-based   */
/* rule, and the classic reason to distrust it is that C leaves punning        */
/* undefined and then miscompiles the programs that do it anyway. Here the     */
/* premise is checked: a whole-program compile can look for the punning        */
/* directly, and any class it finds punned stops disambiguating against        */
/* everything, program-wide. A program that deliberately views one allocation  */
/* as two types keeps working, at the speed it had before.                     */
/*                                                                             */
/* What creates two views of one address is a value reaching two pointer types */
/* with different pointee classes. That covers the explicit cast `(N**)p` and  */
/* the `rawptr` idiom every allocation uses, which carries no cast at all:     */
/*                                                                             */
/*     var m: rawptr = malloc(64);                                             */
/*     var a: int32* = m;      // one view                                     */
/*     var b: float64* = m;    // a second view of the same bytes              */
/*                                                                             */
/* Both are the same event once a destination's declared pointee type is       */
/* attributed to the root value it came from, so one walk finds both. Two      */
/* separate `malloc` calls are separate roots and stay disambiguated, which is */
/* what keeps the ordinary typed-allocation idiom fast. Roots the walk cannot  */
/* name share one pessimistic bucket, so an unresolvable conversion weakens    */
/* the rule rather than escaping it.                                           */
/* ========================================================================== */

/* Lowering records these on each load and store; the numbering lives in ir.h
 * so both sides agree. */
typedef IRAliasClassId IRAliasClass;
#define ALIAS_CLASS_UNKNOWN IR_ALIAS_CLASS_NONE
#define ALIAS_CLASS_POINTER IR_ALIAS_CLASS_POINTER
#define ALIAS_CLASS_COUNT IR_ALIAS_CLASS_COUNT

static int g_alias_punned[ALIAS_CLASS_COUNT];

static IRAliasClass alias_class_of_type(const MtlcType *type) {
  if (!type) {
    return ALIAS_CLASS_UNKNOWN;
  }
  switch (type->kind) {
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_FUNCTION_POINTER:
  case MTLC_TYPE_STRING:
    return ALIAS_CLASS_POINTER;
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return IR_ALIAS_CLASS_I8;
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return IR_ALIAS_CLASS_I16;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
    return IR_ALIAS_CLASS_I32;
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT64:
    return IR_ALIAS_CLASS_I64;
  case MTLC_TYPE_FLOAT32:
    return IR_ALIAS_CLASS_F32;
  case MTLC_TYPE_FLOAT64:
    return IR_ALIAS_CLASS_F64;
  case MTLC_TYPE_FLOAT16:
    return IR_ALIAS_CLASS_F16;
  case MTLC_TYPE_BFLOAT16:
    return IR_ALIAS_CLASS_BF16;
  default:
    /* Aggregates and enums carry their members' storage; a whole-aggregate
     * move is not a typed scalar access and never disambiguates. */
    return ALIAS_CLASS_UNKNOWN;
  }
}

static IRAliasClass alias_pointee_class(const IRProgram *program,
                                        const char *pointer_type_name) {
  MtlcType *type;
  size_t length;
  char stem[128];

  if (!pointer_type_name) {
    return ALIAS_CLASS_UNKNOWN;
  }
  type = ir_program_lookup_type(program, pointer_type_name);
  if (type && type->kind == MTLC_TYPE_POINTER) {
    return alias_class_of_type(type->base_type);
  }
  /* A name the registry does not carry: read the spelling. Only a single
   * trailing star is a scalar view; `T**` points at a pointer. */
  length = strlen(pointer_type_name);
  if (length < 2 || pointer_type_name[length - 1] != '*' ||
      length - 1 >= sizeof(stem)) {
    return ALIAS_CLASS_UNKNOWN;
  }
  memcpy(stem, pointer_type_name, length - 1);
  stem[length - 1] = 0;
  if (stem[length - 2] == '*') {
    return ALIAS_CLASS_POINTER;
  }
  type = ir_program_lookup_type(program, stem);
  if (type) {
    return alias_class_of_type(type);
  }
  if (strcmp(stem, "rawptr") == 0 || strcmp(stem, "cstring") == 0 ||
      strcmp(stem, "string") == 0) {
    return ALIAS_CLASS_POINTER;
  }
  return ALIAS_CLASS_UNKNOWN;
}

/* Every conversion into a typed pointer, keyed by the value it converted. */
typedef struct {
  char *root;
  int classes[ALIAS_CLASS_COUNT];
} IRAliasViewEntry;

typedef struct {
  IRAliasViewEntry *items;
  size_t count;
  size_t capacity;
} IRAliasViews;

static void alias_punn_everything(void) {
  for (int c = 0; c < ALIAS_CLASS_COUNT; c++) {
    g_alias_punned[c] = 1;
  }
}

static void alias_views_note(IRAliasViews *views, const char *root,
                             IRAliasClass klass) {
  if (!root || klass == ALIAS_CLASS_UNKNOWN) {
    return;
  }
  for (size_t i = 0; i < views->count; i++) {
    if (strcmp(views->items[i].root, root) == 0) {
      views->items[i].classes[klass] = 1;
      return;
    }
  }
  if (views->count == views->capacity) {
    size_t capacity = views->capacity ? views->capacity * 2 : 64;
    IRAliasViewEntry *grown =
        realloc(views->items, capacity * sizeof(IRAliasViewEntry));
    if (!grown) {
      alias_punn_everything(); /* losing a conversion would lose a bridge */
      return;
    }
    views->items = grown;
    views->capacity = capacity;
  }
  memset(&views->items[views->count], 0, sizeof(IRAliasViewEntry));
  views->items[views->count].root = mettle_strdup(root);
  if (!views->items[views->count].root) {
    alias_punn_everything();
    return;
  }
  views->items[views->count].classes[klass] = 1;
  views->count++;
}

/* Name the value an operand ultimately came from. Two conversions naming the
 * same root convert the same address; conversions the walk cannot resolve all
 * land on one shared name, so they bridge each other rather than nothing. */
static void alias_view_root(const IRFunction *function,
                            const IROperand *operand, char *out, size_t size,
                            int depth) {
  const IRInstruction *def;
  if (!operand || !operand->name || depth > IR_ALIAS_MAX_DEPTH) {
    snprintf(out, size, "opaque");
    return;
  }
  if (operand->kind == IR_OPERAND_SYMBOL) {
    if (ir_function_symbol_is_parameter(function, operand->name)) {
      snprintf(out, size, "param@%s#%s", function->name, operand->name);
      return;
    }
    if (alias_symbol_def_count(function, operand->name) != 1) {
      /* Every assignment to it converts the same variable, so one name for
       * all of them is exactly right. */
      snprintf(out, size, "sym@%s#%s", function->name, operand->name);
      return;
    }
    def = alias_unique_def(function, IR_OPERAND_SYMBOL, operand->name);
  } else if (operand->kind == IR_OPERAND_TEMP) {
    def = alias_unique_def(function, IR_OPERAND_TEMP, operand->name);
  } else {
    snprintf(out, size, "opaque");
    return;
  }
  if (!def) {
    snprintf(out, size, "opaque");
    return;
  }
  if (def->op == IR_OP_ASSIGN || def->op == IR_OP_CAST) {
    alias_view_root(function, &def->lhs, out, size, depth + 1);
    return;
  }
  if (def->op == IR_OP_ADDRESS_OF && def->lhs.kind == IR_OPERAND_SYMBOL &&
      def->lhs.name) {
    snprintf(out, size, "addr@%s#%s",
             alias_symbol_owner(function, def->lhs.name), def->lhs.name);
    return;
  }
  if (def->op == IR_OP_CALL && def->text) {
    if (alias_name_is_fresh_allocator(def->text)) {
      snprintf(out, size, "alloc@%s#%zu", function->name,
               (size_t)(def - function->instructions));
      return;
    }
    /* Two calls to one function can hand back one address, so its results
     * share a root. */
    snprintf(out, size, "ret@%s", def->text);
    return;
  }
  if (def->op == IR_OP_LOAD) {
    snprintf(out, size, "load");
    return;
  }
  snprintf(out, size, "opaque");
}

static const char *alias_destination_pointer_type(const IRFunction *function,
                                                  const IRInstruction *ins) {
  if (ins->op == IR_OP_CAST) {
    return ins->text;
  }
  if (ins->op != IR_OP_ASSIGN || ins->dest.kind != IR_OPERAND_SYMBOL ||
      !ins->dest.name) {
    return NULL;
  }
  for (size_t p = 0; p < function->parameter_count; p++) {
    if (function->parameter_names[p] &&
        strcmp(function->parameter_names[p], ins->dest.name) == 0) {
      return function->parameter_types[p];
    }
  }
  return ir_function_local_declared_type(function, ins->dest.name);
}

static void alias_collect_views(IRAliasFacts *facts, IRAliasViews *views) {
  IRProgram *program = facts->program;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      const char *type_name;
      IRAliasClass klass;
      char root[256];

      if (ins->op == IR_OP_INLINE_ASM) {
        alias_punn_everything(); /* it can write anything as anything */
        continue;
      }

      type_name = alias_destination_pointer_type(function, ins);
      klass = alias_pointee_class(program, type_name);
      if (klass != ALIAS_CLASS_UNKNOWN) {
        alias_view_root(function, &ins->lhs, root, sizeof(root), 0);
        alias_views_note(views, root, klass);
      }

      if (ins->op == IR_OP_CALL && ins->text) {
        IRFunction *callee = ir_program_find_function(program, ins->text);
        if (callee) {
          for (size_t a = 0;
               a < ins->argument_count && a < callee->parameter_count; a++) {
            IRAliasClass arg_class =
                alias_pointee_class(program, callee->parameter_types[a]);
            if (arg_class == ALIAS_CLASS_UNKNOWN) {
              continue;
            }
            alias_view_root(function, &ins->arguments[a], root, sizeof(root),
                            0);
            alias_views_note(views, root, arg_class);
          }
        }
      }

      if (ins->op == IR_OP_RETURN && function->return_type_name) {
        IRAliasClass ret_class =
            alias_pointee_class(program, function->return_type_name);
        if (ret_class != ALIAS_CLASS_UNKNOWN) {
          alias_view_root(function, &ins->lhs, root, sizeof(root), 0);
          alias_views_note(views, root, ret_class);
        }
      }
    }
  }
}

/* A tagged enum lays its variants' payloads over one another, so one slot
 * really does hold different types at different times. Every class that shares
 * such a union with another is punned. */
static void alias_punn_overlapping_payloads(IRAliasFacts *facts) {
  IRProgram *program = facts->program;
  for (size_t t = 0; t < program->type_registry_count; t++) {
    MtlcType *type = program->type_registry[t].type;
    int seen[ALIAS_CLASS_COUNT] = {0};
    int distinct = 0;
    if (!type || type->kind != MTLC_TYPE_TAGGED_ENUM) {
      continue;
    }
    for (size_t v = 0; v < type->tagged_variant_count; v++) {
      IRAliasClass klass =
          alias_class_of_type(type->tagged_variant_payloads[v]);
      if (klass == ALIAS_CLASS_UNKNOWN || seen[klass]) {
        continue;
      }
      seen[klass] = 1;
      distinct++;
    }
    if (distinct < 2) {
      continue;
    }
    for (int c = 0; c < ALIAS_CLASS_COUNT; c++) {
      if (seen[c]) {
        g_alias_punned[c] = 1;
      }
    }
  }
}

static void alias_build_type_facts(IRAliasFacts *facts) {
  IRAliasViews views = {0};
  memset(g_alias_punned, 0, sizeof(g_alias_punned));
  g_alias_punned[ALIAS_CLASS_UNKNOWN] = 1;

  alias_collect_views(facts, &views);
  for (size_t i = 0; i < views.count; i++) {
    int distinct = 0;
    for (int c = 0; c < ALIAS_CLASS_COUNT; c++) {
      distinct += views.items[i].classes[c] != 0;
    }
    if (distinct >= 2) {
      for (int c = 0; c < ALIAS_CLASS_COUNT; c++) {
        if (views.items[i].classes[c]) {
          g_alias_punned[c] = 1;
        }
      }
    }
  }
  for (size_t i = 0; i < views.count; i++) {
    free(views.items[i].root);
  }
  free(views.items);

  alias_punn_overlapping_payloads(facts);
}

int ir_alias_classes_distinct(unsigned a, unsigned b) {
  if (!g_alias.valid || a == b || a >= ALIAS_CLASS_COUNT ||
      b >= ALIAS_CLASS_COUNT) {
    return 0;
  }
  if (g_alias_punned[a] || g_alias_punned[b]) {
    return 0;
  }
  return 1;
}

void ir_alias_facts_build(IRProgram *program) {
  ir_alias_facts_reset();
  if (!program || program->function_count == 0 ||
      ir_pass_name_is_skipped("alias_facts")) {
    return;
  }
  g_alias.program = program;
  g_alias.function_count = program->function_count;
  g_alias.functions = calloc(program->function_count, sizeof(IRAliasFunction));
  if (!g_alias.functions) {
    memset(&g_alias, 0, sizeof(g_alias));
    return;
  }
  alias_slot_index_build(&g_alias);
  alias_defs_build_all(&g_alias);
  alias_poison_escaping_functions(&g_alias);
  if (!alias_seed_parameters(&g_alias)) {
    ir_alias_facts_reset();
    return;
  }
  for (int round = 0; round < IR_ALIAS_MAX_ROUNDS; round++) {
    if (!alias_propagate_round(&g_alias)) {
      break;
    }
  }
  alias_build_type_facts(&g_alias);
  g_alias.valid = 1;
  if (getenv("METTLE_ALIAS_TRACE")) {
    fprintf(stderr, "[alias] punned:");
    for (int c = 1; c < ALIAS_CLASS_COUNT; c++) {
      if (g_alias_punned[c]) {
        fprintf(stderr, " c%d", c);
      }
    }
    fprintf(stderr, "\n");
  }
  if (getenv("METTLE_ALIAS_TRACE")) {
    for (size_t f = 0; f < g_alias.function_count; f++) {
      IRFunction *fn = program->functions[f];
      if (!fn || g_alias.functions[f].param_count == 0) {
        continue;
      }
      fprintf(stderr, "[alias] %s%s", fn->name,
              g_alias.functions[f].poisoned ? " POISONED" : "");
      for (size_t p = 0; p < g_alias.functions[f].param_count; p++) {
        const IRAliasRootSet *set = &g_alias.functions[f].params[p];
        fprintf(stderr, " | %s=", fn->parameter_names[p]);
        if (set->unknown) {
          fprintf(stderr, "?");
        } else {
          for (int r = 0; r < set->count; r++) {
            fprintf(stderr, "%s%s", r ? "," : "",
                    g_alias.root_names[set->roots[r]]);
          }
          if (set->count == 0) {
            fprintf(stderr, "{}");
          }
        }
      }
      fprintf(stderr, "\n");
    }
  }
}

/* -------------------------------------------------------------------- query */

/* The redundancy pass spells a resolved base as one tag character followed by
 * a name: '&' for the address of a variable, 's' for a symbol holding the
 * address, 't' for a temp. Only the first two can be reasoned about here. */
static int alias_base_root_set(const IRFunction *function, const char *base,
                               IRAliasRootSet *out) {
  size_t slot;
  int index;
  if (!base || !base[0] || !base[1]) {
    return 0;
  }
  if (base[0] == '&') {
    int root = -1;
    char key[256];
    if (snprintf(key, sizeof(key), "%s#%s",
                 alias_symbol_owner(function, base + 1),
                 base + 1) >= (int)sizeof(key)) {
      return 0;
    }
    for (size_t i = 0; i < g_alias.root_count; i++) {
      if (strcmp(g_alias.root_names[i], key) == 0) {
        root = (int)i;
        break;
      }
    }
    out->unknown = 0;
    out->count = 0;
    if (root < 0) {
      /* No call site ever carried this variable's address, so no parameter
       * can hold it. An empty set stays empty: the caller reads it through
       * alias_set_contains, never through disjointness. */
      return 2;
    }
    out->roots[out->count++] = root;
    return 2;
  }
  if (base[0] != 's') {
    return 0;
  }
  slot = alias_function_slot(&g_alias, function);
  if (slot == (size_t)-1) {
    return 0;
  }
  index = alias_function_parameter_index(function, base + 1);
  if (index < 0 || (size_t)index >= g_alias.functions[slot].param_count) {
    return 0;
  }
  *out = g_alias.functions[slot].params[index];
  return 1;
}

int ir_alias_bases_distinct(const IRFunction *function, const char *base_a,
                            const char *base_b) {
  IRAliasRootSet a = {0};
  IRAliasRootSet b = {0};
  int kind_a;
  int kind_b;

  if (!g_alias.valid || !function || !function->name) {
    return 0;
  }
  kind_a = alias_base_root_set(function, base_a, &a);
  kind_b = alias_base_root_set(function, base_b, &b);
  if (!kind_a || !kind_b) {
    return 0;
  }
  if (kind_a == 2 && kind_b == 2) {
    return 0; /* the pass already knows two variables are distinct */
  }
  if (kind_a == 2 || kind_b == 2) {
    /* A variable's address against a parameter: distinct when the parameter's
     * roots are known and that variable is not among them. */
    const IRAliasRootSet *var = kind_a == 2 ? &a : &b;
    const IRAliasRootSet *param = kind_a == 2 ? &b : &a;
    if (param->unknown) {
      return 0;
    }
    if (var->count == 0) {
      return 1; /* the address never reached any call */
    }
    return !alias_set_contains(param, var->roots[0]);
  }
  return alias_sets_disjoint(&a, &b);
}
