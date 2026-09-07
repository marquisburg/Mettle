#include "codegen/binary/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- --debug-hooks struct layout tables ------------------------------------
 * The debug runtime expands struct/array/pointer variables by name: it strips
 * `*`/`[N]` suffixes off a type-name string and looks the base up in these
 * tables. So every emitted field type must be a name the runtime can resolve
 * the same way; dbg_type_display_name reconstructs `Point*` / `int32[64]`
 * spellings for derived types whose MtlcType nodes carry no name. */

static int dbg_type_display_name(MtlcType *type, char *buffer, size_t cap) {
  if (!type || cap == 0) {
    return 0;
  }
  switch (type->kind) {
  case MTLC_TYPE_POINTER: {
    char inner[120];
    if (!dbg_type_display_name(type->base_type, inner, sizeof(inner))) {
      return 0;
    }
    snprintf(buffer, cap, "%s*", inner);
    return 1;
  }
  case MTLC_TYPE_ARRAY: {
    char inner[112];
    if (!dbg_type_display_name(type->base_type, inner, sizeof(inner))) {
      return 0;
    }
    snprintf(buffer, cap, "%s[%zu]", inner, type->array_size);
    return 1;
  }
  default:
    snprintf(buffer, cap, "%s", type->name ? type->name : "?");
    return 1;
  }
}

/* Unwrap pointers/arrays to the underlying named struct, or NULL. */
static MtlcType *dbg_underlying_struct(MtlcType *type) {
  int guard = 0;
  while (type && guard++ < 8 &&
         (type->kind == MTLC_TYPE_POINTER || type->kind == MTLC_TYPE_ARRAY)) {
    type = type->base_type;
  }
  return (type && type->kind == MTLC_TYPE_STRUCT && type->name) ? type : NULL;
}

static void dbg_collect_structs(MtlcType *type, MtlcType **list, size_t *count,
                                size_t cap) {
  MtlcType *s = dbg_underlying_struct(type);
  if (!s) {
    return;
  }
  for (size_t i = 0; i < *count; i++) {
    if (list[i] == s || (list[i]->name && strcmp(list[i]->name, s->name) == 0)) {
      return;
    }
  }
  if (*count >= cap) {
    return;
  }
  list[(*count)++] = s;
  for (size_t f = 0; f < s->field_count; f++) {
    dbg_collect_structs(s->field_types ? s->field_types[f] : NULL, list, count,
                        cap);
  }
}

static int dbg_emit_u64_table(BinaryEmitter *emitter, size_t rdata_section,
                              const char *symbol, const uint64_t *values,
                              size_t count) {
  /* A zero-length table still defines the symbol (one dummy slot) so the
   * debug runtime's externs always link. */
  size_t slots = count > 0 ? count : 1u;
  uint64_t dummy = 0;
  size_t offset = 0;
  if (!binary_emitter_align_section(emitter, rdata_section, 8, 0) ||
      !binary_emitter_append_bytes(emitter, rdata_section,
                                   count > 0 ? (const void *)values
                                             : (const void *)&dummy,
                                   slots * sizeof(uint64_t), &offset) ||
      !binary_emitter_define_symbol(emitter, symbol, BINARY_SYMBOL_GLOBAL,
                                    rdata_section, offset,
                                    slots * sizeof(uint64_t))) {
    return 0;
  }
  return 1;
}

static int code_generator_binary_emit_profile_cstring_table(
    CodeGenerator *generator, BinaryEmitter *emitter, size_t rdata_section,
    const char *table_symbol, const char *const *values, size_t count,
    char **out_name_symbols) {
  size_t table_offset = 0;

  if (!binary_emitter_align_section(emitter, rdata_section, 8, 0)) {
    return 0;
  }

  if (!binary_emitter_append_zeros(emitter, rdata_section, count * sizeof(uint64_t),
                                   &table_offset) ||
      !binary_emitter_define_symbol(emitter, table_symbol, BINARY_SYMBOL_GLOBAL,
                                    rdata_section, table_offset,
                                    count * sizeof(uint64_t))) {
    return 0;
  }

  for (size_t i = 0; i < count; i++) {
    const char *value = values[i] ? values[i] : "?";
    size_t literal_offset = 0;
    size_t length = strlen(value);
    unsigned char terminator = 0;
    int written = 0;

    written = snprintf(NULL, 0, "mettleprof_%s_%zu", table_symbol, i);
    if (written <= 0) {
      return 0;
    }

    out_name_symbols[i] = malloc((size_t)written + 1u);
    if (!out_name_symbols[i]) {
      return 0;
    }
    snprintf(out_name_symbols[i], (size_t)written + 1u, "mettleprof_%s_%zu",
             table_symbol, i);

    if (!binary_emitter_append_bytes(emitter, rdata_section, value, length,
                                     &literal_offset) ||
        !binary_emitter_append_bytes(emitter, rdata_section, &terminator, 1,
                                     NULL) ||
        !binary_emitter_define_symbol(emitter, out_name_symbols[i],
                                      BINARY_SYMBOL_LOCAL, rdata_section,
                                      literal_offset, length + 1) ||
        !binary_emitter_add_relocation(
            emitter, rdata_section, table_offset + i * sizeof(uint64_t),
            BINARY_RELOCATION_ADDR64, out_name_symbols[i], 0)) {
      return 0;
    }
  }

  return 1;
}

int code_generator_binary_emit_profile_tables(CodeGenerator *generator) {
  BinaryEmitter *emitter = NULL;
  IRProgram *program = NULL;
  size_t rdata_section = 0;
  size_t data_section = 0;
  size_t function_count = 0;
  char **name_symbols = NULL;
  char **file_symbols = NULL;
  const char **profile_names = NULL;
  const char **profile_files = NULL;
  uint64_t *profile_lines = NULL;

  /* The debugger reuses the profile registry and these embedded tables for
   * its function/file/line metadata, so --debug-hooks emits them too. */
  if (!generator || (!generator->profile_runtime && !generator->debug_hooks) ||
      !generator->ir_program) {
    return 1;
  }

  program = generator->ir_program;
  function_count = program->profile_entry_count;
  if (function_count == 0) {
    return 1;
  }

  profile_names = calloc(function_count, sizeof(const char *));
  profile_files = calloc(function_count, sizeof(const char *));
  profile_lines = calloc(function_count, sizeof(uint64_t));
  name_symbols = calloc(function_count, sizeof(char *));
  file_symbols = calloc(function_count, sizeof(char *));
  if (!profile_names || !profile_files || !profile_lines || !name_symbols ||
      !file_symbols) {
    free(profile_names);
    free(profile_files);
    free(profile_lines);
    free(name_symbols);
    free(file_symbols);
    code_generator_set_error(generator,
                             "Out of memory while emitting profile tables");
    return 0;
  }

  for (size_t i = 0; i < function_count; i++) {
    IRProfileEntry *entry = &program->profile_entries[i];
    profile_names[i] = entry->name ? entry->name : "?";
    profile_files[i] = entry->filename ? entry->filename : "?";
    profile_lines[i] = entry->line;
  }

  emitter = code_generator_get_binary_emitter(generator);
  if (!emitter) {
    code_generator_set_error(generator, "Binary emitter is not initialized");
    goto fail;
  }

  rdata_section = binary_emitter_get_or_create_section(
      emitter, ".rdata", BINARY_SECTION_RDATA, 0, 8);
  if (rdata_section == (size_t)-1) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to create .rdata section");
    goto fail;
  }

  if (!code_generator_binary_emit_profile_cstring_table(
          generator, emitter, rdata_section, "mettle_profile_names",
          profile_names, function_count, name_symbols) ||
      !code_generator_binary_emit_profile_cstring_table(
          generator, emitter, rdata_section, "mettle_profile_files",
          profile_files, function_count, file_symbols)) {
    if (!generator->has_error) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit profile string tables");
    }
    goto fail;
  }

  {
    size_t lines_offset = 0;
    if (!binary_emitter_align_section(emitter, rdata_section, 8, 0) ||
        !binary_emitter_append_bytes(emitter, rdata_section, profile_lines,
                                     function_count * sizeof(uint64_t),
                                     &lines_offset) ||
        !binary_emitter_define_symbol(emitter, "mettle_profile_lines",
                                      BINARY_SYMBOL_GLOBAL, rdata_section,
                                      lines_offset,
                                      function_count * sizeof(uint64_t))) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit profile line table");
      goto fail;
    }
  }

  data_section = binary_emitter_get_or_create_section(
      emitter, ".data", BINARY_SECTION_DATA, 0, 8);
  if (data_section == (size_t)-1) {
    code_generator_binary_emitter_error(
        generator, emitter, "Failed to create .data section");
    goto fail;
  }

  {
    uint64_t count_value = (uint64_t)function_count;
    size_t count_offset = 0;

    if (!binary_emitter_append_bytes(emitter, data_section, &count_value,
                                     sizeof(count_value), &count_offset) ||
        !binary_emitter_define_symbol(emitter, "mettle_profile_name_count",
                                      BINARY_SYMBOL_GLOBAL, data_section,
                                      count_offset, sizeof(count_value))) {
      code_generator_binary_emitter_error(
        generator, emitter, "Failed to emit profile name count");
      goto fail;
    }
  }

  for (size_t i = 0; i < function_count; i++) {
    free(name_symbols[i]);
    free(file_symbols[i]);
  }
  free(name_symbols);
  free(file_symbols);
  free(profile_names);
  free(profile_files);
  free(profile_lines);

  /* --debug-hooks: the per-variable registration tables (indexed by the
   * local_id the mettle_dbg_local hook passes) and the struct layout tables
   * the runtime uses to expand struct/array/pointer variables. All symbols
   * are ALWAYS defined (possibly empty) so debug.o's externs link even for
   * programs with no variables or no structs. */
  if (generator->debug_hooks) {
    enum { DBG_MAX_STRUCTS = 256 };
    size_t local_count = program->debug_local_entry_count;
    size_t emit_locals = local_count > 0 ? local_count : 1u;
    const char **local_names = calloc(emit_locals, sizeof(const char *));
    const char **local_types = calloc(emit_locals, sizeof(const char *));
    char **scratch_a = calloc(emit_locals, sizeof(char *));
    char **scratch_b = calloc(emit_locals, sizeof(char *));
    MtlcType **structs = calloc(DBG_MAX_STRUCTS, sizeof(MtlcType *));
    size_t struct_count = 0;
    int ok = local_names && local_types && scratch_a && scratch_b && structs;

    if (ok) {
      local_names[0] = "";
      local_types[0] = "";
      for (size_t i = 0; i < local_count; i++) {
        local_names[i] = program->debug_local_entries[i].name;
        local_types[i] = program->debug_local_entries[i].type_name;
        dbg_collect_structs(
            code_generator_binary_get_resolved_type(
                generator, program->debug_local_entries[i].type_name, 0),
            structs, &struct_count, DBG_MAX_STRUCTS);
      }
      ok = code_generator_binary_emit_profile_cstring_table(
               generator, emitter, rdata_section, "mettle_dbg_local_names",
               local_names, emit_locals, scratch_a) &&
           code_generator_binary_emit_profile_cstring_table(
               generator, emitter, rdata_section, "mettle_dbg_local_types",
               local_types, emit_locals, scratch_b);
      for (size_t i = 0; i < emit_locals; i++) {
        free(scratch_a[i]);
        free(scratch_b[i]);
        scratch_a[i] = NULL;
        scratch_b[i] = NULL;
      }
    }

    /* Struct layouts, flattened: per struct a [start, count) window into the
     * parallel field arrays. Field type names are reconstructed spellings
     * (`Point*`, `int32[8]`) the runtime can re-resolve. */
    size_t field_total = 0;
    for (size_t s = 0; s < struct_count; s++) {
      field_total += structs[s]->field_count;
    }
    size_t emit_structs = struct_count > 0 ? struct_count : 1u;
    size_t emit_fields = field_total > 0 ? field_total : 1u;
    const char **struct_names = NULL;
    uint64_t *struct_sizes = NULL, *field_starts = NULL, *field_counts = NULL;
    const char **field_names = NULL;
    char **field_type_names = NULL; /* owned display-name strings */
    uint64_t *field_offsets = NULL;
    char **scratch_c = NULL, **scratch_d = NULL, **scratch_e = NULL;
    if (ok) {
      struct_names = calloc(emit_structs, sizeof(const char *));
      struct_sizes = calloc(emit_structs, sizeof(uint64_t));
      field_starts = calloc(emit_structs, sizeof(uint64_t));
      field_counts = calloc(emit_structs, sizeof(uint64_t));
      field_names = calloc(emit_fields, sizeof(const char *));
      field_type_names = calloc(emit_fields, sizeof(char *));
      field_offsets = calloc(emit_fields, sizeof(uint64_t));
      scratch_c = calloc(emit_structs, sizeof(char *));
      scratch_d = calloc(emit_fields, sizeof(char *));
      scratch_e = calloc(emit_fields, sizeof(char *));
      ok = struct_names && struct_sizes && field_starts && field_counts &&
           field_names && field_type_names && field_offsets && scratch_c &&
           scratch_d && scratch_e;
    }
    if (ok) {
      struct_names[0] = "";
      field_names[0] = "";
      size_t cursor = 0;
      for (size_t s = 0; s < struct_count && ok; s++) {
        MtlcType *st = structs[s];
        struct_names[s] = st->name;
        struct_sizes[s] = (uint64_t)st->size;
        field_starts[s] = (uint64_t)cursor;
        field_counts[s] = (uint64_t)st->field_count;
        for (size_t f = 0; f < st->field_count && ok; f++) {
          char display[128];
          field_names[cursor] =
              st->field_names && st->field_names[f] ? st->field_names[f] : "?";
          field_offsets[cursor] =
              st->field_offsets ? (uint64_t)st->field_offsets[f] : 0;
          if (!dbg_type_display_name(
                  st->field_types ? st->field_types[f] : NULL, display,
                  sizeof(display))) {
            snprintf(display, sizeof(display), "?");
          }
          field_type_names[cursor] = strdup(display);
          ok = field_type_names[cursor] != NULL;
          cursor++;
        }
      }
      if (ok) {
        ok = code_generator_binary_emit_profile_cstring_table(
                 generator, emitter, rdata_section, "mettle_dbg_struct_names",
                 struct_names, emit_structs, scratch_c) &&
             code_generator_binary_emit_profile_cstring_table(
                 generator, emitter, rdata_section, "mettle_dbg_field_names",
                 field_names, emit_fields, scratch_d) &&
             code_generator_binary_emit_profile_cstring_table(
                 generator, emitter, rdata_section, "mettle_dbg_field_types",
                 (const char *const *)field_type_names, emit_fields,
                 scratch_e) &&
             dbg_emit_u64_table(emitter, rdata_section,
                                "mettle_dbg_struct_sizes", struct_sizes,
                                emit_structs) &&
             dbg_emit_u64_table(emitter, rdata_section,
                                "mettle_dbg_struct_field_start", field_starts,
                                emit_structs) &&
             dbg_emit_u64_table(emitter, rdata_section,
                                "mettle_dbg_struct_field_count", field_counts,
                                emit_structs) &&
             dbg_emit_u64_table(emitter, rdata_section,
                                "mettle_dbg_field_offsets", field_offsets,
                                emit_fields);
      }
    }
    if (ok) {
      uint64_t counts[2] = {(uint64_t)local_count, (uint64_t)struct_count};
      size_t count_offset = 0;
      ok = binary_emitter_append_bytes(emitter, data_section, &counts[0],
                                       sizeof(counts[0]), &count_offset) &&
           binary_emitter_define_symbol(emitter, "mettle_dbg_local_count",
                                        BINARY_SYMBOL_GLOBAL, data_section,
                                        count_offset, sizeof(counts[0])) &&
           binary_emitter_append_bytes(emitter, data_section, &counts[1],
                                       sizeof(counts[1]), &count_offset) &&
           binary_emitter_define_symbol(emitter, "mettle_dbg_struct_count",
                                        BINARY_SYMBOL_GLOBAL, data_section,
                                        count_offset, sizeof(counts[1]));
    }

    if (scratch_c) {
      for (size_t i = 0; i < emit_structs; i++) free(scratch_c[i]);
    }
    if (scratch_d) {
      for (size_t i = 0; i < emit_fields; i++) free(scratch_d[i]);
    }
    if (scratch_e) {
      for (size_t i = 0; i < emit_fields; i++) free(scratch_e[i]);
    }
    if (field_type_names) {
      for (size_t i = 0; i < emit_fields; i++) free(field_type_names[i]);
    }
    free(scratch_c);
    free(scratch_d);
    free(scratch_e);
    free(struct_names);
    free(struct_sizes);
    free(field_starts);
    free(field_counts);
    free(field_names);
    free(field_type_names);
    free(field_offsets);
    free(scratch_a);
    free(scratch_b);
    free(local_names);
    free(local_types);
    free(structs);
    if (!ok) {
      if (!generator->has_error) {
        code_generator_set_error(generator, "Failed to emit debug tables");
      }
      return 0;
    }
  }
  return 1;

fail:
  if (name_symbols) {
    for (size_t i = 0; i < function_count; i++) {
      free(name_symbols[i]);
    }
    free(name_symbols);
  }
  if (file_symbols) {
    for (size_t i = 0; i < function_count; i++) {
      free(file_symbols[i]);
    }
    free(file_symbols);
  }
  free(profile_names);
  free(profile_files);
  free(profile_lines);
  return 0;
}
