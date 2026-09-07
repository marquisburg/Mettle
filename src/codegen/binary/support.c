#include "codegen/binary/internal.h"
#include "../../common.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int binary_align_up_int(int value, int alignment, int *result_out) {
  if (!result_out || value < 0 || alignment <= 0) {
    return 0;
  }

  int remainder = value % alignment;
  if (remainder == 0) {
    *result_out = value;
    return 1;
  }
  if (value > INT_MAX - (alignment - remainder)) {
    return 0;
  }

  *result_out = value + (alignment - remainder);
  return 1;
}
int binary_code_buffer_reserve(BinaryCodeBuffer *buffer,
                                      size_t minimum_capacity) {
  if (!buffer) {
    return 0;
  }

  if (buffer->capacity >= minimum_capacity) {
    return 1;
  }

  size_t new_capacity = buffer->capacity == 0 ? 64 : buffer->capacity * 2;
  while (new_capacity < minimum_capacity) {
    new_capacity *= 2;
  }

  unsigned char *grown = realloc(buffer->data, new_capacity);
  if (!grown) {
    return 0;
  }

  buffer->data = grown;
  buffer->capacity = new_capacity;
  return 1;
}

int binary_code_buffer_append_bytes(BinaryCodeBuffer *buffer,
                                           const void *data, size_t size) {
  if (!buffer || (!data && size != 0)) {
    return 0;
  }

  if (!binary_code_buffer_reserve(buffer, buffer->size + size)) {
    return 0;
  }

  if (size != 0) {
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
  }

  return 1;
}

int binary_code_buffer_append_u8(BinaryCodeBuffer *buffer,
                                        unsigned char value) {
  return binary_code_buffer_append_bytes(buffer, &value, sizeof(value));
}

int binary_code_buffer_append_u32(BinaryCodeBuffer *buffer,
                                         uint32_t value) {
  return binary_code_buffer_append_bytes(buffer, &value, sizeof(value));
}

int binary_code_buffer_append_u64(BinaryCodeBuffer *buffer,
                                         uint64_t value) {
  return binary_code_buffer_append_bytes(buffer, &value, sizeof(value));
}

void binary_code_buffer_destroy(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return;
  }

  free(buffer->data);
  buffer->data = NULL;
  buffer->size = 0;
  buffer->capacity = 0;
}

static int binary_index_reserve(size_t **slots, size_t *slot_count,
                                size_t existing_count, size_t needed,
                                void *items,
                                const char *(*name_at)(void *, size_t)) {
  size_t target = 16;
  if (!slots || !slot_count || !name_at) {
    return 0;
  }

  while (target < needed * 2) {
    target *= 2;
  }
  if (*slots && *slot_count >= target) {
    return 1;
  }

  size_t *new_slots = calloc(target, sizeof(size_t));
  if (!new_slots) {
    return 0;
  }

  for (size_t i = 0; i < existing_count; i++) {
    const char *name = name_at(items, i);
    if (!name) {
      continue;
    }
    size_t mask = target - 1;
    size_t h = mettle_fnv1a_hash(name) & mask;
    while (new_slots[h] != 0) {
      h = (h + 1) & mask;
    }
    new_slots[h] = i + 1;
  }

  free(*slots);
  *slots = new_slots;
  *slot_count = target;
  return 1;
}

static int binary_index_find(const size_t *slots, size_t slot_count,
                             const void *items, const char *name,
                             const char *(*name_at)(void *, size_t)) {
  if (!slots || slot_count == 0 || !items || !name || !name_at) {
    return -1;
  }

  size_t mask = slot_count - 1;
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (slots[h] != 0) {
    size_t index = slots[h] - 1;
    const char *candidate = name_at((void *)items, index);
    if (candidate && strcmp(candidate, name) == 0) {
      return (int)index;
    }
    h = (h + 1) & mask;
  }
  return -1;
}

static const char *binary_named_slot_name_at(void *items, size_t index) {
  return ((BinaryNamedSlot *)items)[index].name;
}

static const char *binary_alias_name_at(void *items, size_t index) {
  return ((BinarySymbolAliasEntry *)items)[index].name;
}

static const char *binary_label_name_at(void *items, size_t index) {
  return ((BinaryLabelEntry *)items)[index].name;
}

int binary_named_slot_table_get_offset(const BinaryNamedSlotTable *table,
                                              const char *name) {
  if (!table || !name) {
    return -1;
  }

  int indexed = binary_index_find(table->slots, table->slot_count, table->items,
                                  name, binary_named_slot_name_at);
  if (indexed >= 0) {
    return table->items[indexed].offset;
  }
  if (table->slots) {
    return -1;
  }

  for (size_t i = 0; i < table->count; i++) {
    if (table->items[i].name && strcmp(table->items[i].name, name) == 0) {
      return table->items[i].offset;
    }
  }

  return -1;
}

int binary_named_slot_table_add(BinaryNamedSlotTable *table,
                                       const char *name, int offset) {
  if (!table || !name || offset <= 0) {
    return 0;
  }

  int existing = binary_named_slot_table_get_offset(table, name);
  if (existing >= 0) {
    return existing == offset;
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 8 : table->capacity * 2;
    BinaryNamedSlot *grown =
        realloc(table->items, new_capacity * sizeof(BinaryNamedSlot));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = new_capacity;
  }

  if (!binary_index_reserve(&table->slots, &table->slot_count, table->count,
                            table->count + 1, table->items,
                            binary_named_slot_name_at)) {
    return 0;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  table->items[table->count].name = name_copy;
  table->items[table->count].offset = offset;
  {
    size_t mask = table->slot_count - 1;
    size_t h = mettle_fnv1a_hash(name_copy) & mask;
    while (table->slots[h] != 0) {
      h = (h + 1) & mask;
    }
    table->slots[h] = table->count + 1;
  }
  table->count++;
  return 1;
}

void binary_named_slot_table_destroy(BinaryNamedSlotTable *table) {
  if (!table) {
    return;
  }

  for (size_t i = 0; i < table->count; i++) {
    free(table->items[i].name);
  }
  free(table->items);
  free(table->slots);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
  table->slots = NULL;
  table->slot_count = 0;
}

const char *
binary_symbol_alias_table_get(const BinarySymbolAliasTable *table,
                              const char *name) {
  if (!table || !name) {
    return NULL;
  }

  int indexed = binary_index_find(table->slots, table->slot_count, table->items,
                                  name, binary_alias_name_at);
  if (indexed >= 0) {
    return table->items[indexed].target;
  }
  if (table->slots) {
    return NULL;
  }

  for (size_t i = 0; i < table->count; i++) {
    if (table->items[i].name && strcmp(table->items[i].name, name) == 0) {
      return table->items[i].target;
    }
  }

  return NULL;
}

int binary_symbol_alias_table_add(BinarySymbolAliasTable *table,
                                         const char *name,
                                         const char *target) {
  const char *existing = NULL;
  if (!table || !name || !target || name[0] == '\0' ||
      target[0] == '\0') {
    return 0;
  }

  existing = binary_symbol_alias_table_get(table, name);
  if (existing) {
    return strcmp(existing, target) == 0;
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 8 : table->capacity * 2;
    BinarySymbolAliasEntry *grown =
        realloc(table->items, new_capacity * sizeof(BinarySymbolAliasEntry));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = new_capacity;
  }

  if (!binary_index_reserve(&table->slots, &table->slot_count, table->count,
                            table->count + 1, table->items,
                            binary_alias_name_at)) {
    return 0;
  }

  table->items[table->count].name = name;
  table->items[table->count].target = target;
  {
    size_t mask = table->slot_count - 1;
    size_t h = mettle_fnv1a_hash(name) & mask;
    while (table->slots[h] != 0) {
      h = (h + 1) & mask;
    }
    table->slots[h] = table->count + 1;
  }
  table->count++;
  return 1;
}

void binary_symbol_alias_table_destroy(BinarySymbolAliasTable *table) {
  if (!table) {
    return;
  }
  free(table->items);
  free(table->slots);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
  table->slots = NULL;
  table->slot_count = 0;
}

BinaryLabelEntry *binary_label_table_get(BinaryLabelTable *table,
                                                const char *name) {
  if (!table || !name) {
    return NULL;
  }

  int indexed = binary_index_find(table->slots, table->slot_count,
                                  table->items, name, binary_label_name_at);
  if (indexed >= 0) {
    return &table->items[indexed];
  }
  if (table->slots) {
    return NULL;
  }

  for (size_t i = 0; i < table->count; i++) {
    if (table->items[i].name && strcmp(table->items[i].name, name) == 0) {
      return &table->items[i];
    }
  }

  return NULL;
}

int binary_label_table_define(BinaryLabelTable *table, const char *name,
                                     size_t offset) {
  if (!table || !name) {
    return 0;
  }

  if (binary_label_table_get(table, name)) {
    return 0;
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 8 : table->capacity * 2;
    BinaryLabelEntry *grown =
        realloc(table->items, new_capacity * sizeof(BinaryLabelEntry));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = new_capacity;
  }

  if (!binary_index_reserve(&table->slots, &table->slot_count, table->count,
                            table->count + 1, table->items,
                            binary_label_name_at)) {
    return 0;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  table->items[table->count].name = name_copy;
  table->items[table->count].offset = offset;
  {
    size_t mask = table->slot_count - 1;
    size_t h = mettle_fnv1a_hash(name_copy) & mask;
    while (table->slots[h] != 0) {
      h = (h + 1) & mask;
    }
    table->slots[h] = table->count + 1;
  }
  table->count++;
  return 1;
}

void binary_label_table_destroy(BinaryLabelTable *table) {
  if (!table) {
    return;
  }

  for (size_t i = 0; i < table->count; i++) {
    free(table->items[i].name);
  }
  free(table->items);
  free(table->slots);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
  table->slots = NULL;
  table->slot_count = 0;
}

int binary_label_fixup_table_add(BinaryLabelFixupTable *table,
                                        const char *name,
                                        size_t displacement_offset) {
  if (!table || !name) {
    return 0;
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 8 : table->capacity * 2;
    BinaryLabelFixup *grown =
        realloc(table->items, new_capacity * sizeof(BinaryLabelFixup));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  table->items[table->count].name = name_copy;
  table->items[table->count].displacement_offset = displacement_offset;
  table->count++;
  return 1;
}

void binary_label_fixup_table_destroy(BinaryLabelFixupTable *table) {
  if (!table) {
    return;
  }

  for (size_t i = 0; i < table->count; i++) {
    free(table->items[i].name);
  }
  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

int binary_call_relocation_table_add(BinaryCallRelocationTable *table,
                                            const char *symbol_name,
                                            size_t displacement_offset) {
  if (!table || !symbol_name) {
    return 0;
  }

  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity == 0 ? 8 : table->capacity * 2;
    BinaryCallRelocation *grown =
        realloc(table->items, new_capacity * sizeof(BinaryCallRelocation));
    if (!grown) {
      return 0;
    }
    table->items = grown;
    table->capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(symbol_name);
  if (!name_copy) {
    return 0;
  }

  table->items[table->count].symbol_name = name_copy;
  table->items[table->count].displacement_offset = displacement_offset;
  table->count++;
  return 1;
}

void binary_call_relocation_table_destroy(
    BinaryCallRelocationTable *table) {
  if (!table) {
    return;
  }

  for (size_t i = 0; i < table->count; i++) {
    free(table->items[i].symbol_name);
  }
  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

void binary_offset_table_destroy(BinaryOffsetTable *table) {
  if (!table) {
    return;
  }

  free(table->items);
  table->items = NULL;
  table->count = 0;
  table->capacity = 0;
}

void binary_function_context_destroy(BinaryFunctionContext *context) {
  if (!context) {
    return;
  }

  binary_code_buffer_destroy(&context->code);
  binary_named_slot_table_destroy(&context->parameter_slots);
  binary_named_slot_table_destroy(&context->local_slots);
  binary_named_slot_table_destroy(&context->temp_slots);
  binary_named_slot_table_destroy(&context->string_symbols);
  binary_named_slot_table_destroy(&context->cstring_symbols);
  binary_named_slot_table_destroy(&context->float64_symbols);
  binary_named_slot_table_destroy(&context->address_taken_symbols);
  binary_named_slot_table_destroy(&context->register_symbols);
  binary_named_slot_table_destroy(&context->register_global_symbols);
  binary_symbol_alias_table_destroy(&context->symbol_aliases);
  binary_label_table_destroy(&context->labels);
  binary_label_fixup_table_destroy(&context->label_fixups);
  binary_call_relocation_table_destroy(&context->call_relocations);
  binary_asm_relocation_table_destroy(&context->asm_relocations);
  binary_offset_table_destroy(&context->return_fixups);
  binary_operand_type_index_destroy(&context->operand_types);
  free(context->indirect_return_slot_offsets);
  free(context->incoming_aggregate_offsets);
  free(context->indirect_temp_names);
  free(context->indirect_temp_sizes);
  free(context->runtime_end_label);
  for (size_t i = 0; i < context->debug_export_labels.count; i++) {
    free(context->debug_export_labels.items[i].name);
  }
  free(context->debug_export_labels.items);
}

int code_generator_binary_emitter_error(CodeGenerator *generator,
                                        BinaryEmitter *emitter,
                                        const char *fallback) {
  const char *reported = emitter ? binary_emitter_get_error(emitter) : NULL;

  code_generator_set_error(generator, "%s", reported ? reported : fallback);
  return 0;
}
