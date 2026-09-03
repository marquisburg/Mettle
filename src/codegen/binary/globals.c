#include "codegen/binary/internal.h"
#include "../../common.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

BinaryGlobalConstTable g_binary_global_consts = {0};
void binary_global_const_table_reset(void) {
  for (size_t i = 0; i < g_binary_global_consts.count; i++) {
    free(g_binary_global_consts.items[i].name);
  }
  free(g_binary_global_consts.items);
  free(g_binary_global_consts.slots);
  g_binary_global_consts.items = NULL;
  g_binary_global_consts.count = 0;
  g_binary_global_consts.capacity = 0;
  g_binary_global_consts.slots = NULL;
  g_binary_global_consts.slot_count = 0;
}

int binary_global_const_table_rebuild(size_t needed_count) {
  size_t slot_count = 16;
  size_t *slots = NULL;

  while (slot_count < needed_count * 2) {
    slot_count *= 2;
  }

  slots = calloc(slot_count, sizeof(size_t));
  if (!slots) {
    return 0;
  }

  for (size_t i = 0; i < g_binary_global_consts.count; i++) {
    BinaryGlobalConstEntry *entry = &g_binary_global_consts.items[i];
    if (!entry->name) {
      continue;
    }
    size_t slot = mettle_fnv1a_hash(entry->name) & (slot_count - 1);
    while (slots[slot] != 0) {
      slot = (slot + 1) & (slot_count - 1);
    }
    slots[slot] = i + 1;
  }

  free(g_binary_global_consts.slots);
  g_binary_global_consts.slots = slots;
  g_binary_global_consts.slot_count = slot_count;
  return 1;
}

static BinaryGlobalConstEntry *
binary_global_const_table_find_entry(const char *name) {
  if (!name) {
    return NULL;
  }

  if (g_binary_global_consts.slots && g_binary_global_consts.slot_count > 0) {
    size_t mask = g_binary_global_consts.slot_count - 1;
    size_t slot = mettle_fnv1a_hash(name) & mask;
    while (g_binary_global_consts.slots[slot] != 0) {
      size_t index = g_binary_global_consts.slots[slot] - 1;
      BinaryGlobalConstEntry *entry = &g_binary_global_consts.items[index];
      if (entry->name && strcmp(entry->name, name) == 0) {
        return entry;
      }
      slot = (slot + 1) & mask;
    }
    return NULL;
  }

  for (size_t i = 0; i < g_binary_global_consts.count; i++) {
    BinaryGlobalConstEntry *entry = &g_binary_global_consts.items[i];
    if (entry->name && strcmp(entry->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}

uint64_t binary_global_const_bits(long long int_value, double float_value,
                                         int is_float) {
  uint64_t bits = 0;
  if (is_float) {
    memcpy(&bits, &float_value, sizeof(bits));
  } else {
    bits = (uint64_t)int_value;
  }
  return bits;
}

int binary_global_const_table_add(const char *name, long long int_value,
                                         double float_value, int is_float,
                                         int can_inline_load) {
  if (!name) {
    return 0;
  }

  if (!g_binary_global_consts.slots ||
      ((g_binary_global_consts.count + 1) * 10 >=
       g_binary_global_consts.slot_count * 7)) {
    if (!binary_global_const_table_rebuild(g_binary_global_consts.count + 1)) {
      return 0;
    }
  }

  BinaryGlobalConstEntry *existing = binary_global_const_table_find_entry(name);
  if (existing) {
    existing->int_value = int_value;
    existing->float_value = float_value;
    existing->is_float = is_float ? 1 : 0;
    existing->bits =
        binary_global_const_bits(int_value, float_value, existing->is_float);
    existing->can_inline_load =
        (existing->is_float ? 0 : (can_inline_load ? 1 : 0));
    return 1;
  }

  if (g_binary_global_consts.count >= g_binary_global_consts.capacity) {
    size_t new_capacity =
        g_binary_global_consts.capacity ? g_binary_global_consts.capacity * 2 : 16;
    BinaryGlobalConstEntry *new_items =
        realloc(g_binary_global_consts.items,
                new_capacity * sizeof(BinaryGlobalConstEntry));
    if (!new_items) {
      return 0;
    }
    g_binary_global_consts.items = new_items;
    g_binary_global_consts.capacity = new_capacity;
  }

  char *name_copy = mettle_strdup(name);
  if (!name_copy) {
    return 0;
  }

  /* Float globals are never inline-loaded as a GP immediate: that path emits
   * `mov GP_reg, imm64`, leaving the bits in a general-purpose register, but a
   * float consumer reads its value from an XMM register. Force the RIP-relative
   * load path (which is XMM-aware) so the value reaches an XMM lane. */
  if (is_float) {
    can_inline_load = 0;
  }

  size_t index = g_binary_global_consts.count;
  g_binary_global_consts.items[index].name = name_copy;
  g_binary_global_consts.items[index].int_value = int_value;
  g_binary_global_consts.items[index].float_value = float_value;
  g_binary_global_consts.items[index].is_float = is_float ? 1 : 0;
  g_binary_global_consts.items[index].bits =
      binary_global_const_bits(int_value, float_value, is_float);
  g_binary_global_consts.items[index].can_inline_load =
      can_inline_load ? 1 : 0;
  g_binary_global_consts.count++;

  size_t slot = mettle_fnv1a_hash(name_copy) & (g_binary_global_consts.slot_count - 1);
  while (g_binary_global_consts.slots[slot] != 0) {
    slot = (slot + 1) & (g_binary_global_consts.slot_count - 1);
  }
  g_binary_global_consts.slots[slot] = index + 1;
  return 1;
}

int binary_global_const_table_get(const char *name, uint64_t *value_out) {
  if (!name || !value_out) {
    return 0;
  }

  BinaryGlobalConstEntry *entry = binary_global_const_table_find_entry(name);
  if (entry && entry->can_inline_load) {
    *value_out = entry->bits;
    return 1;
  }

  return 0;
}

BinaryIRFunctionIndex g_binary_ir_function_index = {0};
void binary_ir_function_index_reset(void) {
  free(g_binary_ir_function_index.slots);
  g_binary_ir_function_index.slots = NULL;
  g_binary_ir_function_index.slot_count = 0;
  g_binary_ir_function_index.program = NULL;
  g_binary_ir_function_index.function_count = 0;
}

void binary_ir_function_index_insert(BinaryIRFunctionIndex *index,
                                            IRFunction *function) {
  size_t mask = index->slot_count - 1;
  size_t i = mettle_fnv1a_hash(function->name) & mask;
  while (index->slots[i].name) {
    /* First definition of a given name wins, matching the old linear scan
     * which returned the earliest matching function. */
    if (strcmp(index->slots[i].name, function->name) == 0) {
      return;
    }
    i = (i + 1) & mask;
  }
  index->slots[i].name = function->name;
  index->slots[i].function = function;
}

/* Returns 1 on success (index ready to query), 0 on allocation failure (caller
 * should fall back to a linear scan rather than miss real functions). */
int binary_ir_function_index_ensure(const IRProgram *program) {
  if (g_binary_ir_function_index.program == program &&
      g_binary_ir_function_index.function_count == program->function_count &&
      g_binary_ir_function_index.slots) {
    return 1;
  }

  binary_ir_function_index_reset();

  /* Size to >=2x function count, power of two, min 16, to keep load factor
   * under 0.5 and probe chains short. */
  size_t slot_count = 16;
  while (slot_count < program->function_count * 2) {
    slot_count *= 2;
  }

  BinaryIRFunctionSlot *slots =
      calloc(slot_count, sizeof(BinaryIRFunctionSlot));
  if (!slots) {
    return 0;
  }

  g_binary_ir_function_index.slots = slots;
  g_binary_ir_function_index.slot_count = slot_count;
  g_binary_ir_function_index.program = program;
  g_binary_ir_function_index.function_count = program->function_count;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name) {
      binary_ir_function_index_insert(&g_binary_ir_function_index, function);
    }
  }

  return 1;
}

IRFunction *code_generator_find_ir_function_binary(CodeGenerator *generator,
                                                          const char *name) {
  if (!generator || !generator->ir_program || !name) {
    return NULL;
  }

  const IRProgram *program = generator->ir_program;

  if (binary_ir_function_index_ensure(program)) {
    const BinaryIRFunctionIndex *index = &g_binary_ir_function_index;
    size_t mask = index->slot_count - 1;
    size_t i = mettle_fnv1a_hash(name) & mask;
    while (index->slots[i].name) {
      if (strcmp(index->slots[i].name, name) == 0) {
        return index->slots[i].function;
      }
      i = (i + 1) & mask;
    }
    return NULL;
  }

  /* Fallback: index allocation failed; behave as before. */
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }

  return NULL;
}
/* The AST-based global-initializer evaluator that used to live here was
 * removed in Phase 2 (2.4): initializer constants are now baked onto the IR
 * module symbol table at lowering (see src/frontend/mtlc_lower_module.c), so
 * codegen reads sym->init_bits/init_is_float/init_string directly instead of
 * re-evaluating the AST. */

/* Emit a folded aggregate initializer: the bytes go to .data verbatim, then one
 * ADDR64 relocation per pointer-sized hole. A hole naming a module symbol
 * relocates against that symbol's linkage name; a hole holding a string element
 * gets its characters parked in .rdata under a generated label and relocates
 * against that. */
static int code_generator_binary_emit_global_aggregate_image(
    CodeGenerator *generator, const char *link_name, const IRModuleSymbol *sym,
    size_t alignment) {
  BinaryEmitter *emitter = code_generator_get_binary_emitter(generator);
  size_t data_section = 0;
  size_t image_offset = 0;

  data_section = binary_emitter_get_or_create_section(
      emitter, ".data", BINARY_SECTION_DATA, 0, alignment);
  if (data_section == (size_t)-1) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create .data section");
    return 0;
  }
  if (!binary_emitter_align_section(emitter, data_section, alignment, 0) ||
      !binary_emitter_append_bytes(emitter, data_section, sym->init_bytes,
                                   sym->init_bytes_size, &image_offset)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to emit aggregate global image");
    return 0;
  }

  for (size_t i = 0; i < sym->init_reloc_count; i++) {
    const IRInitReloc *reloc = &sym->init_relocs[i];
    const char *target = reloc->symbol;
    char *chars_label = NULL;

    if (!target) {
      /* A string element. The characters go to .rdata; a `string` slot then
       * points at a { chars, length } record built beside them (that is what a
       * string value is everywhere else in the language), while a `cstring`
       * slot points straight at the characters. */
      size_t rdata_section = binary_emitter_get_or_create_section(
          emitter, ".rdata", BINARY_SECTION_RDATA, 0, 8);
      size_t chars_offset = 0;
      size_t length = reloc->string ? reloc->string_length : 0;
      unsigned char terminator = 0;

      chars_label = code_generator_generate_label(generator, "str_chars");
      if (rdata_section == (size_t)-1 || !chars_label) {
        code_generator_set_error(generator,
                                 "Failed to emit aggregate string element");
        free(chars_label);
        return 0;
      }
      if (!binary_emitter_append_bytes(emitter, rdata_section,
                                       reloc->string ? reloc->string : "",
                                       length, &chars_offset) ||
          !binary_emitter_append_bytes(emitter, rdata_section, &terminator, 1,
                                       NULL) ||
          !binary_emitter_define_symbol(emitter, chars_label,
                                        BINARY_SYMBOL_LOCAL, rdata_section,
                                        chars_offset, length + 1)) {
        code_generator_set_error(generator, "%s",
                                 binary_emitter_get_error(emitter)
                                     ? binary_emitter_get_error(emitter)
                                     : "Failed to emit aggregate string bytes");
        free(chars_label);
        return 0;
      }
      target = chars_label;

      if (reloc->string_wants_record) {
        /* The field IS the {chars, length} record, sixteen bytes at the
         * field's own offset: the relocation below fills the pointer half and
         * the length is written beside it here.
         *
         * This used to emit a separate record in rodata and point the field at
         * it, which made a string field of a constant the one place a string
         * was still a pointer after the type became an ordinary aggregate.
         * Reading such a field then produced the record's ADDRESS where every
         * other site expected its contents. */
        BinarySection *image_section =
            binary_emitter_get_section(emitter, data_section);
        uint64_t encoded_length = (uint64_t)length;
        size_t length_at = image_offset + reloc->offset + 8;
        if (!image_section || !image_section->data ||
            length_at + sizeof(encoded_length) > image_section->size) {
          code_generator_set_error(
              generator, "String field does not fit its aggregate image");
          free(chars_label);
          return 0;
        }
        memcpy(image_section->data + length_at, &encoded_length,
               sizeof(encoded_length));
      }
    } else if (generator->ir_program) {
      /* Relocate against the referenced symbol's linkage name, which may
       * differ from its source name. */
      const IRModuleSymbol *referenced =
          ir_program_lookup_symbol(generator->ir_program, target);
      if (referenced && referenced->link_name && referenced->link_name[0]) {
        target = referenced->link_name;
      }
    }

    if (!binary_emitter_add_relocation(emitter, data_section,
                                       image_offset + reloc->offset,
                                       BINARY_RELOCATION_ADDR64, target, 0)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit aggregate relocation");
      free(chars_label);
      return 0;
    }
    free(chars_label);
  }

  if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                    data_section, image_offset,
                                    sym->init_bytes_size)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to define aggregate global symbol");
    return 0;
  }
  return 1;
}

int code_generator_emit_binary_global_variable(CodeGenerator *generator,
                                                      const IRModuleSymbol *sym) {
  BinaryEmitter *emitter = NULL;
  const MtlcType *type = NULL;
  const char *link_name = NULL;
  const char *section_name = NULL;
  BinarySectionKind section_kind = BINARY_SECTION_DATA;
  size_t section_index = 0;
  size_t value_offset = 0;
  size_t alignment = 1;
  int size = 0;
  unsigned char bytes[8] = {0};

  if (!generator || !sym || !sym->name) {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  type = sym->type;
  if (!type) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports scalar integer/pointer/string/"
        "float64 global variables (encountered '%s')",
        sym->name);
    return 0;
  }

  link_name = sym->link_name ? sym->link_name : sym->name;
  if (link_name[0] == '\0') {
    code_generator_set_error(generator, "Invalid global symbol '%s'",
                             sym->name);
    return 0;
  }

  if (type->kind == MTLC_TYPE_STRING) {
    if (sym->has_unfoldable_initializer) {
      code_generator_set_error(
          generator,
          "Direct object backend only supports string-literal global "
          "initializers for string globals (encountered '%s')",
          sym->name);
      return 0;
    }
    return code_generator_binary_emit_global_string_variable(
        generator, link_name, sym->has_initializer ? sym->init_string : NULL,
        sym->has_initializer ? sym->init_string_length : 0);
  }

  /* Aggregates: a global struct, array or tagged enum either carries a folded
   * initializer image (`var t: int32[4] = [1, 2, 3, 4];`, and every aggregate
   * `const`) or is plain zero-filled storage of its laid-out size. The image
   * goes to .data with its relocations; the zero case stays in .bss, where it
   * costs nothing in the object file. An all-zero image counts as the zero
   * case: since an uninitialized aggregate starts zeroed, `var wm: Fact[100];`
   * arrives here carrying five kilobytes of nothing. Handled ahead of the
   * scalar path, whose type check is shared with ABI decisions and must keep
   * rejecting them -- a tagged enum reaching it was refused outright, so a
   * program could not declare one at global scope at all. */
  if (type->kind == MTLC_TYPE_STRUCT || type->kind == MTLC_TYPE_ARRAY ||
      type->kind == MTLC_TYPE_TAGGED_ENUM) {
    size_t aggregate_size = type->size;
    size_t aggregate_alignment = type->alignment ? type->alignment : 8;
    size_t aggregate_section = 0;
    size_t aggregate_offset = 0;

    if (aggregate_size == 0) {
      code_generator_set_error(
          generator, "Global '%s' has an incomplete aggregate type",
          sym->name);
      return 0;
    }
    if (sym->has_initializer || sym->has_unfoldable_initializer) {
      /* A scalar-shaped or unfoldable initializer on an aggregate: the
       * frontend rejects these with a source location, so reaching here means
       * something slipped through rather than a user error. */
      code_generator_set_error(
          generator,
          "Aggregate global '%s' carries an initializer that was not folded to "
          "a constant image",
          sym->name);
      return 0;
    }
    if (sym->init_bytes && sym->init_bytes_size > 0 &&
        !ir_init_image_is_all_zero(sym)) {
      return code_generator_binary_emit_global_aggregate_image(
          generator, link_name, sym, aggregate_alignment);
    }

    aggregate_section = binary_emitter_get_or_create_section(
        emitter, ".bss", BINARY_SECTION_BSS, 0, aggregate_alignment);
    if (aggregate_section == (size_t)-1) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to create global aggregate section");
      return 0;
    }
    if (!binary_emitter_align_section(emitter, aggregate_section,
                                      aggregate_alignment, 0)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to align global aggregate section");
      return 0;
    }
    if (!binary_emitter_append_zeros(emitter, aggregate_section, aggregate_size,
                                     &aggregate_offset)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to reserve global aggregate storage");
      return 0;
    }
    if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                      aggregate_section, aggregate_offset,
                                      aggregate_size)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to define global aggregate symbol");
      return 0;
    }
    return 1;
  }

  /* `var p = &other;` -- a data pointer aliasing a global, or a function
   * pointer naming an entry point. The value is a link-time address, so reserve
   * a pointer-sized slot and let the linker fill it through a relocation. */
  if (sym->init_symbol_ref && sym->init_symbol_ref[0] != '\0') {
    size_t ref_section = 0;
    size_t ref_offset = 0;
    const char *target = sym->init_symbol_ref;
    const IRModuleSymbol *referenced = NULL;

    if (type->kind != MTLC_TYPE_POINTER &&
        type->kind != MTLC_TYPE_FUNCTION_POINTER) {
      code_generator_set_error(
          generator,
          "Global '%s' is initialized with an address but is not a pointer type",
          sym->name);
      return 0;
    }

    /* Relocate against the referenced symbol's linkage name, which may differ
     * from its source name (an extern with an explicit link name). */
    if (generator->ir_program) {
      referenced = ir_program_lookup_symbol(generator->ir_program, target);
      if (referenced && referenced->link_name && referenced->link_name[0]) {
        target = referenced->link_name;
      }
    }

    ref_section = binary_emitter_get_or_create_section(
        emitter, ".data", BINARY_SECTION_DATA, 0, 8);
    if (ref_section == (size_t)-1) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to create .data section");
      return 0;
    }
    if (!binary_emitter_align_section(emitter, ref_section, 8, 0) ||
        !binary_emitter_append_zeros(emitter, ref_section, 8, &ref_offset)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to reserve global pointer storage");
      return 0;
    }
    if (!binary_emitter_add_relocation(emitter, ref_section, ref_offset,
                                       BINARY_RELOCATION_ADDR64, target, 0)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit global pointer relocation");
      return 0;
    }
    if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                      ref_section, ref_offset, 8)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to define global pointer symbol");
      return 0;
    }
    return 1;
  }

  if (!code_generator_binary_resolved_type_is_supported(type, 0)) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports scalar integer/pointer/string/"
        "float64 global variables (encountered '%s')",
        sym->name);
    return 0;
  }

  size = code_generator_binary_resolved_type_scalar_size(type);
  if (size <= 0 || size > 8) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports global variables up to 8 bytes "
        "(encountered '%s')",
        sym->name);
    return 0;
  }

  if (sym->has_unfoldable_initializer) {
    code_generator_set_error(
        generator,
        "Direct object backend only supports constant numeric global "
        "initializers "
        "(encountered '%s')",
        sym->name);
    return 0;
  }

  section_kind =
      sym->has_initializer ? BINARY_SECTION_DATA : BINARY_SECTION_BSS;
  section_name = section_kind == BINARY_SECTION_DATA ? ".data" : ".bss";
  alignment = type->alignment ? type->alignment : (size_t)size;
  if (alignment == 0) {
    alignment = 1;
  }

  section_index = binary_emitter_get_or_create_section(
      emitter, section_name, section_kind, 0, alignment);
  if (section_index == (size_t)-1) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to create global variable section");
    return 0;
  }

  if (!binary_emitter_align_section(emitter, section_index, alignment, 0)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to align global variable section");
    return 0;
  }

  if (sym->has_initializer) {
    int float_bits = code_generator_binary_resolved_type_float_bits(type);
    double numeric_value = 0.0;
    if (sym->init_is_float) {
      memcpy(&numeric_value, &sym->init_bits, sizeof(numeric_value));
    } else {
      numeric_value = (double)sym->init_bits;
    }

    if (float_bits == 64) {
      memcpy(bytes, &numeric_value, sizeof(numeric_value));
    } else if (float_bits == 32) {
      float value = (float)numeric_value;
      if (type && (type->kind == MTLC_TYPE_FLOAT16 || type->kind == MTLC_TYPE_BFLOAT16)) {
        uint32_t b = 0;
        uint16_t h = 0;
        memcpy(&b, &value, 4);
        if (type->kind == MTLC_TYPE_FLOAT16) {
          h = mettle_f32bits_to_f16bits(b);
        } else {
          h = mettle_f32bits_to_bf16bits(b);
        }
        memcpy(bytes, &h, 2);
      } else {
        memcpy(bytes, &value, sizeof(value));
      }
    } else {
      if (sym->init_is_float) {
        code_generator_set_error(
            generator,
            "Direct object backend does not support floating global "
            "initializers for non-float globals (encountered '%s')",
            sym->name);
        return 0;
      }
      uint64_t encoded = (uint64_t)sym->init_bits;
      memcpy(bytes, &encoded, (size_t)size);
    }

    if (!binary_emitter_append_bytes(emitter, section_index, bytes, (size_t)size,
                                     &value_offset)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to emit global initializer");
      return 0;
    }
  } else if (!binary_emitter_append_zeros(emitter, section_index, (size_t)size,
                                          &value_offset)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to reserve global storage");
    return 0;
  }

  if (!binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                    section_index, value_offset, (size_t)size)) {
    code_generator_set_error(generator, "%s",
                             binary_emitter_get_error(emitter)
                                 ? binary_emitter_get_error(emitter)
                                 : "Failed to define global variable symbol");
    return 0;
  }

  return 1;
}

/* --- Written-global name set --------------------------------------------- *
 *
 * collect_global_constants needs to know, per candidate global, whether any
 * instruction writes it (or takes its address). Asking
 * code_generator_binary_global_is_written per global rescans every instruction
 * of every function, O(globals x instructions) with a strcmp inside, which
 * dominated codegen once a frontend with many string literals pushed module
 * globals into the tens of thousands. Build the set of written names in one
 * pass instead and answer each query from the table (the same cure
 * BinaryIRFunctionIndex applies to function lookup; see internal.h). Names are
 * borrowed from the IR, which outlives the set. */
typedef struct {
  const char **slots; /* open addressing; NULL = empty */
  size_t slot_count;  /* power of two, 0 until first insert */
  size_t count;
} BinaryWrittenSet;

static int binary_written_set_grow(BinaryWrittenSet *set) {
  size_t new_count = set->slot_count ? set->slot_count * 2 : 1024;
  const char **new_slots = calloc(new_count, sizeof(*new_slots));
  if (!new_slots) {
    return 0;
  }
  for (size_t i = 0; i < set->slot_count; i++) {
    const char *name = set->slots[i];
    if (!name) {
      continue;
    }
    size_t h = mettle_fnv1a_hash(name) & (new_count - 1);
    while (new_slots[h]) {
      h = (h + 1) & (new_count - 1);
    }
    new_slots[h] = name;
  }
  free(set->slots);
  set->slots = new_slots;
  set->slot_count = new_count;
  return 1;
}

static int binary_written_set_add(BinaryWrittenSet *set, const char *name) {
  if (!name) {
    return 1;
  }
  /* grow at 50% load so probe chains stay short */
  if (set->slot_count == 0 || set->count * 2 >= set->slot_count) {
    if (!binary_written_set_grow(set)) {
      return 0;
    }
  }
  size_t mask = set->slot_count - 1;
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (set->slots[h]) {
    if (strcmp(set->slots[h], name) == 0) {
      return 1;
    }
    h = (h + 1) & mask;
  }
  set->slots[h] = name;
  set->count++;
  return 1;
}

static int binary_written_set_contains(const BinaryWrittenSet *set,
                                       const char *name) {
  if (!name || set->slot_count == 0) {
    return 0;
  }
  size_t mask = set->slot_count - 1;
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (set->slots[h]) {
    if (strcmp(set->slots[h], name) == 0) {
      return 1;
    }
    h = (h + 1) & mask;
  }
  return 0;
}

/* Collect every symbol name the program writes or takes the address of,
 * the same three cases code_generator_binary_global_is_written tests. */
static int binary_written_set_build(BinaryWrittenSet *set,
                                    const IRProgram *ir_program) {
  for (size_t fn_i = 0; fn_i < ir_program->function_count; fn_i++) {
    const IRFunction *function = ir_program->functions[fn_i];
    if (!function) {
      continue;
    }
    for (size_t insn_i = 0; insn_i < function->instruction_count; insn_i++) {
      const IRInstruction *instruction = &function->instructions[insn_i];
      if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
          instruction->dest.name) {
        if (!binary_written_set_add(set, instruction->dest.name)) {
          return 0;
        }
      }
      if (instruction->op == IR_OP_ADDRESS_OF &&
          instruction->lhs.kind == IR_OPERAND_SYMBOL &&
          instruction->lhs.name) {
        if (!binary_written_set_add(set, instruction->lhs.name)) {
          return 0;
        }
      }
      /* An asm block is opaque, so every global it binds counts as written. */
      if (instruction->op == IR_OP_INLINE_ASM && instruction->text) {
        for (size_t sym_i = 0; sym_i < ir_program->module_symbol_count;
             sym_i++) {
          const IRModuleSymbol *sym = &ir_program->module_symbols[sym_i];
          if (sym->kind != IR_MODSYM_VARIABLE || !sym->name) {
            continue;
          }
          if (ir_inline_asm_binds_symbol(instruction->text, sym->name) &&
              !binary_written_set_add(set, sym->name)) {
            return 0;
          }
        }
      }
    }
  }
  return 1;
}

int code_generator_binary_global_is_written(IRProgram *ir_program,
                                                   const char *name) {
  if (!ir_program || !name) {
    return 1;
  }

  for (size_t fn_i = 0; fn_i < ir_program->function_count; fn_i++) {
    IRFunction *function = ir_program->functions[fn_i];
    if (!function) {
      continue;
    }

    for (size_t insn_i = 0; insn_i < function->instruction_count; insn_i++) {
      IRInstruction *instruction = &function->instructions[insn_i];
      if (!instruction) {
        continue;
      }

      if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
          instruction->dest.name && strcmp(instruction->dest.name, name) == 0) {
        return 1;
      }
      if (instruction->op == IR_OP_ADDRESS_OF &&
          instruction->lhs.kind == IR_OPERAND_SYMBOL &&
          instruction->lhs.name && strcmp(instruction->lhs.name, name) == 0) {
        return 1;
      }
      if (instruction->op == IR_OP_STORE &&
          instruction->dest.kind == IR_OPERAND_SYMBOL &&
          instruction->dest.name && strcmp(instruction->dest.name, name) == 0) {
        return 1;
      }
      if (instruction->op == IR_OP_INLINE_ASM &&
          ir_inline_asm_binds_symbol(instruction->text, name)) {
        return 1;
      }
    }
  }

  return 0;
}

int code_generator_binary_collect_global_constants(CodeGenerator *generator) {
  BinaryWrittenSet written = {0};

  if (!generator || !generator->ir_program) {
    return 0;
  }

  if (!binary_written_set_build(&written, generator->ir_program)) {
    free(written.slots);
    code_generator_set_error(generator,
                             "Out of memory while scanning written globals");
    return 0;
  }

  for (size_t i = 0; i < generator->ir_program->module_symbol_count; i++) {
    const IRModuleSymbol *sym = &generator->ir_program->module_symbols[i];
    if (sym->kind != IR_MODSYM_VARIABLE || sym->is_extern ||
        !sym->has_initializer || sym->init_string) {
      continue; /* extern, no initializer, or a string initializer */
    }
    /* An exported global is reachable from outside this compilation, so a scan
     * of this program's writes proves nothing about its value. Folding its
     * initializer into the reads made `export var SHARED: int64 = 7;` answer 7
     * forever: a C caller that stored 99 saw its own store and Mettle's reader
     * still returned the constant. */
    if (sym->is_exported) {
      continue;
    }

    const MtlcType *type = sym->type;
    if (!type || !code_generator_binary_resolved_type_is_supported(type, 0) ||
        type->kind == MTLC_TYPE_STRING || type->kind == MTLC_TYPE_VOID ||
        type->size == 0 || type->size > 8) {
      continue;
    }

    double float_value = 0.0;
    long long int_value = sym->init_bits;
    if (sym->init_is_float) {
      memcpy(&float_value, &sym->init_bits, sizeof(float_value));
      int_value = (long long)float_value;
    }

    /* Same predicate code_generator_binary_global_is_written applies, but
     * answered from the precomputed set. A NULL name counts as written. */
    int is_const = sym->name && !binary_written_set_contains(&written, sym->name);
    if (!binary_global_const_table_add(sym->name, int_value, float_value,
                                       sym->init_is_float, is_const)) {
      code_generator_set_error(
          generator, "Out of memory while tracking constant global '%s'",
          sym->name);
      free(written.slots);
      return 0;
    }
  }

  free(written.slots);
  return 1;
}

int code_generator_declare_binary_externs(CodeGenerator *generator) {
  BinaryEmitter *emitter = NULL;

  if (!generator || !generator->ir_program) {
    return 0;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    return 0;
  }

  for (size_t i = 0; i < generator->ir_program->module_symbol_count; i++) {
    const IRModuleSymbol *sym = &generator->ir_program->module_symbols[i];
    const char *extern_name = NULL;

    if (!sym->is_extern || !sym->name) {
      continue;
    }
    extern_name = sym->link_name ? sym->link_name : sym->name;

    if (code_generator_binary_active_abi()->shadow_space_size > 0 &&
        (strcmp(extern_name, "malloc") == 0 ||
         strcmp(extern_name, "calloc") == 0 ||
         strcmp(extern_name, "realloc") == 0 ||
         strcmp(extern_name, "free") == 0 ||
         strcmp(extern_name, "memset") == 0 ||
         strcmp(extern_name, "memcpy") == 0 ||
         strcmp(extern_name, "memmove") == 0 ||
         strcmp(extern_name, "memcmp") == 0)) {
      continue;
    }

    if (!binary_emitter_declare_external(emitter, extern_name)) {
      code_generator_set_error(generator, "%s",
                               binary_emitter_get_error(emitter)
                                   ? binary_emitter_get_error(emitter)
                                   : "Failed to declare external symbol");
      return 0;
    }
  }

  return 1;
}

