#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

#include <limits.h>

int ir_operand_clone(const IROperand *source, IROperand *out) {
  if (!out) {
    return 0;
  }

  *out = ir_operand_none();
  if (!source) {
    return 1;
  }

  out->kind = source->kind;
  out->int_value = source->int_value;
  out->float_value = source->float_value;
  out->float_bits = source->float_bits;

  switch (source->kind) {
  case IR_OPERAND_STRING:
    if (!source->name) {
      return 0;
    }
    out->name =
        ir_copy_literal_bytes(source->name, ir_operand_string_length(source));
    if (!out->name) {
      *out = ir_operand_none();
      return 0;
    }
    break;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_LABEL:
    if (!source->name) {
      return 0;
    }
    out->name = mettle_strdup(source->name);
    if (!out->name) {
      *out = ir_operand_none();
      return 0;
    }
    break;
  default:
    break;
  }

  return 1;
}

static int ir_name_map_reindex(IRNameMap *map, size_t min_buckets) {
  size_t nb = 64;
  while (nb < min_buckets) {
    nb *= 2;
  }
  size_t *fresh = (size_t *)calloc(nb, sizeof(size_t));
  if (!fresh) {
    return 0;
  }
  for (size_t i = 0; i < map->count; i++) {
    if (!map->items[i].from) {
      continue;
    }
    size_t b = mettle_fnv1a_hash(map->items[i].from) & (nb - 1);
    while (fresh[b]) {
      b = (b + 1) & (nb - 1);
    }
    fresh[b] = i + 1;
  }
  free(map->buckets);
  map->buckets = fresh;
  map->bucket_count = nb;
  return 1;
}

static int ir_name_map_find(const IRNameMap *map, const char *from) {
  if (!map || !from) {
    return -1;
  }

  if (map->bucket_count) {
    size_t b = mettle_fnv1a_hash(from) & (map->bucket_count - 1);
    while (map->buckets[b]) {
      size_t i = map->buckets[b] - 1;
      if (map->items[i].from && map->items[i].from[0] == from[0] &&
          strcmp(map->items[i].from, from) == 0) {
        return (int)i;
      }
      b = (b + 1) & (map->bucket_count - 1);
    }
    return -1;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].from && map->items[i].from[0] == from[0] &&
        strcmp(map->items[i].from, from) == 0) {
      return (int)i;
    }
  }

  return -1;
}

const char *ir_name_map_lookup(const IRNameMap *map, const char *from) {
  int index = ir_name_map_find(map, from);
  if (index < 0) {
    return NULL;
  }
  return map->items[index].to;
}

int ir_name_map_add(IRNameMap *map, const char *from, const char *to) {
  if (!map || !from || !to) {
    return 0;
  }

  if (ir_name_map_find(map, from) >= 0) {
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRNameMapEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRNameMapEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  char *from_copy = mettle_strdup(from);
  char *to_copy = mettle_strdup(to);
  if (!from_copy || !to_copy) {
    free(from_copy);
    free(to_copy);
    return 0;
  }

  map->items[map->count].from = from_copy;
  map->items[map->count].to = to_copy;
  map->count++;
  if ((map->count + 1) * 4 >= map->bucket_count * 3) {
    if (!ir_name_map_reindex(map, (map->count + 1) * 2)) {
      return 0;
    }
  } else {
    size_t b = mettle_fnv1a_hash(from_copy) & (map->bucket_count - 1);
    while (map->buckets[b]) {
      b = (b + 1) & (map->bucket_count - 1);
    }
    map->buckets[b] = map->count;
  }
  return 1;
}

void ir_name_map_destroy(IRNameMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].from);
    free(map->items[i].to);
  }
  free(map->items);
  free(map->buckets);
  map->items = NULL;
  map->buckets = NULL;
  map->count = 0;
  map->capacity = 0;
  map->bucket_count = 0;
}

char *ir_make_inline_prefix(const char *callee_name, size_t inline_id) {
  /* The site id alone is unique (one shared counter per inlining run; the
   * forced-inline simulator uses a disjoint high base). The callee name used
   * to be embedded for readability, but nested inlining compounds prefixes
   * into very long names whose hashing/compare/copy cost was measurable in
   * every downstream pass -- keep them short. */
  (void)callee_name;
  int length = snprintf(NULL, 0, "__inl_%zu", inline_id);
  if (length < 0) {
    return NULL;
  }

  size_t size = (size_t)length + 1;
  char *prefix = malloc(size);
  if (!prefix) {
    return NULL;
  }

  snprintf(prefix, size, "__inl_%zu", inline_id);
  return prefix;
}

char *ir_make_inline_name(const char *prefix, const char *kind,
                                 const char *base) {
  if (!prefix || !kind || !base) {
    return NULL;
  }

  int length = snprintf(NULL, 0, "%s_%s_%s", prefix, kind, base);
  if (length < 0) {
    return NULL;
  }

  size_t size = (size_t)length + 1;
  char *name = malloc(size);
  if (!name) {
    return NULL;
  }
  snprintf(name, size, "%s_%s_%s", prefix, kind, base);
  return name;
}

const char *ir_name_map_get_or_create(IRNameMap *map, const char *from,
                                             const char *prefix,
                                             const char *kind) {
  const char *existing = ir_name_map_lookup(map, from);
  if (existing) {
    return existing;
  }

  char *generated = ir_make_inline_name(prefix, kind, from);
  if (!generated) {
    return NULL;
  }

  int ok = ir_name_map_add(map, from, generated);
  free(generated);
  if (!ok) {
    return NULL;
  }

  return ir_name_map_lookup(map, from);
}

int ir_instruction_writes_temp(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind != IR_OPERAND_TEMP ||
      !instruction->dest.name) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  /* A three-operand select and the LCG kernel both name their result in
   * dest. Leaving them out told every caller the symbol was never written,
   * which is the unsound direction: a pass that asks whether a variable is
   * assigned anywhere concluded no and treated it as a constant. */
  case IR_OP_SELECT:
  case IR_OP_SIMD_LCG_U32:
    return 1;
  default:
    return 0;
  }
}

int ir_instruction_writes_symbol(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind != IR_OPERAND_SYMBOL ||
      !instruction->dest.name) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_SIMD_FILL: /* dest, when set, receives the final byte offset */
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
  case IR_OP_SELECT:
  case IR_OP_SIMD_LCG_U32:
  /* GPU shared/local allocation names its pointer in dest. */
  case IR_OP_ADDRESS_SPACE_ALLOC:
    return 1;
  default:
    return 0;
  }
}

int ir_instruction_writes_destination(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind == IR_OPERAND_NONE) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_SIMD_FILL: /* dest, when set, receives the final byte offset */
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
  case IR_OP_SELECT:
  case IR_OP_SIMD_LCG_U32:
  /* GPU shared/local allocation names its pointer in dest. */
  case IR_OP_ADDRESS_SPACE_ALLOC:
    return 1;
  default:
    return 0;
  }
}

/* An integer divide or modulo faults on the machine, and a fault is something
 * the program does. Removing one because nothing reads its result removes the
 * trap with it, and `if ((b % (a / b)) != 0) { a = a; }` is a whole branch
 * whose only effect IS the trap: debug stopped with a divide-by-zero report at
 * the right line and -O returned a number. `--verify` calls that a miscompile
 * and quarantines whichever pass did it, so the passes and the validator
 * disagreed about the same instruction.
 *
 * A constant divisor settles it. Zero is the fault everyone means; on x86 the
 * signed form also faults on the minimum over -1, so -1 stays live too. Any
 * other constant cannot fault and the instruction is as dead as an add. A
 * float divide answers inf or nan and never faults. */
static int ir_binary_divide_may_fault(const IRInstruction *instruction) {
  if (instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text) {
    return 0;
  }
  if (strcmp(instruction->text, "/") != 0 &&
      strcmp(instruction->text, "%") != 0) {
    return 0;
  }
  if (instruction->rhs.kind == IR_OPERAND_INT &&
      instruction->rhs.int_value != 0 && instruction->rhs.int_value != -1) {
    return 0;
  }
  return 1;
}

int ir_instruction_is_trivially_dead_if_dest_unused(
    const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
    return !ir_binary_divide_may_fault(instruction);
  default:
    return 0;
  }
}

/* True when a declared type name makes the SYMBOL denote storage rather than a
 * value: a string, an array, a struct. `@a <- @b` on one of those is a block
 * copy, so `@a` still names its own bytes afterwards -- rewriting a later use
 * of `@a` to `@b` would send `*(@a + k) <- v` through the source instead, and
 * when the source is a literal that means a write into rodata. Scalars,
 * pointers and function pointers carry their whole value in the symbol. */
static int ir_type_name_denotes_storage(const char *type) {
  size_t len = type ? strlen(type) : 0;

  if (len == 0 || type[len - 1] == '*') {
    return 0; /* unknown, or a pointer */
  }
  if (strncmp(type, "fn(", 3) == 0 || strncmp(type, "Fn(", 3) == 0) {
    return 0;
  }
  if (strcmp(type, "cstring") == 0 || strcmp(type, "rawptr") == 0 ||
      strcmp(type, "bool") == 0 || strcmp(type, "float32") == 0 ||
      strcmp(type, "float64") == 0 || strcmp(type, "void") == 0) {
    return 0;
  }
  if (ir_int_type_name_info(type, NULL, NULL)) {
    return 0;
  }
  return 1; /* string, T[N], a struct name */
}

int ir_storage_symbol_set_build(const IRFunction *function,
                                IRTempValueMap *set) {
  static const IROperand one = {.kind = IR_OPERAND_INT, .int_value = 1};

  if (!function || !set) {
    return 0;
  }
  if (function->parameter_names && function->parameter_types) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      if (function->parameter_names[i] &&
          ir_type_name_denotes_storage(function->parameter_types[i]) &&
          !ir_temp_value_map_set(set, function->parameter_names[i], &one)) {
        return 0;
      }
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ir_type_name_denotes_storage(ins->text)) {
      if (!ir_temp_value_map_set(set, ins->dest.name, &one)) {
        return 0;
      }
    }
  }
  return 1;
}

int ir_operand_is_propagatable_value(const IROperand *operand) {
  if (!operand) {
    return 0;
  }

  switch (operand->kind) {
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
  case IR_OPERAND_LABEL:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_TEMP:
    return 1;
  /* A string literal operand means different things in different positions:
   * as a LOAD base it addresses the literal's 16-byte {chars, length} record
   * (that is how a string-to-cstring coercion reads the pointer out), and as
   * an ASSIGN value it is the character data. Propagating one from an assign
   * into a load therefore reads the low byte of a pointer where the program
   * asked for the first character. The frontend puts each literal in the
   * position it means; the optimizer must leave it there. */
  case IR_OPERAND_STRING:
  default:
    return 0;
  }
}

void ir_instruction_clear_arguments(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  if (instruction->arguments) {
    for (size_t i = 0; i < instruction->argument_count; i++) {
      ir_operand_destroy(&instruction->arguments[i]);
    }
    free(instruction->arguments);
  }
  free(instruction->argument_types);
  instruction->arguments = NULL;
  instruction->argument_types = NULL;
  instruction->argument_count = 0;
}

void ir_instruction_make_nop(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  if (instruction->text) {
    mettle_free_string(instruction->text);
    instruction->text = NULL;
  }
  instruction->is_float = 0;
  instruction->async_copy_element_count = 0;
  instruction->async_copy_transaction_bytes = 0;
  instruction->async_copy_pending_groups = 0;
  instruction->async_copy_cache = MTLC_ASYNC_CACHE_DEFAULT;
  instruction->async_copy_generated = 0;
  instruction->tensor_residency_id = 0;
  instruction->tensor_residency_role = IR_TENSOR_RESIDENCY_NONE;
  instruction->tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_NONE;
  ir_instruction_tensor_clear(instruction);
  instruction->ast_ref = NULL;
  instruction->op = IR_OP_NOP;
}

void ir_instruction_make_jump(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  ir_instruction_tensor_clear(instruction);
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  instruction->op = IR_OP_JUMP;
}

void ir_instruction_destroy_storage(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  ir_instruction_tensor_clear(instruction);
  if (instruction->text) {
    mettle_free_string(instruction->text);
    instruction->text = NULL;
  }
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  instruction->op = IR_OP_NOP;
}

int ir_instruction_vector_reserve(IRInstructionVector *vector,
                                  size_t capacity) {
  if (!vector) {
    return 0;
  }
  if (vector->capacity >= capacity) {
    return 1;
  }
  size_t new_capacity = vector->capacity == 0 ? 64 : vector->capacity * 2;
  while (new_capacity < capacity) {
    new_capacity *= 2;
  }
  IRInstruction *new_items =
      realloc(vector->items, new_capacity * sizeof(IRInstruction));
  if (!new_items) {
    return 0;
  }
  vector->items = new_items;
  vector->capacity = new_capacity;
  return 1;
}

int ir_instruction_vector_append_move(IRInstructionVector *vector,
                                             IRInstruction *instruction) {
  if (!vector || !instruction) {
    return 0;
  }

  if (vector->count >= vector->capacity) {
    size_t new_capacity = vector->capacity == 0 ? 64 : vector->capacity * 2;
    IRInstruction *new_items =
        realloc(vector->items, new_capacity * sizeof(IRInstruction));
    if (!new_items) {
      return 0;
    }
    vector->items = new_items;
    vector->capacity = new_capacity;
  }

  vector->items[vector->count++] = *instruction;
  /* A move: the vector now owns every heap block the instruction carried, so
   * the source must not. `argument_types` is one of them -- it is a separate
   * allocation from `arguments`, and leaving it behind here left the moved
   * copy pointing at memory the caller's destroy sweep then freed. Only a
   * frontend that supplies typed call arguments (the public builder does;
   * Mettle source does not) ever fills that array, which is why the dangling
   * read stayed hidden. */
  instruction->tensor = NULL;
  instruction->op = IR_OP_NOP;
  instruction->dest = ir_operand_none();
  instruction->lhs = ir_operand_none();
  instruction->rhs = ir_operand_none();
  instruction->text = NULL;
  instruction->arguments = NULL;
  instruction->argument_types = NULL;
  instruction->argument_count = 0;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  return 1;
}

void ir_instruction_vector_destroy(IRInstructionVector *vector) {
  if (!vector) {
    return;
  }

  for (size_t i = 0; i < vector->count; i++) {
    ir_instruction_destroy_storage(&vector->items[i]);
  }
  free(vector->items);
  vector->items = NULL;
  vector->count = 0;
  vector->capacity = 0;
}

int ir_index_vector_append(IRIndexVector *vector, size_t value) {
  if (!vector) {
    return 0;
  }

  if (vector->count >= vector->capacity) {
    size_t new_capacity = vector->capacity == 0 ? 16 : vector->capacity * 2;
    size_t *new_items = realloc(vector->items, new_capacity * sizeof(size_t));
    if (!new_items) {
      return 0;
    }
    vector->items = new_items;
    vector->capacity = new_capacity;
  }

  vector->items[vector->count++] = value;
  return 1;
}

void ir_index_vector_destroy(IRIndexVector *vector) {
  if (!vector) {
    return;
  }
  free(vector->items);
  vector->items = NULL;
  vector->count = 0;
  vector->capacity = 0;
}

int ir_rewrite_to_assign_operand(IRInstruction *instruction,
                                        const IROperand *value, int *changed) {
  IROperand cloned = ir_operand_none();
  if (!instruction || !value || !ir_operand_clone(value, &cloned)) {
    return 0;
  }

  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  if (instruction->text) {
    mettle_free_string(instruction->text);
    instruction->text = NULL;
  }

  instruction->op = IR_OP_ASSIGN;
  instruction->lhs = cloned;
  instruction->rhs = ir_operand_none();
  instruction->is_float = (cloned.kind == IR_OPERAND_FLOAT) ? 1 : 0;
  if (instruction->is_float) {
    instruction->float_bits = (cloned.float_bits == 32) ? 32 : 64;
  }
  instruction->ast_ref = NULL;

  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_rewrite_to_assign_int(IRInstruction *instruction, long long value,
                                    int *changed) {
  IROperand constant = ir_operand_int(value);
  return ir_rewrite_to_assign_operand(instruction, &constant, changed);
}

int ir_temp_value_map_init(IRTempValueMap *map) {
  if (!map) {
    return 0;
  }

  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  map->ix = NULL;
  map->ix_capacity = 0;
  map->ix_tombstones = 0;
  map->vsym_counts = NULL;
  return 1;
}

/* ---- hash index over the entry array --------------------------------------
 * find/set/remove were linear strcmp scans; on the multi-thousand-entry maps
 * copy-propagation builds inside post-inlining functions that turned the pass
 * quadratic (it was 26s of a 29s compile on a 4000-function fixture). The
 * array remains the storage passes iterate; this index only maps name->slot. */

#define IR_TVM_TOMB UINT_MAX

static unsigned int ir_tvm_hash(const char *s) {
  unsigned int h = 2166136261u;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h;
}

static void ir_tvm_index_insert_with(IRTempValueMap *map, unsigned int hash,
                                     size_t slot) {
  size_t mask = map->ix_capacity - 1;
  size_t b = hash & mask;
  while (map->ix[b] != 0 && map->ix[b] != IR_TVM_TOMB) {
    b = (b + 1) & mask;
  }
  if (map->ix[b] == IR_TVM_TOMB && map->ix_tombstones > 0) {
    map->ix_tombstones--;
  }
  map->ix[b] = (unsigned int)(slot + 1);
}

static void ir_tvm_index_insert(IRTempValueMap *map, const char *name,
                                size_t slot) {
  ir_tvm_index_insert_with(map, ir_tvm_hash(name), slot);
}

int ir_temp_value_map_reindex(IRTempValueMap *map) {
  if (!map) {
    return 0;
  }
  size_t want = 32;
  while (want < map->count * 2 + 8) {
    want <<= 1;
  }
  if (map->ix_capacity < want) {
    free(map->ix);
    map->ix = malloc(want * sizeof(unsigned int));
    if (!map->ix) {
      map->ix_capacity = 0;
      return 0;
    }
    map->ix_capacity = want;
  }
  memset(map->ix, 0, map->ix_capacity * sizeof(unsigned int));
  map->ix_tombstones = 0;
  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].name) {
      ir_tvm_index_insert(map, map->items[i].name, i);
    }
  }
  return 1;
}

/* Make sure the index exists and is healthy; falls back to "no index" on
 * allocation failure (find then degrades to the linear scan). */
static void ir_tvm_index_ensure(IRTempValueMap *map) {
  if (!map->ix || (map->count + map->ix_tombstones) * 2 >= map->ix_capacity) {
    ir_temp_value_map_reindex(map);
  }
}

static int ir_temp_value_map_find_with(const IRTempValueMap *map,
                                       const char *name, unsigned int hash) {
  if (!map || !name || map->count == 0) {
    return -1;
  }

  ir_tvm_index_ensure((IRTempValueMap *)map);
  if (!map->ix) {
    /* Allocation failed: stay correct via the linear scan. */
    for (size_t i = 0; i < map->count; i++) {
      if (map->items[i].name && map->items[i].name[0] == name[0] &&
          strcmp(map->items[i].name, name) == 0) {
        return (int)i;
      }
    }
    return -1;
  }

  size_t mask = map->ix_capacity - 1;
  size_t b = hash & mask;
  while (map->ix[b] != 0) {
    if (map->ix[b] != IR_TVM_TOMB) {
      size_t slot = (size_t)map->ix[b] - 1;
      if (slot < map->count && map->items[slot].name &&
          map->items[slot].name[0] == name[0] &&
          strcmp(map->items[slot].name, name) == 0) {
        return (int)slot;
      }
    }
    b = (b + 1) & mask;
  }
  return -1;
}

static int ir_temp_value_map_find(const IRTempValueMap *map, const char *name) {
  return name ? ir_temp_value_map_find_with(map, name, ir_tvm_hash(name)) : -1;
}

/* Tombstone the bucket that points at `slot` for `name`. */
static void ir_tvm_index_erase(IRTempValueMap *map, const char *name,
                               size_t slot) {
  if (!map->ix) {
    return;
  }
  size_t mask = map->ix_capacity - 1;
  size_t b = ir_tvm_hash(name) & mask;
  while (map->ix[b] != 0) {
    if (map->ix[b] != IR_TVM_TOMB && (size_t)map->ix[b] - 1 == slot) {
      map->ix[b] = IR_TVM_TOMB;
      map->ix_tombstones++;
      return;
    }
    b = (b + 1) & mask;
  }
}

/* Repoint the bucket of `name` from `old_slot` to `new_slot` (swap-remove
 * moved it). */
static void ir_tvm_index_move(IRTempValueMap *map, const char *name,
                              size_t old_slot, size_t new_slot) {
  if (!map->ix) {
    return;
  }
  size_t mask = map->ix_capacity - 1;
  size_t b = ir_tvm_hash(name) & mask;
  while (map->ix[b] != 0) {
    if (map->ix[b] != IR_TVM_TOMB && (size_t)map->ix[b] - 1 == old_slot) {
      map->ix[b] = (unsigned int)(new_slot + 1);
      return;
    }
    b = (b + 1) & mask;
  }
}

/* ---- reverse value-symbol counts -------------------------------------------
 * vsym_counts maps a symbol name to the number of entries whose VALUE is that
 * symbol, so per-symbol-write invalidation can skip the full-entry scan when
 * nothing maps to the written symbol (the common case -- this scan per write
 * was quadratic inside copy-propagation on big inlined functions). Built
 * lazily on first use, then maintained by set/remove and the compactors. */

static void ir_tvm_vsym_adjust(IRTempValueMap *counts, const char *sym,
                               long long delta) {
  if (!counts || !sym) {
    return;
  }
  const IROperand *cur = ir_temp_value_map_lookup(counts, sym);
  long long v = (cur ? cur->int_value : 0) + delta;
  if (v <= 0) {
    ir_temp_value_map_remove(counts, sym);
    return;
  }
  IROperand op = {.kind = IR_OPERAND_INT, .int_value = v};
  ir_temp_value_map_set(counts, sym, &op);
}

static void ir_tvm_vsym_note_value(IRTempValueMap *map, const IROperand *value,
                                   long long delta) {
  if (map->vsym_counts && value && value->kind == IR_OPERAND_SYMBOL &&
      value->name) {
    ir_tvm_vsym_adjust(map->vsym_counts, value->name, delta);
  }
}

/* Lazily build the reverse counts; returns 0 (and leaves the map without
 * counts) on allocation failure, in which case callers use the plain scan. */
static int ir_tvm_vsym_ensure(IRTempValueMap *map) {
  if (map->vsym_counts) {
    return 1;
  }
  IRTempValueMap *counts = malloc(sizeof(*counts));
  if (!counts || !ir_temp_value_map_init(counts)) {
    free(counts);
    return 0;
  }
  map->vsym_counts = counts;
  for (size_t i = 0; i < map->count; i++) {
    ir_tvm_vsym_note_value(map, &map->items[i].value, 1);
  }
  return 1;
}

void ir_temp_value_map_remove(IRTempValueMap *map, const char *name) {
  if (!map || !name) {
    return;
  }

  int index = ir_temp_value_map_find(map, name);
  if (index < 0) {
    return;
  }

  size_t idx = (size_t)index;
  ir_tvm_index_erase(map, map->items[idx].name, idx);
  ir_tvm_vsym_note_value(map, &map->items[idx].value, -1);
  free(map->items[idx].name);
  ir_operand_destroy(&map->items[idx].value);

  size_t last = map->count - 1;
  if (idx != last) {
    /* Swap-remove; entry order carries no meaning for these maps. */
    map->items[idx] = map->items[last];
    if (map->items[idx].name) {
      ir_tvm_index_move(map, map->items[idx].name, last, idx);
    }
  }
  map->count--;
}

int ir_temp_value_map_set(IRTempValueMap *map, const char *name,
                                 const IROperand *value) {
  if (!map || !name || !value) {
    return 0;
  }

  unsigned int hash = ir_tvm_hash(name);
  int existing = ir_temp_value_map_find_with(map, name, hash);
  if (existing >= 0) {
    IROperand cloned = ir_operand_none();
    if (!ir_operand_clone(value, &cloned)) {
      return 0;
    }

    ir_tvm_vsym_note_value(map, &map->items[existing].value, -1);
    ir_operand_destroy(&map->items[existing].value);
    map->items[existing].value = cloned;
    ir_tvm_vsym_note_value(map, &map->items[existing].value, 1);
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRTempValueEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRTempValueEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  IROperand cloned = ir_operand_none();
  if (!name_copy || !ir_operand_clone(value, &cloned)) {
    free(name_copy);
    ir_operand_destroy(&cloned);
    return 0;
  }

  map->items[map->count].name = name_copy;
  map->items[map->count].value = cloned;
  map->count++;
  ir_tvm_vsym_note_value(map, &map->items[map->count - 1].value, 1);
  if (map->ix && (map->count + map->ix_tombstones) * 2 < map->ix_capacity) {
    ir_tvm_index_insert_with(map, hash, map->count - 1);
  } else {
    ir_temp_value_map_reindex(map);
  }
  return 1;
}

void ir_temp_value_map_remove_symbol_values(IRTempValueMap *map,
                                                   const char *symbol_name) {
  if (!map) {
    return;
  }

  /* O(1) fast path: nothing in the map values this symbol. This call happens
   * once per symbol WRITE in copy-propagation, so without the reverse count
   * it was an O(entries) scan per write -- quadratic on inlined functions. */
  if (symbol_name && ir_tvm_vsym_ensure(map) &&
      !ir_temp_value_map_lookup(map->vsym_counts, symbol_name)) {
    return;
  }

  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    int remove = 0;

    if (entry->value.kind == IR_OPERAND_SYMBOL && entry->value.name) {
      if (!symbol_name || strcmp(entry->value.name, symbol_name) == 0) {
        remove = 1;
      }
    }

    if (remove) {
      ir_tvm_vsym_note_value(map, &entry->value, -1);
      mettle_free_string(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }

    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }

  if (map->count != write) {
    map->count = write;
    ir_temp_value_map_reindex(map);
  }
}

/* The set of symbols whose address is taken anywhere in `function`, built in
 * one scan so store invalidation can test membership in O(1) instead of
 * rescanning the whole function per map entry per store. The pass that uses
 * it builds it once: copy-propagation never introduces ADDRESS_OF, so the set
 * is stable across a pass run. */
static IRProgram *g_ir_optimize_program = NULL;

void ir_optimize_set_program(IRProgram *program) {
  g_ir_optimize_program = program;
}

static int ir_addr_taken_seed_module_globals(IRTempValueMap *set,
                                             const IROperand *one) {
  IRProgram *p = g_ir_optimize_program;
  if (!p) {
    return 1;
  }
  (void)ir_program_global_address_taken(p, "");
  for (size_t k = 0; k < p->alias_global_count; k++) {
    if (!ir_temp_value_map_set(set, p->alias_globals[k], one)) {
      return 0;
    }
  }
  return 1;
}

int ir_addr_taken_set_build(const IRFunction *function, IRTempValueMap *set) {
  static const IROperand one = {.kind = IR_OPERAND_INT, .int_value = 1};
  if (!function || !set) {
    return 0;
  }
  if (!ir_addr_taken_seed_module_globals(set, &one)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_ADDRESS_OF && ins->lhs.kind == IR_OPERAND_SYMBOL &&
        ins->lhs.name) {
      if (!ir_temp_value_map_set(set, ins->lhs.name, &one)) {
        return 0;
      }
    }
  }
  return 1;
}

/* Store-aware invalidation for copy-propagation's temp and symbol value maps.
 * A store writes memory; it cannot change a mapping unless the mapped key is an
 * address-taken symbol (the store may overwrite its storage) or the mapped
 * value embeds an address-taken symbol (that value may now be stale). Constants,
 * temps, and non-escaped symbol copies such as `%t <- @i` survive, so copy
 * propagation can see through `buf[i] = ...; ... buf[i] ...`.
 *
 * Works for both the temp map (keys are temps, never address-taken) and the
 * symbol map (keys are symbols). Matches the old clear-on-store for escaped
 * symbols while preserving non-escaped ones. */
void ir_temp_value_map_invalidate_after_store(IRTempValueMap *map,
                                              const IRTempValueMap *addr_taken) {
  if (!map) {
    return;
  }

  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    IRTempValueEntry *entry = &map->items[read];
    int remove = 0;

    if (entry->name && ir_temp_value_map_lookup(addr_taken, entry->name)) {
      remove = 1;
    }
    if (!remove && entry->value.kind == IR_OPERAND_SYMBOL &&
        entry->value.name &&
        ir_temp_value_map_lookup(addr_taken, entry->value.name)) {
      remove = 1;
    }

    if (remove) {
      ir_tvm_vsym_note_value(map, &entry->value, -1);
      mettle_free_string(entry->name);
      ir_operand_destroy(&entry->value);
      continue;
    }

    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }

  if (map->count != write) {
    map->count = write;
    ir_temp_value_map_reindex(map);
  }
}

int ir_temp_value_map_any_value_symbol(IRTempValueMap *map,
                                       const char *symbol_name) {
  if (!map || !symbol_name) {
    return 1; /* unknown: caller must scan */
  }
  if (!ir_tvm_vsym_ensure(map)) {
    return 1;
  }
  return ir_temp_value_map_lookup(map->vsym_counts, symbol_name) != NULL;
}

void ir_temp_value_map_note_value_removed(IRTempValueMap *map,
                                          const IROperand *value) {
  ir_tvm_vsym_note_value(map, value, -1);
}

const IROperand *ir_temp_value_map_lookup(const IRTempValueMap *map,
                                                 const char *name) {
  if (!map || !name) {
    return NULL;
  }

  int index = ir_temp_value_map_find(map, name);
  if (index < 0) {
    return NULL;
  }

  return &map->items[index].value;
}

void ir_temp_value_map_clear(IRTempValueMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].name);
    ir_operand_destroy(&map->items[i].value);
  }
  map->count = 0;
  if (map->ix) {
    memset(map->ix, 0, map->ix_capacity * sizeof(unsigned int));
  }
  map->ix_tombstones = 0;
  if (map->vsym_counts) {
    ir_temp_value_map_clear(map->vsym_counts);
  }
}

void ir_temp_value_map_destroy(IRTempValueMap *map) {
  if (!map) {
    return;
  }

  if (map->vsym_counts) {
    /* Detach first so the recursive destroy (depth 1: counts hold ints,
     * never their own counts) doesn't see a half-torn map. */
    IRTempValueMap *counts = map->vsym_counts;
    map->vsym_counts = NULL;
    ir_temp_value_map_destroy(counts);
    free(counts);
  }
  ir_temp_value_map_clear(map);
  free(map->items);
  map->items = NULL;
  map->capacity = 0;
  free(map->ix);
  map->ix = NULL;
  map->ix_capacity = 0;
}

int ir_temp_value_map_clone(IRTempValueMap *dest,
                                   const IRTempValueMap *src) {
  if (!dest || !src) {
    return 0;
  }

  ir_temp_value_map_clear(dest);
  for (size_t i = 0; i < src->count; i++) {
    if (!ir_temp_value_map_set(dest, src->items[i].name, &src->items[i].value)) {
      return 0;
    }
  }
  return 1;
}

static int ir_temp_value_map_intersect_with(IRTempValueMap *dest,
                                            const IRTempValueMap *other,
                                            int *changed) {
  if (!dest || !other) {
    return 0;
  }

  size_t i = 0;
  while (i < dest->count) {
    IRTempValueEntry *entry = &dest->items[i];
    const IROperand *other_value =
        ir_temp_value_map_lookup(other, entry->name);
    if (!other_value || !ir_operand_equals(&entry->value, other_value)) {
      ir_temp_value_map_remove(dest, entry->name);
      if (changed) {
        *changed = 1;
      }
      continue;
    }
    i++;
  }

  return 1;
}

int ir_label_value_map_init(IRLabelValueMap *map) {
  if (!map) {
    return 0;
  }
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  return ir_temp_value_map_init(&map->index);
}

static int ir_label_value_map_find(const IRLabelValueMap *map,
                                   const char *label) {
  if (!map || !label) {
    return -1;
  }
  const IROperand *slot = ir_temp_value_map_lookup(&map->index, label);
  return slot ? (int)slot->int_value : -1;
}

static IRLabelValueEntry *ir_label_value_map_get_or_add(IRLabelValueMap *map,
                                                        const char *label) {
  if (!map || !label) {
    return NULL;
  }

  int existing = ir_label_value_map_find(map, label);
  if (existing >= 0) {
    return &map->items[existing];
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRLabelValueEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRLabelValueEntry));
    if (!new_items) {
      return NULL;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  IRLabelValueEntry *entry = &map->items[map->count];
  memset(entry, 0, sizeof(*entry));
  entry->label = mettle_strdup(label);
  if (!entry->label || !ir_temp_value_map_init(&entry->in_map)) {
    free(entry->label);
    entry->label = NULL;
    return NULL;
  }
  entry->initialized = 0;
  IROperand slot = {.kind = IR_OPERAND_INT, .int_value = (long long)map->count};
  if (!ir_temp_value_map_set(&map->index, label, &slot)) {
    free(entry->label);
    entry->label = NULL;
    ir_temp_value_map_destroy(&entry->in_map);
    return NULL;
  }
  map->count++;
  return entry;
}

int ir_label_value_map_merge_incoming(IRLabelValueMap *map,
                                             const char *label,
                                             const IRTempValueMap *incoming,
                                             int *changed) {
  if (!map || !label || !incoming) {
    return 0;
  }

  IRLabelValueEntry *entry = ir_label_value_map_get_or_add(map, label);
  if (!entry) {
    return 0;
  }

  if (!entry->initialized) {
    if (!ir_temp_value_map_clone(&entry->in_map, incoming)) {
      return 0;
    }
    entry->initialized = 1;
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  return ir_temp_value_map_intersect_with(&entry->in_map, incoming, changed);
}

const IRLabelValueEntry *ir_label_value_map_lookup(
    const IRLabelValueMap *map, const char *label) {
  if (!map || !label) {
    return NULL;
  }
  int index = ir_label_value_map_find(map, label);
  if (index < 0) {
    return NULL;
  }
  return &map->items[index];
}

void ir_label_value_map_destroy(IRLabelValueMap *map) {
  if (!map) {
    return;
  }
  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].label);
    ir_temp_value_map_destroy(&map->items[i].in_map);
  }
  free(map->items);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  ir_temp_value_map_destroy(&map->index);
}

int ir_operand_equals(const IROperand *lhs, const IROperand *rhs) {
  if (!lhs || !rhs || lhs->kind != rhs->kind) {
    return 0;
  }

  switch (lhs->kind) {
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_INT:
    return lhs->int_value == rhs->int_value;
  case IR_OPERAND_FLOAT:
    /* Same numeric value at different IEEE-754 widths is NOT the same operand
     * for CSE/propagation purposes: a float32 0.1 and float64 0.1 have
     * distinct bit patterns and must not be coalesced. Treat unspecified (0)
     * as the default 64 so legacy float64-only IR keeps matching. */
    return lhs->float_value == rhs->float_value &&
           ((lhs->float_bits == 32) == (rhs->float_bits == 32));
  case IR_OPERAND_STRING: {
    size_t lhs_length = ir_operand_string_length(lhs);
    if (!lhs->name || !rhs->name) {
      return 0;
    }
    return lhs_length == ir_operand_string_length(rhs) &&
           memcmp(lhs->name, rhs->name, lhs_length) == 0;
  }
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_LABEL:
    if (!lhs->name || !rhs->name) {
      return 0;
    }
    return strcmp(lhs->name, rhs->name) == 0;
  default:
    return 0;
  }
}

static int ir_binary_op_is_commutative(const char *op_text) {
  if (!op_text) {
    return 0;
  }
  return strcmp(op_text, "+") == 0 || strcmp(op_text, "*") == 0 ||
         strcmp(op_text, "&") == 0 || strcmp(op_text, "|") == 0 ||
         strcmp(op_text, "^") == 0 || strcmp(op_text, "==") == 0 ||
         strcmp(op_text, "!=") == 0;
}

static void ir_expression_entry_destroy(IRExpressionEntry *entry) {
  if (!entry) {
    return;
  }

  mettle_free_string(entry->op_text);
  entry->op_text = NULL;
  ir_operand_destroy(&entry->lhs);
  ir_operand_destroy(&entry->rhs);
  ir_operand_destroy(&entry->value);
  entry->kind = IR_EXPR_BINARY;
  entry->is_float = 0;
}

static void ir_expression_map_clear(IRExpressionMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].alive) {
      ir_expression_entry_destroy(&map->items[i]);
    }
  }
  map->count = 0;
  map->dead_count = 0;
  map->occ_count = 0;
  map->sym_count = 0;
  if (map->occ_heads) {
    memset(map->occ_heads, 0, map->occ_head_capacity * sizeof(size_t));
  }
  if (map->name_hashes) {
    memset(map->name_hashes, 0, map->name_hash_capacity * sizeof(size_t));
  }
  map->name_hash_count = 0;
  map->expr_valid = 0;
}

static void ir_expression_map_destroy(IRExpressionMap *map) {
  if (!map) {
    return;
  }

  ir_expression_map_clear(map);
  free(map->items);
  free(map->name_hashes);
  free(map->expr_slots);
  free(map->occ);
  free(map->occ_heads);
  free(map->sym_entries);
  map->occ = NULL;
  map->occ_capacity = 0;
  map->occ_heads = NULL;
  map->occ_head_capacity = 0;
  map->sym_entries = NULL;
  map->sym_capacity = 0;
  map->name_hashes = NULL;
  map->name_hash_capacity = 0;
  map->expr_slots = NULL;
  map->expr_capacity = 0;
  map->expr_valid = 0;
  map->items = NULL;
  map->capacity = 0;
}

static int ir_instruction_is_cse_candidate(const IRInstruction *instruction) {
  if (!instruction || !ir_instruction_writes_destination(instruction)) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_BINARY:
    return instruction->text != NULL;
  case IR_OP_UNARY:
    if (!instruction->text) {
      return 0;
    }
    return strcmp(instruction->text, "*") != 0 &&
           strcmp(instruction->text, "&") != 0;
  case IR_OP_CAST:
    return instruction->text != NULL;
  case IR_OP_ADDRESS_OF:
    return instruction->lhs.kind == IR_OPERAND_SYMBOL &&
           instruction->lhs.name != NULL;
  default:
    return 0;
  }
}

static int ir_expression_entry_matches_instruction(
    const IRExpressionEntry *entry, const IRInstruction *instruction) {
  if (!entry || !instruction) {
    return 0;
  }

  switch (entry->kind) {
  case IR_EXPR_BINARY:
    if (instruction->op != IR_OP_BINARY || !instruction->text ||
        !entry->op_text || strcmp(entry->op_text, instruction->text) != 0 ||
        entry->is_float != instruction->is_float) {
      return 0;
    }
    if (ir_operand_equals(&entry->lhs, &instruction->lhs) &&
        ir_operand_equals(&entry->rhs, &instruction->rhs)) {
      return 1;
    }
    if (ir_binary_op_is_commutative(instruction->text) &&
        ir_operand_equals(&entry->lhs, &instruction->rhs) &&
        ir_operand_equals(&entry->rhs, &instruction->lhs)) {
      return 1;
    }
    return 0;
  case IR_EXPR_UNARY:
    return instruction->op == IR_OP_UNARY && instruction->text &&
           entry->op_text && strcmp(entry->op_text, instruction->text) == 0 &&
           entry->is_float == instruction->is_float &&
           ir_operand_equals(&entry->lhs, &instruction->lhs);
  case IR_EXPR_CAST:
    return instruction->op == IR_OP_CAST && instruction->text &&
           entry->op_text && strcmp(entry->op_text, instruction->text) == 0 &&
           entry->is_float == instruction->is_float &&
           ir_operand_equals(&entry->lhs, &instruction->lhs);
  case IR_EXPR_ADDRESS_OF:
    return instruction->op == IR_OP_ADDRESS_OF &&
           ir_operand_equals(&entry->lhs, &instruction->lhs);
  default:
    return 0;
  }
}

static void ir_expression_map_note_name(IRExpressionMap *map,
                                        const IROperand *operand);
static int ir_expression_map_may_hold_name(const IRExpressionMap *map,
                                           const char *name);
static size_t ir_expr_operand_hash(const IROperand *operand) {
  if (!operand) {
    return 1u;
  }
  size_t h = (size_t)operand->kind * 2654435761u + 17u;
  switch (operand->kind) {
  case IR_OPERAND_INT:
    h ^= (size_t)operand->int_value * 1099511628211u;
    break;
  case IR_OPERAND_STRING:
    if (operand->name) {
      h ^= (size_t)mettle_fnv1a_hash(operand->name);
      h ^= (size_t)ir_operand_string_length(operand) * 2654435761u;
    }
    break;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_LABEL:
    if (operand->name) {
      h ^= (size_t)mettle_fnv1a_hash(operand->name);
    }
    break;
  default:
    /* IR_OPERAND_FLOAT contributes only its kind: equality there also weighs
     * the IEEE width, and hashing less is always safe. */
    break;
  }
  return h ? h : 1u;
}

static size_t ir_expr_shape_hash(IRExpressionKind kind, int is_float,
                                 const char *op_text, const IROperand *lhs,
                                 const IROperand *rhs) {
  size_t h = (size_t)kind * 40503u + 1469598103u;
  if (kind != IR_EXPR_ADDRESS_OF) {
    h = h * 31u + (size_t)(is_float ? 1 : 0);
    if (op_text) {
      h ^= (size_t)mettle_fnv1a_hash(op_text);
    }
  }
  /* Added, not ordered: the matcher accepts a commutative binary in either
   * order, so the two must hash alike. For a non-commutative operator this
   * only widens the bucket. */
  size_t operands = ir_expr_operand_hash(lhs);
  if (kind == IR_EXPR_BINARY) {
    operands += ir_expr_operand_hash(rhs);
  }
  h ^= operands * 1099511628211u;
  return h ? h : 1u;
}

static size_t ir_expr_entry_hash(const IRExpressionEntry *entry) {
  return ir_expr_shape_hash(entry->kind, entry->is_float, entry->op_text,
                            &entry->lhs, &entry->rhs);
}

static int ir_expr_instruction_hash(const IRInstruction *instruction,
                                    size_t *out_hash) {
  IRExpressionKind kind;
  switch (instruction->op) {
  case IR_OP_BINARY: kind = IR_EXPR_BINARY; break;
  case IR_OP_UNARY: kind = IR_EXPR_UNARY; break;
  case IR_OP_CAST: kind = IR_EXPR_CAST; break;
  case IR_OP_ADDRESS_OF: kind = IR_EXPR_ADDRESS_OF; break;
  default: return 0;
  }
  *out_hash = ir_expr_shape_hash(kind, instruction->is_float,
                                 instruction->text, &instruction->lhs,
                                 &instruction->rhs);
  return 1;
}

static void ir_expression_map_index_put(IRExpressionMap *map, size_t index) {
  size_t mask = map->expr_capacity - 1u;
  size_t s = ir_expr_entry_hash(&map->items[index]) & mask;
  while (map->expr_slots[s]) {
    s = (s + 1u) & mask;
  }
  map->expr_slots[s] = index + 1u;
}

static int ir_expression_map_index_build(IRExpressionMap *map) {
  size_t needed = 16;
  while (needed < (map->count + 1u) * 2u) {
    needed *= 2u;
  }
  if (map->expr_capacity < needed) {
    size_t *slots = (size_t *)calloc(needed, sizeof(size_t));
    if (!slots) {
      return 0;
    }
    free(map->expr_slots);
    map->expr_slots = slots;
    map->expr_capacity = needed;
  }
  else {
    memset(map->expr_slots, 0, map->expr_capacity * sizeof(size_t));
  }
  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].alive) {
      ir_expression_map_index_put(map, i);
    }
  }
  map->expr_valid = 1;
  return 1;
}
static size_t ir_expr_name_hash(const char *name);

static void ir_expression_map_add_occurrence(IRExpressionMap *map,
                                             const IROperand *operand,
                                             size_t entry_index) {
  if (!operand || !operand->name ||
      (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL)) {
    return;
  }
  if (map->occ_count == map->occ_capacity) {
    size_t grown = map->occ_capacity ? map->occ_capacity * 2 : 64;
    IRExprNameOcc *items =
        (IRExprNameOcc *)realloc(map->occ, grown * sizeof(IRExprNameOcc));
    if (!items) {
      map->occ_head_capacity = 0; /* fall back to walking the map */
      return;
    }
    map->occ = items;
    map->occ_capacity = grown;
  }
  if (map->occ_head_capacity == 0 ||
      map->occ_count * 4 >= map->occ_head_capacity * 3) {
    size_t grown = map->occ_head_capacity ? map->occ_head_capacity * 2 : 128;
    size_t *heads = (size_t *)calloc(grown, sizeof(size_t));
    if (!heads) {
      map->occ_head_capacity = 0;
      return;
    }
    free(map->occ_heads);
    map->occ_heads = heads;
    map->occ_head_capacity = grown;
    for (size_t i = 0; i < map->occ_count; i++) {
      size_t b = map->occ[i].name_hash & (grown - 1u);
      map->occ[i].next = map->occ_heads[b];
      map->occ_heads[b] = i + 1u;
    }
  }
  size_t h = ir_expr_name_hash(operand->name);
  size_t bucket = h & (map->occ_head_capacity - 1u);
  map->occ[map->occ_count].name_hash = h;
  map->occ[map->occ_count].entry_index = entry_index;
  map->occ[map->occ_count].next = map->occ_heads[bucket];
  map->occ_heads[bucket] = map->occ_count + 1u;
  map->occ_count++;
}

/* List an entry among the ones a store can reach. The VALUE counts as much as
 * the operands: an entry says "this expression is already available in that
 * place", and a store through a pointer to a symbol changes what the place
 * holds. `-3.5 -> @fy` outlived `*(&fy) <- x` and a later `-3.5` was rewritten
 * to a read of @fy, which by then held something else. Duplicates are harmless:
 * the store path only kills, and killing twice is a no-op. */
static void ir_expression_map_note_symbol_entry(IRExpressionMap *map,
                                                size_t index) {
  if (map->sym_count == map->sym_capacity) {
    size_t grown = map->sym_capacity ? map->sym_capacity * 2 : 32;
    size_t *items = (size_t *)realloc(map->sym_entries, grown * sizeof(size_t));
    if (!items) {
      map->sym_capacity = 0;
      map->sym_count = 0;
      return; /* the store path then falls back to walking the map */
    }
    map->sym_entries = items;
    map->sym_capacity = grown;
  }
  map->sym_entries[map->sym_count++] = index;
}

static void ir_expression_map_register_entry(IRExpressionMap *map,
                                             size_t index) {
  IRExpressionEntry *entry = &map->items[index];
  ir_expression_map_add_occurrence(map, &entry->lhs, index);
  ir_expression_map_add_occurrence(map, &entry->rhs, index);
  ir_expression_map_add_occurrence(map, &entry->value, index);
  if (entry->lhs.kind == IR_OPERAND_SYMBOL ||
      entry->rhs.kind == IR_OPERAND_SYMBOL ||
      entry->value.kind == IR_OPERAND_SYMBOL) {
    ir_expression_map_note_symbol_entry(map, index);
  }
}

static void ir_expression_map_kill(IRExpressionMap *map, size_t index) {
  if (!map->items[index].alive) {
    return;
  }
  ir_expression_entry_destroy(&map->items[index]);
  map->items[index].alive = 0;
  map->dead_count++;
}

/* Once the dead outnumber the living, so the cost of rebuilding the indices is
 * spread over the removals that caused it -- or once the occurrence array has
 * outgrown the entries it describes. That second condition is not optional: an
 * occurrence is only dropped by a rebuild, so a symbol written every iteration
 * accumulates a chain of stale occurrences and walking it becomes the very
 * cost the chain was meant to remove. */
static void ir_expression_map_maybe_compact(IRExpressionMap *map) {
  size_t live = map->count - map->dead_count;
  int crowded = map->dead_count * 2 > map->count;
  /* Measured against the LIVE entries, not every entry ever added: an
   * occurrence naming a dead entry is exactly the stale weight a chain walk
   * pays for, and comparing against the total let it grow without bound. */
  int chained = map->occ_count > 32 && map->occ_count > (live + 1u) * 4u;
  if (!crowded && !chained) {
    return;
  }
  size_t write = 0;
  for (size_t read = 0; read < map->count; read++) {
    if (!map->items[read].alive) {
      continue;
    }
    if (write != read) {
      map->items[write] = map->items[read];
    }
    write++;
  }
  map->count = write;
  map->dead_count = 0;
  map->expr_valid = 0;
  map->occ_count = 0;
  map->sym_count = 0;
  if (map->occ_heads) {
    memset(map->occ_heads, 0, map->occ_head_capacity * sizeof(size_t));
  }
  for (size_t i = 0; i < map->count; i++) {
    ir_expression_map_register_entry(map, i);
  }
}

static int ir_expression_map_find_matching_instruction(
    const IRExpressionMap *map, const IRInstruction *instruction) {
  if (!map || !instruction) {
    return -1;
  }

  size_t wanted = 0;
  IRExpressionMap *mutable_map = (IRExpressionMap *)map;
  if (map->count != 0 && ir_expr_instruction_hash(instruction, &wanted)) {
    if (!mutable_map->expr_valid && !ir_expression_map_index_build(mutable_map)) {
      goto scan; /* out of memory: the walk still answers correctly */
    }
    size_t mask = map->expr_capacity - 1u;
    size_t s = wanted & mask;
    while (map->expr_slots[s]) {
      size_t idx = map->expr_slots[s] - 1u;
      if (idx < map->count && map->items[idx].alive &&
          ir_expression_entry_matches_instruction(&map->items[idx],
                                                  instruction)) {
        return (int)idx;
      }
      s = (s + 1u) & mask;
    }
    return -1;
  }

scan:
  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].alive &&
        ir_expression_entry_matches_instruction(&map->items[i], instruction)) {
      return (int)i;
    }
  }

  return -1;
}

static const IROperand *ir_expression_map_lookup(const IRExpressionMap *map,
                                                 const IRInstruction *instruction) {
  int index = ir_expression_map_find_matching_instruction(map, instruction);
  if (index < 0) {
    return NULL;
  }
  return &map->items[index].value;
}

static int ir_expression_map_store_value_for_instruction(
    IRExpressionMap *map, const IRInstruction *instruction) {
  if (!map || !instruction || !ir_instruction_is_cse_candidate(instruction) ||
      instruction->dest.kind == IR_OPERAND_NONE) {
    return 0;
  }

  /* Refuse self-referential definitions such as `@i = @i + 1`, where the
   * destination is also one of the source operands. Recording `@i + 1 -> @i`
   * is invalid: this instruction redefines @i, so the operand named in the key
   * now holds a different value than when the expression was evaluated. A later
   * textual `@i + 1` (using the new @i) would wrongly be rewritten to @i. */
  if (ir_operand_equals(&instruction->dest, &instruction->lhs) ||
      ir_operand_equals(&instruction->dest, &instruction->rhs)) {
    return 1;
  }

  int existing_index =
      ir_expression_map_find_matching_instruction(map, instruction);
  if (existing_index >= 0) {
    IROperand new_value = ir_operand_none();
    if (!ir_operand_clone(&instruction->dest, &new_value)) {
      return 0;
    }
    ir_operand_destroy(&map->items[existing_index].value);
    map->items[existing_index].value = new_value;
    ir_expression_map_note_name(map, &map->items[existing_index].value);
    /* The entry now holds its value in a symbol, so a store can reach it even
     * if neither operand named one when it was registered. */
    if (new_value.kind == IR_OPERAND_SYMBOL &&
        map->items[existing_index].lhs.kind != IR_OPERAND_SYMBOL &&
        map->items[existing_index].rhs.kind != IR_OPERAND_SYMBOL) {
      ir_expression_map_note_symbol_entry(map, (size_t)existing_index);
    }
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRExpressionEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRExpressionEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  IRExpressionEntry *entry = &map->items[map->count];
  memset(entry, 0, sizeof(*entry));
  entry->lhs = ir_operand_none();
  entry->rhs = ir_operand_none();
  entry->value = ir_operand_none();

  switch (instruction->op) {
  case IR_OP_BINARY:
    entry->kind = IR_EXPR_BINARY;
    entry->is_float = instruction->is_float;
    entry->op_text = mettle_strdup(instruction->text);
    if (!entry->op_text) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs) ||
        !ir_operand_clone(&instruction->rhs, &entry->rhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  case IR_OP_UNARY:
    entry->kind = IR_EXPR_UNARY;
    entry->is_float = instruction->is_float;
    entry->op_text = mettle_strdup(instruction->text);
    if (!entry->op_text) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  case IR_OP_CAST:
    entry->kind = IR_EXPR_CAST;
    entry->is_float = instruction->is_float;
    entry->op_text = mettle_strdup(instruction->text);
    if (!entry->op_text) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  case IR_OP_ADDRESS_OF:
    entry->kind = IR_EXPR_ADDRESS_OF;
    if (!ir_operand_clone(&instruction->lhs, &entry->lhs)) {
      ir_expression_entry_destroy(entry);
      return 0;
    }
    break;
  default:
    return 0;
  }

  if (!ir_operand_clone(&instruction->dest, &entry->value)) {
    ir_expression_entry_destroy(entry);
    return 0;
  }

  ir_expression_map_note_name(map, &entry->lhs);
  ir_expression_map_note_name(map, &entry->rhs);
  ir_expression_map_note_name(map, &entry->value);
  entry->alive = 1;
  map->count++;
  ir_expression_map_register_entry(map, map->count - 1u);
  if (map->expr_valid) {
    if ((map->count + 1u) * 2u > map->expr_capacity) {
      map->expr_valid = 0; /* rebuilt, larger, on the next lookup */
    } else {
      ir_expression_map_index_put(map, map->count - 1u);
    }
  }
  return 1;
}

/* 0 marks an empty slot, so a name hashing to 0 is nudged to 1. */
static size_t ir_expr_name_hash(const char *name) {
  size_t h = (size_t)mettle_fnv1a_hash(name);
  return h ? h : (size_t)1;
}

static void ir_expression_map_note_name(IRExpressionMap *map,
                                        const IROperand *operand) {
  if (!operand || !operand->name ||
      (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL)) {
    return;
  }
  if (map->name_hash_count * 4 >= map->name_hash_capacity * 3) {
    size_t grown = map->name_hash_capacity ? map->name_hash_capacity * 2 : 64;
    size_t *slots = (size_t *)calloc(grown, sizeof(size_t));
    if (!slots) {
      return; /* the set only ever proves absence; losing it costs speed */
    }
    for (size_t i = 0; i < map->name_hash_capacity; i++) {
      size_t h = map->name_hashes[i];
      if (!h) {
        continue;
      }
      size_t s = h & (grown - 1u);
      while (slots[s]) {
        s = (s + 1u) & (grown - 1u);
      }
      slots[s] = h;
    }
    free(map->name_hashes);
    map->name_hashes = slots;
    map->name_hash_capacity = grown;
  }
  size_t h = ir_expr_name_hash(operand->name);
  size_t mask = map->name_hash_capacity - 1u;
  size_t s = h & mask;
  while (map->name_hashes[s]) {
    if (map->name_hashes[s] == h) {
      return;
    }
    s = (s + 1u) & mask;
  }
  map->name_hashes[s] = h;
  map->name_hash_count++;
}

static int ir_expression_map_may_hold_name(const IRExpressionMap *map,
                                           const char *name) {
  if (!map->name_hashes || !name) {
    return map->count != 0;
  }
  size_t h = ir_expr_name_hash(name);
  size_t mask = map->name_hash_capacity - 1u;
  size_t s = h & mask;
  while (map->name_hashes[s]) {
    if (map->name_hashes[s] == h) {
      return 1;
    }
    s = (s + 1u) & mask;
  }
  return 0;
}
static int ir_operand_matches_named(const IROperand *operand,
                                    IROperandKind kind, const char *name) {
  if (!operand || !name || operand->kind != kind || !operand->name) {
    return 0;
  }
  return strcmp(operand->name, name) == 0;
}

static int ir_expression_entry_uses_named(const IRExpressionEntry *entry,
                                          IROperandKind kind,
                                          const char *name) {
  if (!entry || !name) {
    return 0;
  }

  if (ir_operand_matches_named(&entry->lhs, kind, name) ||
      ir_operand_matches_named(&entry->rhs, kind, name) ||
      ir_operand_matches_named(&entry->value, kind, name)) {
    return 1;
  }

  return 0;
}

static void ir_expression_map_invalidate_named(IRExpressionMap *map,
                                               IROperandKind kind,
                                               const char *name) {
  if (!map || !name) {
    return;
  }

  /* A name that has never been an operand of any entry in this map cannot
   * match one, and a fresh temp -- which is most of what a straight-line
   * function writes -- is exactly that. Skipping those turns the compaction
   * from once per instruction into once per instruction that can matter. */
  if (!ir_expression_map_may_hold_name(map, name)) {
    return;
  }

  /* The name IS an operand here, so follow its chain and clear only the
   * entries that actually name it. Walking the map instead was the other half
   * of the quadratic: a symbol written every iteration matches this guard
   * every time. */
  if (map->occ_head_capacity != 0) {
    size_t h = ir_expr_name_hash(name);
    size_t o = map->occ_heads[h & (map->occ_head_capacity - 1u)];
    while (o != 0) {
      const IRExprNameOcc *occ = &map->occ[o - 1u];
      o = occ->next;
      if (occ->name_hash != h || occ->entry_index >= map->count) {
        continue;
      }
      if (map->items[occ->entry_index].alive &&
          ir_expression_entry_uses_named(&map->items[occ->entry_index], kind,
                                         name)) {
        ir_expression_map_kill(map, occ->entry_index);
      }
    }
    ir_expression_map_maybe_compact(map);
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (map->items[i].alive &&
        ir_expression_entry_uses_named(&map->items[i], kind, name)) {
      ir_expression_map_kill(map, i);
    }
  }
  ir_expression_map_maybe_compact(map);
}

/* True if @operand names a symbol whose address is taken (and so could be
 * aliased by a store/call through a pointer). Temps, constants, and symbols
 * that never escape cannot be reached by such a store. `addr_taken` is the
 * function's precomputed address-taken set: consulting it per entry is O(1),
 * where rescanning the function per entry per store was a cubic term on
 * large functions. */
static int ir_operand_is_aliasable_symbol(const IRTempValueMap *addr_taken,
                                          const IROperand *operand) {
  return operand && operand->kind == IR_OPERAND_SYMBOL && operand->name &&
         ir_temp_value_map_lookup(addr_taken, operand->name) != NULL;
}

/* A STORE writes memory but cannot change the value of a pure arithmetic
 * expression unless one of its operands is a symbol whose address has escaped.
 * Rather than clearing the whole CSE map on every store, drop only the entries
 * that could actually be invalidated; pointer arithmetic such as `@buf + @i`
 * (over non-escaped symbols) then survives across the store in `buf[i] = ...`.
 *
 * ADDRESS_OF entries cache `&@sym`, whose value never changes, but @sym is by
 * definition address-taken, so they are conservatively dropped here exactly as
 * the old clear-on-store did -- no regression. */
static void ir_expression_map_invalidate_after_store(
    IRExpressionMap *map, const IRTempValueMap *addr_taken) {
  if (!map) {
    return;
  }

  /* Only an entry with a SYMBOL operand can be reached by a store, and those
   * are listed, so the rest of the map is not walked. */
  if (map->sym_capacity != 0) {
    for (size_t i = 0; i < map->sym_count; i++) {
      size_t idx = map->sym_entries[i];
      if (idx >= map->count || !map->items[idx].alive) {
        continue;
      }
      if (ir_operand_is_aliasable_symbol(addr_taken, &map->items[idx].lhs) ||
          ir_operand_is_aliasable_symbol(addr_taken, &map->items[idx].rhs) ||
          ir_operand_is_aliasable_symbol(addr_taken, &map->items[idx].value)) {
        ir_expression_map_kill(map, idx);
      }
    }
    ir_expression_map_maybe_compact(map);
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    if (!map->items[i].alive) {
      continue;
    }
    if (ir_operand_is_aliasable_symbol(addr_taken, &map->items[i].lhs) ||
        ir_operand_is_aliasable_symbol(addr_taken, &map->items[i].rhs) ||
        ir_operand_is_aliasable_symbol(addr_taken, &map->items[i].value)) {
      ir_expression_map_kill(map, i);
    }
  }
  ir_expression_map_maybe_compact(map);
}

int ir_common_subexpression_elimination_pass(IRFunction *function,
                                                    int *changed) {
  if (!function) {
    return 0;
  }

  IRExpressionMap map = {0};
  IRTempValueMap addr_taken;
  if (!ir_temp_value_map_init(&addr_taken) ||
      !ir_addr_taken_set_build(function, &addr_taken)) {
    ir_temp_value_map_destroy(&addr_taken);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL) {
      ir_expression_map_clear(&map);
      continue;
    }

    if (ir_instruction_writes_temp(instruction) && instruction->dest.name) {
      ir_expression_map_invalidate_named(&map, IR_OPERAND_TEMP,
                                         instruction->dest.name);
    }
    if (ir_instruction_writes_symbol(instruction) && instruction->dest.name) {
      ir_expression_map_invalidate_named(&map, IR_OPERAND_SYMBOL,
                                         instruction->dest.name);
    }

    if (ir_instruction_is_cse_candidate(instruction)) {
      const IROperand *existing = ir_expression_map_lookup(&map, instruction);
      if (existing) {
        if (!ir_rewrite_to_assign_operand(instruction, existing, changed)) {
          ir_expression_map_destroy(&map);
          ir_temp_value_map_destroy(&addr_taken);
          return 0;
        }
      } else if (!ir_expression_map_store_value_for_instruction(&map,
                                                                instruction)) {
        ir_expression_map_destroy(&map);
        ir_temp_value_map_destroy(&addr_taken);
        return 0;
      }
    }

    if (instruction->op == IR_OP_STORE) {
      ir_expression_map_invalidate_after_store(&map, &addr_taken);
    }

    if (instruction->op == IR_OP_CALL ||
        instruction->op == IR_OP_CALL_INDIRECT ||
        instruction->op == IR_OP_INLINE_ASM) {
      ir_expression_map_clear(&map);
    }

    if (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_BRANCH_ZERO ||
        instruction->op == IR_OP_BRANCH_EQ ||
        instruction->op == IR_OP_RETURN) {
      ir_expression_map_clear(&map);
    }
  }

  ir_expression_map_destroy(&map);
  ir_temp_value_map_destroy(&addr_taken);
  return 1;
}

int ir_temp_use_map_init(IRTempUseMap *map) {
  if (!map) {
    return 0;
  }

  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  map->hash = NULL;
  map->hash_count = 0;
  return 1;
}

/* Insert items[index] into the hash table (hash table must have room). The
 * caller passes the key's hash when it already had to compute one to look the
 * key up and miss, which is every insert that comes through add. */
static void ir_temp_use_map_hash_put_with(IRTempUseMap *map, size_t index,
                                          size_t hash) {
  size_t mask = map->hash_count - 1;
  size_t h = hash & mask;
  while (map->hash[h] != 0) {
    h = (h + 1) & mask;
  }
  map->hash[h] = index + 1; /* store index+1; 0 == empty */
}

static void ir_temp_use_map_hash_put(IRTempUseMap *map, size_t index) {
  ir_temp_use_map_hash_put_with(map, index,
                                mettle_fnv1a_hash(map->items[index].name));
}

/* Grow/allocate the hash table so it can hold map->count entries at <0.5 load,
 * rehashing all existing items. Returns 0 on allocation failure. */
static int ir_temp_use_map_hash_reserve(IRTempUseMap *map, size_t needed) {
  size_t target = 16;
  while (target < needed * 2) {
    target *= 2;
  }
  if (map->hash && map->hash_count >= target) {
    return 1;
  }

  size_t *new_hash = calloc(target, sizeof(size_t));
  if (!new_hash) {
    return 0;
  }
  free(map->hash);
  map->hash = new_hash;
  map->hash_count = target;
  for (size_t i = 0; i < map->count; i++) {
    ir_temp_use_map_hash_put(map, i);
  }
  return 1;
}

static int ir_temp_use_map_find_with(const IRTempUseMap *map, const char *name,
                                     size_t hash) {
  if (!map || !name || !map->hash) {
    return -1;
  }

  size_t mask = map->hash_count - 1;
  size_t h = hash & mask;
  while (map->hash[h] != 0) {
    size_t idx = map->hash[h] - 1;
    if (map->items[idx].name && map->items[idx].name[0] == name[0] &&
        strcmp(map->items[idx].name, name) == 0) {
      return (int)idx;
    }
    h = (h + 1) & mask;
  }
  return -1;
}

static int ir_temp_use_map_find(const IRTempUseMap *map, const char *name) {
  return name ? ir_temp_use_map_find_with(map, name, mettle_fnv1a_hash(name))
              : -1;
}

static int ir_temp_use_map_add(IRTempUseMap *map, const char *name) {
  size_t hash;
  int existing;

  if (!map || !name) {
    return 0;
  }

  hash = mettle_fnv1a_hash(name);
  existing = ir_temp_use_map_find_with(map, name, hash);
  if (existing >= 0) {
    map->items[existing].use_count++;
    return 1;
  }

  if (map->count >= map->capacity) {
    size_t new_capacity = map->capacity == 0 ? 16 : map->capacity * 2;
    IRTempUseEntry *new_items =
        realloc(map->items, new_capacity * sizeof(IRTempUseEntry));
    if (!new_items) {
      return 0;
    }
    map->items = new_items;
    map->capacity = new_capacity;
  }

  /* Ensure the hash can hold one more entry (rehashes existing items if so). */
  if (!ir_temp_use_map_hash_reserve(map, map->count + 1)) {
    return 0;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  map->items[map->count].name = name_copy;
  map->items[map->count].use_count = 1;
  ir_temp_use_map_hash_put_with(map, map->count, hash);
  map->count++;
  return 1;
}

size_t ir_temp_use_map_get(const IRTempUseMap *map, const char *name) {
  if (!map || !name) {
    return 0;
  }

  int index = ir_temp_use_map_find(map, name);
  if (index < 0) {
    return 0;
  }

  return map->items[index].use_count;
}

static void ir_temp_use_map_discard(IRTempUseMap *map, const char *name) {
  if (!map || !name) {
    return;
  }

  int index = ir_temp_use_map_find(map, name);
  if (index >= 0) {
    map->items[index].use_count = 0;
  }
}

void ir_temp_use_map_destroy(IRTempUseMap *map) {
  if (!map) {
    return;
  }

  for (size_t i = 0; i < map->count; i++) {
    free(map->items[i].name);
  }
  free(map->items);
  free(map->hash);
  map->items = NULL;
  map->count = 0;
  map->capacity = 0;
  map->hash = NULL;
  map->hash_count = 0;
}

static int ir_collect_operand_temp_use(IRTempUseMap *uses,
                                       const IROperand *operand) {
  if (!uses || !operand) {
    return 0;
  }

  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 1;
  }

  return ir_temp_use_map_add(uses, operand->name);
}

int ir_name_index_init(IRNameIndex *index, size_t expected) {
  size_t capacity = 16;

  if (!index) {
    return 0;
  }
  index->names = NULL;
  index->values = NULL;
  index->capacity = 0;
  while (capacity < expected * 2) {
    capacity *= 2;
  }
  index->names = (const char **)calloc(capacity, sizeof(*index->names));
  index->values = (size_t *)calloc(capacity, sizeof(*index->values));
  if (!index->names || !index->values) {
    ir_name_index_destroy(index);
    return 0;
  }
  index->capacity = capacity;
  return 1;
}

void ir_name_index_insert(IRNameIndex *index, const char *name, size_t value) {
  size_t mask;
  size_t slot;

  if (!index || !index->capacity || !name) {
    return;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (index->names[slot]) {
    if (strcmp(index->names[slot], name) == 0) {
      return;
    }
    slot = (slot + 1) & mask;
  }
  index->names[slot] = name;
  index->values[slot] = value;
}

void ir_name_index_add(IRNameIndex *index, const char *name, size_t delta) {
  size_t mask;
  size_t slot;

  if (!index || !index->capacity || !name) {
    return;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (index->names[slot]) {
    if (strcmp(index->names[slot], name) == 0) {
      index->values[slot] += delta;
      return;
    }
    slot = (slot + 1) & mask;
  }
  index->names[slot] = name;
  index->values[slot] = delta;
}

void ir_name_index_sub(IRNameIndex *index, const char *name, size_t delta) {
  size_t mask;
  size_t slot;

  if (!index || !index->capacity || !name) {
    return;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (index->names[slot]) {
    if (strcmp(index->names[slot], name) == 0) {
      index->values[slot] =
          (index->values[slot] > delta) ? index->values[slot] - delta : 0;
      return;
    }
    slot = (slot + 1) & mask;
  }
}

int ir_name_index_find(const IRNameIndex *index, const char *name,
                       size_t *out_value) {
  size_t mask;
  size_t slot;

  if (!index || !index->capacity || !name) {
    return 0;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(name) & mask;
  while (index->names[slot]) {
    if (strcmp(index->names[slot], name) == 0) {
      if (out_value) {
        *out_value = index->values[slot];
      }
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

void ir_name_index_destroy(IRNameIndex *index) {
  if (!index) {
    return;
  }
  free(index->names);
  free(index->values);
  index->names = NULL;
  index->values = NULL;
  index->capacity = 0;
}

int ir_collect_instruction_temp_uses(IRTempUseMap *uses,
                                            const IRInstruction *instruction) {
  if (!uses || !instruction) {
    return 0;
  }

  switch (instruction->op) {
  case IR_OP_STORE:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_BYTE_MAP:
  case IR_OP_SIMD_FILL:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
    if (!ir_collect_operand_temp_use(uses, &instruction->dest) ||
        !ir_collect_operand_temp_use(uses, &instruction->lhs) ||
        !ir_collect_operand_temp_use(uses, &instruction->rhs)) {
      return 0;
    }
    break;

  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_NEW:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_PREFETCH:
  case IR_OP_RETURN:
  case IR_OP_SELECT:
    if (!ir_collect_operand_temp_use(uses, &instruction->lhs) ||
        !ir_collect_operand_temp_use(uses, &instruction->rhs)) {
      return 0;
    }
    break;
  default:
    break;
  }

  /* The argument vector is always an input list, including for first-class
   * operations that do not otherwise fit the scalar opcode groups above (for
   * example TENSOR_MMA). Keep this generic so adding a neutral operation cannot
   * make dead-temp elimination erase its operand producers. */
  for (size_t i = 0; i < instruction->argument_count; i++) {
    if (!ir_collect_operand_temp_use(uses, &instruction->arguments[i])) {
      return 0;
    }
  }

  return 1;
}

int ir_eliminate_dead_temp_writes_pass(IRFunction *function,
                                              int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap live;
  if (!ir_temp_use_map_init(&live)) {
    return 0;
  }

  int crossed_label = 0;
  for (size_t i = function->instruction_count; i > 0;) {
    i--;
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL) {
      crossed_label = 1;
    }

    if (ir_instruction_writes_temp(instruction) && instruction->dest.name) {
      int dest_live = ir_temp_use_map_get(&live, instruction->dest.name) != 0;
      if (!dest_live &&
          ir_instruction_is_trivially_dead_if_dest_unused(instruction)) {
        ir_instruction_make_nop(instruction);
        if (changed) {
          *changed = 1;
        }
        continue;
      }

      if (!crossed_label) {
        ir_temp_use_map_discard(&live, instruction->dest.name);
      }
    }

    if (!ir_collect_instruction_temp_uses(&live, instruction)) {
      ir_temp_use_map_destroy(&live);
      return 0;
    }
  }

  ir_temp_use_map_destroy(&live);
  return 1;
}
