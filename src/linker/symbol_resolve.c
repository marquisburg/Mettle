#include "linker/symbol_resolve.h"
#include "linker/linker_common.h"
#include "linker/unresolved_hint.h"
#include "../common.h"

/* Section merge and symbol resolution for COFF produced by the object backend;
 * pair with relocation.c and see docs/linker-build-pipelines.md for pipeline
 * triage (internal link vs external gcc). */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SCN_ALIGN_MASK 0x00F00000u
#define IMAGE_SCN_ALIGN_1BYTES 0x00100000u
#define IMAGE_SCN_ALIGN_2BYTES 0x00200000u
#define IMAGE_SCN_ALIGN_4BYTES 0x00300000u
#define IMAGE_SCN_ALIGN_8BYTES 0x00400000u
#define IMAGE_SCN_ALIGN_16BYTES 0x00500000u
#define IMAGE_SCN_ALIGN_32BYTES 0x00600000u
#define IMAGE_SCN_ALIGN_64BYTES 0x00700000u
#define IMAGE_SCN_ALIGN_128BYTES 0x00800000u
#define IMAGE_SCN_ALIGN_256BYTES 0x00900000u
#define IMAGE_SCN_ALIGN_512BYTES 0x00A00000u
#define IMAGE_SCN_ALIGN_1024BYTES 0x00B00000u
#define IMAGE_SCN_ALIGN_2048BYTES 0x00C00000u
#define IMAGE_SCN_ALIGN_4096BYTES 0x00D00000u
#define IMAGE_SCN_ALIGN_8192BYTES 0x00E00000u


static size_t link_section_index_from_kind(LinkSectionKind kind) {
  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
    return 0u;
  case LINK_SECTION_KIND_RDATA:
    return 1u;
  case LINK_SECTION_KIND_DATA:
    return 2u;
  case LINK_SECTION_KIND_BSS:
    return 3u;
  case LINK_SECTION_KIND_PDATA:
    return 4u;
  case LINK_SECTION_KIND_XDATA:
    return 5u;
  case LINK_SECTION_KIND_TLS:
    return 6u;
  case LINK_SECTION_KIND_UNKNOWN:
  default:
    return LINKED_SECTION_INDEX_NONE;
  }
}

static const char *link_section_name_from_kind(LinkSectionKind kind) {
  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
    return ".text";
  case LINK_SECTION_KIND_RDATA:
    return ".rdata";
  case LINK_SECTION_KIND_DATA:
    return ".data";
  case LINK_SECTION_KIND_BSS:
    return ".bss";
  case LINK_SECTION_KIND_PDATA:
    return ".pdata";
  case LINK_SECTION_KIND_XDATA:
    return ".xdata";
  case LINK_SECTION_KIND_TLS:
    return ".tls";
  case LINK_SECTION_KIND_UNKNOWN:
  default:
    return "<unknown>";
  }
}

static size_t link_alignment_from_characteristics(uint32_t characteristics) {
  switch (characteristics & IMAGE_SCN_ALIGN_MASK) {
  case IMAGE_SCN_ALIGN_1BYTES:
    return 1u;
  case IMAGE_SCN_ALIGN_2BYTES:
    return 2u;
  case IMAGE_SCN_ALIGN_4BYTES:
    return 4u;
  case IMAGE_SCN_ALIGN_8BYTES:
    return 8u;
  case IMAGE_SCN_ALIGN_16BYTES:
    return 16u;
  case IMAGE_SCN_ALIGN_32BYTES:
    return 32u;
  case IMAGE_SCN_ALIGN_64BYTES:
    return 64u;
  case IMAGE_SCN_ALIGN_128BYTES:
    return 128u;
  case IMAGE_SCN_ALIGN_256BYTES:
    return 256u;
  case IMAGE_SCN_ALIGN_512BYTES:
    return 512u;
  case IMAGE_SCN_ALIGN_1024BYTES:
    return 1024u;
  case IMAGE_SCN_ALIGN_2048BYTES:
    return 2048u;
  case IMAGE_SCN_ALIGN_4096BYTES:
    return 4096u;
  case IMAGE_SCN_ALIGN_8192BYTES:
    return 8192u;
  default:
    return 0u;
  }
}

static size_t link_default_section_alignment(LinkSectionKind kind,
                                             size_t fallback_alignment) {
  if (fallback_alignment > 1u) {
    return fallback_alignment;
  }

  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
  case LINK_SECTION_KIND_RDATA:
  case LINK_SECTION_KIND_DATA:
  case LINK_SECTION_KIND_BSS:
  case LINK_SECTION_KIND_PDATA:
  case LINK_SECTION_KIND_XDATA:
    return 16u;
  case LINK_SECTION_KIND_UNKNOWN:
  default:
    return 1u;
  }
}

static size_t link_section_alignment(const LinkSection *section,
                                     size_t fallback_alignment) {
  size_t alignment = 0;

  if (!section) {
    return 1u;
  }

  alignment = (size_t)section->alignment;
  if (alignment > 1u) {
    return alignment;
  }

  return link_default_section_alignment(section->kind, fallback_alignment);
}

static int link_section_reserve_data(LinkedSection *section, size_t minimum_size,
                                     char **error_message_out) {
  unsigned char *grown = NULL;
  size_t new_capacity = 0;

  if (!section) {
    return 0;
  }
  if (minimum_size <= section->data_capacity) {
    return 1;
  }

  new_capacity = section->data_capacity ? section->data_capacity : 64u;
  while (new_capacity < minimum_size) {
    new_capacity *= 2u;
  }

  grown = realloc(section->data, new_capacity);
  if (!grown) {
    mettle_set_error(error_message_out,
                              "Out of memory while growing merged section '%s'",
                              section->name);
    return 0;
  }

  section->data = grown;
  section->data_capacity = new_capacity;
  return 1;
}

static int link_section_reserve_contributions(LinkedSection *section,
                                              size_t minimum_count,
                                              char **error_message_out) {
  LinkedSectionContribution *grown = NULL;
  size_t new_capacity = 0;

  if (!section) {
    return 0;
  }
  if (section->contribution_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = section->contribution_capacity ? section->contribution_capacity
                                                : 4u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(section->contributions,
                  new_capacity * sizeof(LinkedSectionContribution));
  if (!grown) {
    mettle_set_error(
        error_message_out,
        "Out of memory while recording contributions for merged section '%s'",
        section->name);
    return 0;
  }

  section->contributions = grown;
  section->contribution_capacity = new_capacity;
  return 1;
}

static size_t link_estimate_section_size(const LinkObject *object,
                                         size_t section_index) {
  const LinkSection *section = NULL;
  size_t size = 0;
  size_t i = 0;
  int saw_symbol = 0;

  if (!object || section_index >= object->section_count) {
    return 0u;
  }

  section = &object->sections[section_index];
  for (i = 0; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];

    if (symbol->is_auxiliary || symbol->aux_section_length == 0u ||
        !symbol->name) {
      continue;
    }
    if (symbol->is_external ||
        symbol->section_index != (int64_t)section_index) {
      continue;
    }
    if (strcmp(symbol->name, section->name) != 0) {
      continue;
    }
    if (symbol->aux_section_length != 0u) {
      return (size_t)symbol->aux_section_length;
    }
  }

  size = section->size_of_raw_data;
  if (section->virtual_size > size) {
    size = section->virtual_size;
  }
  if (section->kind != LINK_SECTION_KIND_BSS) {
    return size;
  }

  for (i = 0; i < object->symbol_count; i++) {
    const LinkSymbol *symbol = &object->symbols[i];
    size_t end = 0;

    if (symbol->is_auxiliary ||
        symbol->section_index != (int64_t)section_index) {
      continue;
    }
    saw_symbol = 1;
    end = (size_t)symbol->value + 1u;
    if (end > size) {
      size = end;
    }
  }

  if (size == 0u && saw_symbol) {
    size = 1u;
  }

  return size;
}

static int link_resolution_init_sections(LinkResolution *resolution) {
  static const LinkSectionKind kinds[LINKED_SECTION_COUNT] = {
      LINK_SECTION_KIND_TEXT, LINK_SECTION_KIND_RDATA, LINK_SECTION_KIND_DATA,
      LINK_SECTION_KIND_BSS,  LINK_SECTION_KIND_PDATA, LINK_SECTION_KIND_XDATA,
      LINK_SECTION_KIND_TLS};
  size_t i = 0;

  if (!resolution) {
    return 0;
  }

  for (i = 0; i < LINKED_SECTION_COUNT; i++) {
    resolution->sections[i].kind = kinds[i];
    resolution->sections[i].name = link_section_name_from_kind(kinds[i]);
    resolution->sections[i].alignment = 1u;
  }

  return 1;
}

static int link_resolution_load_objects(LinkResolution *resolution,
                                        const char **object_paths,
                                        size_t object_count,
                                        char **error_message_out) {
  size_t i = 0;

  if (!resolution || (!object_paths && object_count != 0u)) {
    return 0;
  }

  resolution->objects = calloc(object_count, sizeof(LinkedInputObject));
  if (!resolution->objects && object_count != 0u) {
    mettle_set_error(error_message_out,
                              "Out of memory while allocating input objects");
    return 0;
  }

  resolution->object_count = object_count;
  for (i = 0; i < object_count; i++) {
    LinkedInputObject *input = &resolution->objects[i];

    input->path = mettle_strdup(object_paths[i]);
    if (!input->path) {
      mettle_set_error(error_message_out,
                                "Out of memory while storing object path");
      return 0;
    }

    if (!link_object_read(object_paths[i], &input->object, error_message_out)) {
      return 0;
    }
  }

  return 1;
}

/* Section garbage collection: the runtime objects are compiled with
 * -ffunction-sections/-fdata-sections, so every function and global lives in
 * its own `.text$name`/`.data$name` section. Only those granular sections are
 * collectable; plain sections (all Mettle-emitted code among them) are roots.
 * Reachability follows relocations, binding locally when the referenced
 * symbol is defined in the same object and by name otherwise, with the same
 * runtime-default-loses precedence the real symbol resolution applies. */

typedef struct {
  const char *name;
  size_t object_index;
  size_t section_index;
  int is_runtime_default;
} GcDefinition;

typedef struct {
  size_t object_index;
  size_t section_index;
} GcWorkItem;

static int link_section_is_granular(const char *name) {
  if (!name) {
    return 0;
  }
  if (strchr(name, '$')) {
    return 1;
  }
  return name[0] == '.' && strchr(name + 1, '.') != NULL;
}

static int link_section_is_gc_eligible(const LinkSection *section) {
  if (!link_section_is_granular(section->name)) {
    return 0;
  }
  switch (section->kind) {
  case LINK_SECTION_KIND_TEXT:
  case LINK_SECTION_KIND_RDATA:
  case LINK_SECTION_KIND_DATA:
  case LINK_SECTION_KIND_BSS:
    return 1;
  default:
    return 0;
  }
}

static size_t gc_hash_name(const char *name) {
  size_t hash = 1469598103934665603u;
  while (*name) {
    hash ^= (unsigned char)*name++;
    hash *= 1099511628211u;
  }
  return hash;
}

static const GcDefinition *gc_find_definition(const GcDefinition *table,
                                              size_t bucket_count,
                                              const char *name) {
  size_t slot = gc_hash_name(name) & (bucket_count - 1u);
  while (table[slot].name) {
    if (strcmp(table[slot].name, name) == 0) {
      return &table[slot];
    }
    slot = (slot + 1u) & (bucket_count - 1u);
  }
  return NULL;
}

/* Mirrors link_resolution_record_global_symbol: a runtime default loses to a
 * program definition, and two same-precedence definitions are an error even
 * when both would be collected, so a duplicate never links silently. */
static int gc_insert_definition(GcDefinition *table, size_t bucket_count,
                                const GcDefinition *definition,
                                char **error_message_out) {
  size_t slot = gc_hash_name(definition->name) & (bucket_count - 1u);
  while (table[slot].name) {
    if (strcmp(table[slot].name, definition->name) == 0) {
      if (table[slot].is_runtime_default == definition->is_runtime_default) {
        mettle_set_error(error_message_out,
                         "Duplicate external symbol '%s' in object index %zu "
                         "and object index %zu",
                         definition->name, table[slot].object_index,
                         definition->object_index);
        return 0;
      }
      if (table[slot].is_runtime_default && !definition->is_runtime_default) {
        table[slot] = *definition;
      }
      return 1;
    }
    slot = (slot + 1u) & (bucket_count - 1u);
  }
  table[slot] = *definition;
  return 1;
}

static void gc_mark(unsigned char **live, GcWorkItem *worklist,
                    size_t *worklist_count, size_t object_index,
                    size_t section_index) {
  if (live[object_index][section_index]) {
    return;
  }
  live[object_index][section_index] = 1;
  worklist[*worklist_count].object_index = object_index;
  worklist[*worklist_count].section_index = section_index;
  (*worklist_count)++;
}

static int link_resolution_gc_sections(LinkResolution *resolution,
                                       const LinkResolutionOptions *options,
                                       char **error_message_out) {
  size_t object_index = 0;
  size_t total_sections = 0;
  size_t definition_count = 0;
  size_t bucket_count = 64;
  GcDefinition *table = NULL;
  unsigned char **live = NULL;
  GcWorkItem *worklist = NULL;
  size_t worklist_count = 0;
  size_t processed = 0;
  int ok = 0;

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    const LinkObject *object = resolution->objects[object_index].object;
    if (!object) {
      continue;
    }
    total_sections += object->section_count;
    definition_count += object->symbol_count;
  }
  if (total_sections == 0u) {
    return 1;
  }

  while (bucket_count < definition_count * 2u) {
    bucket_count *= 2u;
  }

  table = calloc(bucket_count, sizeof(GcDefinition));
  live = calloc(resolution->object_count, sizeof(unsigned char *));
  worklist = malloc(total_sections * sizeof(GcWorkItem));
  if (!table || !live || !worklist) {
    mettle_set_error(error_message_out,
                     "Out of memory during section garbage collection");
    goto cleanup;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    const LinkObject *object = input->object;
    size_t i = 0;

    if (!object) {
      continue;
    }
    live[object_index] = calloc(object->section_count ? object->section_count : 1u,
                                sizeof(unsigned char));
    input->symbol_gc_referenced =
        calloc(object->symbol_count ? object->symbol_count : 1u,
               sizeof(unsigned char));
    if (!live[object_index] || !input->symbol_gc_referenced) {
      mettle_set_error(error_message_out,
                       "Out of memory during section garbage collection");
      goto cleanup;
    }

    for (i = 0; i < object->symbol_count; i++) {
      const LinkSymbol *symbol = &object->symbols[i];
      GcDefinition definition = {0};

      if (symbol->is_auxiliary || !symbol->name ||
          !symbol->is_external || symbol->section_index < 0 ||
          (size_t)symbol->section_index >= object->section_count) {
        continue;
      }
      definition.name = symbol->name;
      definition.object_index = object_index;
      definition.section_index = (size_t)symbol->section_index;
      definition.is_runtime_default = input->is_runtime_default;
      if (!gc_insert_definition(table, bucket_count, &definition,
                                error_message_out)) {
        goto cleanup;
      }
    }
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    const LinkObject *object = resolution->objects[object_index].object;
    size_t i = 0;

    if (!object) {
      continue;
    }
    for (i = 0; i < object->section_count; i++) {
      const LinkSection *section = &object->sections[i];
      if (section->kind == LINK_SECTION_KIND_UNKNOWN) {
        continue;
      }
      if (!link_section_is_gc_eligible(section)) {
        gc_mark(live, worklist, &worklist_count, object_index, i);
      }
    }
  }

  if (options && options->entry_symbol_name && options->entry_symbol_name[0]) {
    const GcDefinition *entry =
        gc_find_definition(table, bucket_count, options->entry_symbol_name);
    if (entry) {
      gc_mark(live, worklist, &worklist_count, entry->object_index,
              entry->section_index);
    }
  }

  while (processed < worklist_count) {
    GcWorkItem item = worklist[processed++];
    LinkedInputObject *input = &resolution->objects[item.object_index];
    const LinkObject *object = input->object;
    const LinkSection *section = &object->sections[item.section_index];
    size_t r = 0;

    for (r = 0; r < section->relocation_count; r++) {
      uint32_t symbol_index = section->relocations[r].symbol_index;
      const LinkSymbol *symbol = NULL;

      if (symbol_index >= object->symbol_count) {
        continue;
      }
      input->symbol_gc_referenced[symbol_index] = 1;
      symbol = &object->symbols[symbol_index];
      if (symbol->is_auxiliary) {
        continue;
      }
      if (symbol->section_index >= 0 &&
          (size_t)symbol->section_index < object->section_count) {
        gc_mark(live, worklist, &worklist_count, item.object_index,
                (size_t)symbol->section_index);
      } else if (symbol->section_index == LINK_SECTION_INDEX_UNDEFINED &&
                 symbol->name) {
        const GcDefinition *definition =
            gc_find_definition(table, bucket_count, symbol->name);
        if (definition) {
          gc_mark(live, worklist, &worklist_count, definition->object_index,
                  definition->section_index);
        }
      }
    }
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    const LinkObject *object = input->object;
    size_t i = 0;

    if (!object) {
      continue;
    }
    input->section_gc_dead =
        calloc(object->section_count ? object->section_count : 1u,
               sizeof(unsigned char));
    if (!input->section_gc_dead) {
      mettle_set_error(error_message_out,
                       "Out of memory during section garbage collection");
      goto cleanup;
    }
    for (i = 0; i < object->section_count; i++) {
      if (!live[object_index][i] &&
          link_section_is_gc_eligible(&object->sections[i])) {
        input->section_gc_dead[i] = 1;
      }
    }
  }

  if (getenv("METTLE_LINK_GC_REPORT")) {
    for (object_index = 0; object_index < resolution->object_count;
         object_index++) {
      const LinkedInputObject *input = &resolution->objects[object_index];
      const LinkObject *object = input->object;
      size_t i = 0;

      if (!object) {
        continue;
      }
      for (i = 0; i < object->section_count; i++) {
        const LinkSection *section = &object->sections[i];
        if (section->kind == LINK_SECTION_KIND_UNKNOWN ||
            section->size_of_raw_data == 0u) {
          continue;
        }
        fprintf(stderr, "gc %s %s %s %u\n",
                input->section_gc_dead[i] ? "dead" : "live",
                input->path ? input->path : "<unknown>", section->name,
                section->size_of_raw_data);
      }
    }
  }

  ok = 1;

cleanup:
  if (live) {
    for (object_index = 0; object_index < resolution->object_count;
         object_index++) {
      free(live[object_index]);
    }
  }
  free(live);
  free(table);
  free(worklist);
  return ok;
}

static int link_resolution_merge_sections(LinkResolution *resolution,
                                          size_t fallback_alignment,
                                          char **error_message_out) {
  size_t object_index = 0;

  if (!resolution) {
    return 0;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    size_t section_count = input->object ? input->object->section_count : 0u;
    size_t section_index = 0;

    input->section_merged_indices = malloc(section_count * sizeof(size_t));
    input->section_merged_offsets = calloc(section_count, sizeof(size_t));
    input->section_merged_sizes = calloc(section_count, sizeof(size_t));
    input->section_alignments = calloc(section_count, sizeof(size_t));
    if ((!input->section_merged_indices && section_count != 0u) ||
        (!input->section_merged_offsets && section_count != 0u) ||
        (!input->section_merged_sizes && section_count != 0u) ||
        (!input->section_alignments && section_count != 0u)) {
      mettle_set_error(error_message_out,
                                "Out of memory while mapping object sections");
      return 0;
    }

    for (section_index = 0; section_index < section_count; section_index++) {
      const LinkSection *section = &input->object->sections[section_index];
      size_t merged_index = link_section_index_from_kind(section->kind);
      size_t alignment = link_section_alignment(section, fallback_alignment);
      size_t contribution_size = 0;
      LinkedSection *merged = NULL;
      size_t start = 0;

      input->section_merged_indices[section_index] = LINKED_SECTION_INDEX_NONE;
      if (merged_index == LINKED_SECTION_INDEX_NONE) {
        continue;
      }
      if (input->section_gc_dead && input->section_gc_dead[section_index]) {
        continue;
      }

      merged = &resolution->sections[merged_index];
      if (alignment > merged->alignment) {
        merged->alignment = alignment;
      }

      contribution_size = link_estimate_section_size(input->object, section_index);
      start = linker_align_up(merged->virtual_size, alignment);

      if (!link_section_reserve_contributions(merged, merged->contribution_count + 1u,
                                              error_message_out)) {
        return 0;
      }

      if (section->kind != LINK_SECTION_KIND_BSS &&
          section->size_of_raw_data != 0u) {
        if (!link_section_reserve_data(merged, start + section->size_of_raw_data,
                                       error_message_out)) {
          return 0;
        }
        if (start > merged->size) {
          memset(merged->data + merged->size, 0, start - merged->size);
        }
        if (section->size_of_raw_data > 0u) {
          memcpy(merged->data + start, section->raw_data, section->size_of_raw_data);
        }
        merged->size = start + section->size_of_raw_data;
        if (merged->virtual_size < merged->size) {
          merged->virtual_size = merged->size;
        }
      } else {
        if (merged->virtual_size < start + contribution_size) {
          merged->virtual_size = start + contribution_size;
        }
      }

      merged->contributions[merged->contribution_count].object_index = object_index;
      merged->contributions[merged->contribution_count].section_index = section_index;
      merged->contributions[merged->contribution_count].merged_offset = start;
      merged->contributions[merged->contribution_count].size = contribution_size;
      merged->contributions[merged->contribution_count].alignment = alignment;
      merged->contribution_count++;

      input->section_merged_indices[section_index] = merged_index;
      input->section_merged_offsets[section_index] = start;
      input->section_merged_sizes[section_index] = contribution_size;
      input->section_alignments[section_index] = alignment;
    }
  }

  return 1;
}

static int link_resolution_reindex_symbols(LinkResolution *resolution,
                                           size_t min_buckets) {
  size_t nb = 64;
  size_t *fresh = NULL;
  size_t i = 0;

  while (nb < min_buckets) {
    nb *= 2u;
  }
  fresh = calloc(nb, sizeof(size_t));
  if (!fresh) {
    return 0;
  }
  for (i = 0; i < resolution->symbol_count; i++) {
    if (resolution->symbols[i].name) {
      size_t b = mettle_fnv1a_hash(resolution->symbols[i].name) & (nb - 1);
      while (fresh[b]) {
        b = (b + 1) & (nb - 1);
      }
      fresh[b] = i + 1;
    }
  }
  free(resolution->symbol_buckets);
  resolution->symbol_buckets = fresh;
  resolution->symbol_bucket_count = nb;
  return 1;
}

static int link_resolution_index_insert(LinkResolution *resolution,
                                        size_t symbol_index) {
  size_t b = 0;

  if ((resolution->symbol_count + 1u) * 4u >=
      resolution->symbol_bucket_count * 3u) {
    if (!link_resolution_reindex_symbols(resolution,
                                         (resolution->symbol_count + 1u) * 2u)) {
      return 0;
    }
    return 1; /* reindex covered the new entry */
  }
  b = mettle_fnv1a_hash(resolution->symbols[symbol_index].name) &
      (resolution->symbol_bucket_count - 1);
  while (resolution->symbol_buckets[b]) {
    b = (b + 1) & (resolution->symbol_bucket_count - 1);
  }
  resolution->symbol_buckets[b] = symbol_index + 1u;
  return 1;
}

LinkedSymbol *link_resolution_find_symbol_mutable(LinkResolution *resolution,
                                                  const char *name) {
  size_t i = 0;

  if (!resolution || !name) {
    return NULL;
  }

  if (resolution->symbol_bucket_count) {
    size_t b = mettle_fnv1a_hash(name) & (resolution->symbol_bucket_count - 1);
    while (resolution->symbol_buckets[b]) {
      LinkedSymbol *s = &resolution->symbols[resolution->symbol_buckets[b] - 1];
      if (s->name && strcmp(s->name, name) == 0) {
        return s;
      }
      b = (b + 1) & (resolution->symbol_bucket_count - 1);
    }
    return NULL;
  }

  for (i = 0; i < resolution->symbol_count; i++) {
    if (resolution->symbols[i].name &&
        strcmp(resolution->symbols[i].name, name) == 0) {
      return &resolution->symbols[i];
    }
  }

  return NULL;
}

static int link_resolution_reserve_symbols(LinkResolution *resolution,
                                           size_t minimum_count,
                                           char **error_message_out) {
  LinkedSymbol *grown = NULL;
  size_t new_capacity = 0;

  if (!resolution) {
    return 0;
  }
  if (resolution->symbol_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = resolution->symbol_capacity ? resolution->symbol_capacity : 8u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(resolution->symbols, new_capacity * sizeof(LinkedSymbol));
  if (!grown) {
    mettle_set_error(error_message_out,
                              "Out of memory while growing global symbol table");
    return 0;
  }

  memset(grown + resolution->symbol_capacity, 0,
         (new_capacity - resolution->symbol_capacity) * sizeof(LinkedSymbol));
  resolution->symbols = grown;
  resolution->symbol_capacity = new_capacity;
  return 1;
}

static int link_resolution_record_global_symbol(
    LinkResolution *resolution, const LinkedInputObject *input,
    const LinkedObjectSymbol *object_symbol, char **error_message_out) {
  LinkedSymbol *global_symbol = NULL;

  if (!resolution || !input || !object_symbol || !object_symbol->name) {
    return 0;
  }

  global_symbol =
      link_resolution_find_symbol_mutable(resolution, object_symbol->name);
  if (!global_symbol) {
    if (!link_resolution_reserve_symbols(resolution, resolution->symbol_count + 1u,
                                         error_message_out)) {
      return 0;
    }

    global_symbol = &resolution->symbols[resolution->symbol_count++];
    memset(global_symbol, 0, sizeof(*global_symbol));
    global_symbol->name = mettle_strdup(object_symbol->name);
    if (!global_symbol->name) {
      mettle_set_error(error_message_out,
                                "Out of memory while storing symbol '%s'",
                                object_symbol->name);
      return 0;
    }
    global_symbol->defining_object_index = LINKED_SECTION_INDEX_NONE;
    global_symbol->defining_symbol_index = UINT32_MAX;
    if (!link_resolution_index_insert(resolution,
                                      resolution->symbol_count - 1u)) {
      mettle_set_error(error_message_out,
                                "Out of memory while indexing symbol '%s'",
                                object_symbol->name);
      return 0;
    }
  }

  global_symbol->is_external = 1;
  if (!object_symbol->is_defined) {
    global_symbol->is_weak = object_symbol->is_weak;
    return 1;
  }

  if (global_symbol->is_defined) {
    const LinkedInputObject *holder =
        &resolution->objects[global_symbol->defining_object_index];
    /* A runtime default loses to a real definition, whichever order they
     * arrive in. Relocations inside an object that defines the symbol itself
     * are bound locally (see relocation.c), so replacing the global definition
     * redirects other objects without rerouting the runtime's own calls. */
    if (holder->is_runtime_default && !input->is_runtime_default) {
      /* fall through and let the program definition take over */
    } else if (!holder->is_runtime_default && input->is_runtime_default) {
      return 1;
    } else {
      mettle_set_error(
          error_message_out,
          "Duplicate external symbol '%s' in '%s' and object index %zu",
          object_symbol->name, input->path ? input->path : "<unknown>",
          global_symbol->defining_object_index);
      return 0;
    }
  }

  global_symbol->is_defined = 1;
  global_symbol->defining_object_index = object_symbol->object_index;
  global_symbol->defining_symbol_index = object_symbol->symbol_index;
  global_symbol->merged_section_index = object_symbol->merged_section_index;
  global_symbol->merged_offset = object_symbol->merged_offset;
  global_symbol->size = object_symbol->size;
  global_symbol->elf_type = object_symbol->elf_type;
  return 1;
}

static int link_resolution_build_symbols(LinkResolution *resolution,
                                         char **error_message_out) {
  size_t object_index = 0;

  if (!resolution) {
    return 0;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    size_t symbol_index = 0;

    input->symbol_count = input->object ? input->object->symbol_count : 0u;
    input->symbols = calloc(input->symbol_count, sizeof(LinkedObjectSymbol));
    if (!input->symbols && input->symbol_count != 0u) {
      mettle_set_error(error_message_out,
                                "Out of memory while mapping object symbols");
      return 0;
    }

    for (symbol_index = 0; symbol_index < input->symbol_count; symbol_index++) {
      const LinkSymbol *symbol = &input->object->symbols[symbol_index];
      LinkedObjectSymbol *resolved = &input->symbols[symbol_index];
      size_t section_index = 0;
      size_t merged_index = LINKED_SECTION_INDEX_NONE;

      resolved->object_index = object_index;
      resolved->symbol_index = (uint32_t)symbol_index;
      resolved->section_index = symbol->section_index;
      resolved->merged_section_index = LINKED_SECTION_INDEX_NONE;
      resolved->is_auxiliary = symbol->is_auxiliary;
      resolved->size = symbol->size;
      resolved->elf_type = symbol->elf_type;
      resolved->is_weak = symbol->is_weak;
      resolved->name = mettle_strdup(symbol->name);
      if (symbol->name && !resolved->name) {
        mettle_set_error(error_message_out,
                                  "Out of memory while storing object symbol");
        return 0;
      }

      if (symbol->is_auxiliary) {
        resolved->is_local = 1;
        continue;
      }

      resolved->is_external = symbol->is_external;
      resolved->is_local = !resolved->is_external;

      if (symbol->section_index >= 0) {
        section_index = (size_t)symbol->section_index;
        if (section_index >= input->object->section_count) {
          mettle_set_error(error_message_out,
                                    "Symbol '%s' in '%s' refers to section %lld "
                                    "outside the section table",
                                    symbol->name ? symbol->name : "<unnamed>",
                                    input->path ? input->path : "<unknown>",
                                    (long long)symbol->section_index);
          return 0;
        }

        merged_index = input->section_merged_indices[section_index];
        if (merged_index != LINKED_SECTION_INDEX_NONE) {
          resolved->is_defined = 1;
          resolved->merged_section_index = merged_index;
          resolved->merged_offset =
              input->section_merged_offsets[section_index] + (size_t)symbol->value;
        }
      }

      if (symbol->section_index >= 0 && input->section_gc_dead &&
          input->section_gc_dead[(size_t)symbol->section_index]) {
        continue;
      }
      /* An undefined external nothing retained relocates against is dropped
       * rather than recorded, so it neither fails resolution nor becomes a
       * DLL import. Mettle objects declare every extern a module names, used
       * or not, and the runtime's dead code names libc symbols. */
      if (symbol->section_index == LINK_SECTION_INDEX_UNDEFINED &&
          input->symbol_gc_referenced &&
          !input->symbol_gc_referenced[symbol_index]) {
        continue;
      }

      if (resolved->is_external && resolved->name) {
        if (!link_resolution_record_global_symbol(resolution, input, resolved,
                                                  error_message_out)) {
          return 0;
        }
      }
    }
  }

  return 1;
}

static int link_resolution_assign_virtual_addresses(
    LinkResolution *resolution, size_t section_alignment) {
  size_t section_index = 0;
  uint64_t current_address = 0;
  size_t object_index = 0;
  size_t symbol_index = 0;

  if (!resolution) {
    return 0;
  }

  for (section_index = 0; section_index < LINKED_SECTION_COUNT; section_index++) {
    LinkedSection *section = &resolution->sections[section_index];

    if (section->virtual_size == 0u) {
      continue;
    }

    current_address = (uint64_t)linker_align_up((size_t)current_address,
                                              section_alignment);
    section->virtual_address = current_address;
    current_address += (uint64_t)section->virtual_size;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];
    for (symbol_index = 0; symbol_index < input->symbol_count; symbol_index++) {
      LinkedObjectSymbol *symbol = &input->symbols[symbol_index];

      if (!symbol->is_defined ||
          symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
        continue;
      }
      symbol->virtual_address =
          resolution->sections[symbol->merged_section_index].virtual_address +
          (uint64_t)symbol->merged_offset;
    }
  }

  for (symbol_index = 0; symbol_index < resolution->symbol_count; symbol_index++) {
    LinkedSymbol *symbol = &resolution->symbols[symbol_index];

    if (!symbol->is_defined ||
        symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    symbol->virtual_address =
        resolution->sections[symbol->merged_section_index].virtual_address +
        (uint64_t)symbol->merged_offset;
  }

  return 1;
}

static int link_resolution_reserve_shared_imports(LinkResolution *resolution,
                                                  size_t minimum_count,
                                                  char **error_message_out) {
  LinkedSharedImport *grown = NULL;
  size_t capacity = resolution->shared_import_capacity;

  if (capacity >= minimum_count) {
    return 1;
  }
  capacity = capacity ? capacity : 16u;
  while (capacity < minimum_count) {
    capacity *= 2u;
  }
  grown = realloc(resolution->shared_imports,
                  capacity * sizeof(LinkedSharedImport));
  if (!grown) {
    mettle_set_error(error_message_out,
                     "Out of memory while growing the shared import table");
    return 0;
  }
  memset(grown + resolution->shared_import_capacity, 0,
         (capacity - resolution->shared_import_capacity) *
             sizeof(LinkedSharedImport));
  resolution->shared_imports = grown;
  resolution->shared_import_capacity = capacity;
  return 1;
}

static int link_resolution_add_shared_import(LinkResolution *resolution,
                                             size_t symbol_index,
                                             size_t library_index,
                                             const ElfSharedSymbol *definition,
                                             char **error_message_out) {
  LinkedSharedImport *import = NULL;
  LinkedSymbol *symbol = &resolution->symbols[symbol_index];

  if (!link_resolution_reserve_shared_imports(
          resolution, resolution->shared_import_count + 1u,
          error_message_out)) {
    return 0;
  }
  import = &resolution->shared_imports[resolution->shared_import_count];
  memset(import, 0, sizeof(*import));
  import->library_index = library_index;
  import->symbol_index = symbol_index;
  import->type = definition ? definition->type : ELF_SHARED_TYPE_NOTYPE;
  import->size = definition ? definition->size : 0u;
  import->alignment = definition ? definition->alignment : 0u;
  import->is_weak = definition ? definition->is_weak : symbol->is_weak;
  if (definition && definition->version) {
    import->version = mettle_strdup(definition->version);
    if (!import->version) {
      mettle_set_error(error_message_out,
                       "Out of memory while recording the version of '%s'",
                       symbol->name ? symbol->name : "<unnamed>");
      return 0;
    }
  }
  symbol->is_shared_import = 1;
  symbol->shared_import_index = resolution->shared_import_count;
  resolution->shared_import_count++;
  if (library_index != LINKED_LIBRARY_INDEX_NONE) {
    resolution->shared_library_used[library_index] = 1u;
  }
  return 1;
}

static int link_resolution_bind_shared_libraries(
    LinkResolution *resolution, const LinkResolutionOptions *options,
    char **error_message_out) {
  size_t path_count = options ? options->shared_library_path_count : 0u;
  size_t i = 0u;

  if (path_count == 0u && !(options && options->produce_shared_library)) {
    return 1;
  }

  if (path_count != 0u) {
    resolution->shared_libraries =
        calloc(path_count, sizeof(*resolution->shared_libraries));
    resolution->shared_library_used = calloc(path_count, 1u);
    if (!resolution->shared_libraries || !resolution->shared_library_used) {
      mettle_set_error(error_message_out,
                       "Out of memory while loading shared libraries");
      return 0;
    }
    for (i = 0u; i < path_count; i++) {
      if (!elf_shared_library_read(options->shared_library_paths[i],
                                   &resolution->shared_libraries[i],
                                   error_message_out)) {
        return 0;
      }
      resolution->shared_library_count++;
    }
  }

  for (i = 0u; i < resolution->symbol_count; i++) {
    LinkedSymbol *symbol = &resolution->symbols[i];
    const ElfSharedSymbol *definition = NULL;
    size_t library_index = LINKED_LIBRARY_INDEX_NONE;
    size_t library = 0u;

    if (symbol->is_defined || !symbol->is_external || !symbol->name) {
      continue;
    }
    for (library = 0u; library < resolution->shared_library_count; library++) {
      definition = elf_shared_library_find(resolution->shared_libraries[library],
                                           symbol->name);
      if (definition) {
        library_index = library;
        break;
      }
    }
    if (!definition && !(options && options->produce_shared_library)) {
      continue;
    }
    if (!link_resolution_add_shared_import(resolution, i, library_index,
                                           definition, error_message_out)) {
      return 0;
    }
  }
  return 1;
}

static int link_resolution_validate_externals(
    LinkResolution *resolution, const LinkResolutionOptions *options,
    char **error_message_out) {
  size_t symbol_index = 0;
  const char *entry_name = NULL;

  if (!resolution) {
    return 0;
  }

  if (!options || !options->allow_unresolved_externals) {
    for (symbol_index = 0; symbol_index < resolution->symbol_count;
         symbol_index++) {
      const LinkedSymbol *symbol = &resolution->symbols[symbol_index];
      if (symbol->is_external && !symbol->is_defined &&
          !symbol->is_shared_import) {
        link_unresolved_format(resolution,
                               symbol->name ? symbol->name : "<unnamed>",
                               error_message_out);
        return 0;
      }
    }
  }

  entry_name = (options && options->entry_symbol_name)
                   ? options->entry_symbol_name
                   : NULL;
  if (entry_name && entry_name[0] != '\0') {
    resolution->entry_symbol = link_resolution_find_symbol(resolution, entry_name);
    if (!resolution->entry_symbol || !resolution->entry_symbol->is_defined) {
      mettle_set_error(error_message_out,
                                "Entry point symbol '%s' was not resolved",
                                entry_name);
      return 0;
    }
  }

  return 1;
}

int link_resolution_build(const char **object_paths, size_t object_count,
                          const LinkResolutionOptions *options,
                          LinkResolution **resolution_out,
                          char **error_message_out) {
  LinkResolution *resolution = NULL;
  size_t section_alignment = 16u;
  int ok = 0;

  if (resolution_out) {
    *resolution_out = NULL;
  }
  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }

  if (!object_paths || object_count == 0u || !resolution_out) {
    mettle_set_error(error_message_out,
                              "At least one object file is required");
    return 0;
  }

  resolution = calloc(1, sizeof(LinkResolution));
  if (!resolution) {
    mettle_set_error(error_message_out,
                              "Out of memory while creating link resolution");
    return 0;
  }

  link_resolution_init_sections(resolution);
  if (options && options->section_alignment > 1u) {
    section_alignment = options->section_alignment;
  }

  if (!link_resolution_load_objects(resolution, object_paths, object_count,
                                    error_message_out)) {
    goto cleanup;
  }
  if (options && options->object_is_runtime_default) {
    size_t i = 0;
    for (i = 0; i < resolution->object_count; i++) {
      resolution->objects[i].is_runtime_default =
          options->object_is_runtime_default[i] ? 1 : 0;
    }
  }

  if (!link_resolution_gc_sections(resolution, options, error_message_out) ||
      !link_resolution_merge_sections(resolution, section_alignment,
                                      error_message_out) ||
      !link_resolution_build_symbols(resolution, error_message_out) ||
      !link_resolution_bind_shared_libraries(resolution, options,
                                             error_message_out) ||
      !link_resolution_assign_virtual_addresses(resolution, section_alignment) ||
      !link_resolution_validate_externals(resolution, options,
                                          error_message_out)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (!ok) {
    link_resolution_destroy(resolution);
    return 0;
  }

  *resolution_out = resolution;
  return 1;
}

void link_resolution_destroy(LinkResolution *resolution) {
  size_t object_index = 0;
  size_t section_index = 0;
  size_t symbol_index = 0;

  if (!resolution) {
    return;
  }

  for (object_index = 0; object_index < resolution->object_count;
       object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];

    if (input->symbols) {
      for (symbol_index = 0; symbol_index < input->symbol_count; symbol_index++) {
        free(input->symbols[symbol_index].name);
      }
    }
    free(input->path);
    free(input->section_merged_indices);
    free(input->section_merged_offsets);
    free(input->section_merged_sizes);
    free(input->section_alignments);
    free(input->symbols);
    free(input->section_gc_dead);
    free(input->symbol_gc_referenced);
    link_object_destroy(input->object);
  }

  for (section_index = 0; section_index < LINKED_SECTION_COUNT; section_index++) {
    free(resolution->sections[section_index].data);
    free(resolution->sections[section_index].contributions);
  }

  for (symbol_index = 0; symbol_index < resolution->symbol_count;
       symbol_index++) {
    free(resolution->symbols[symbol_index].name);
  }

  for (symbol_index = 0; symbol_index < resolution->shared_library_count;
       symbol_index++) {
    elf_shared_library_destroy(resolution->shared_libraries[symbol_index]);
  }
  for (symbol_index = 0; symbol_index < resolution->shared_import_count;
       symbol_index++) {
    free(resolution->shared_imports[symbol_index].version);
  }

  free(resolution->shared_libraries);
  free(resolution->shared_library_used);
  free(resolution->shared_imports);
  free(resolution->objects);
  free(resolution->symbols);
  free(resolution->symbol_buckets);
  free(resolution);
}

const LinkedSection *link_resolution_find_section(
    const LinkResolution *resolution, LinkSectionKind kind) {
  size_t section_index = link_section_index_from_kind(kind);

  if (!resolution || section_index == LINKED_SECTION_INDEX_NONE) {
    return NULL;
  }

  return &resolution->sections[section_index];
}

const LinkedSymbol *link_resolution_find_symbol(const LinkResolution *resolution,
                                                const char *name) {
  size_t symbol_index = 0;

  if (!resolution || !name) {
    return NULL;
  }

  if (resolution->symbol_bucket_count) {
    size_t b = mettle_fnv1a_hash(name) & (resolution->symbol_bucket_count - 1);
    while (resolution->symbol_buckets[b]) {
      const LinkedSymbol *s =
          &resolution->symbols[resolution->symbol_buckets[b] - 1];
      if (s->name && strcmp(s->name, name) == 0) {
        return s;
      }
      b = (b + 1) & (resolution->symbol_bucket_count - 1);
    }
    return NULL;
  }

  for (symbol_index = 0; symbol_index < resolution->symbol_count;
       symbol_index++) {
    if (resolution->symbols[symbol_index].name &&
        strcmp(resolution->symbols[symbol_index].name, name) == 0) {
      return &resolution->symbols[symbol_index];
    }
  }

  return NULL;
}
