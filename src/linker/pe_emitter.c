#include "linker/pe_emitter.h"
#include "linker/linker_common.h"
#include "../common.h"

#include "linker/coff_reader.h"
#include "linker/import_lib.h"
#include "linker/relocation.h"
#include "runtime/verify_owned.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef IMAGE_FILE_RELOCS_STRIPPED
#define IMAGE_FILE_RELOCS_STRIPPED 0x0001u
#endif
#ifndef IMAGE_FILE_EXECUTABLE_IMAGE
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002u
#endif
#ifndef IMAGE_FILE_LINE_NUMS_STRIPPED
#define IMAGE_FILE_LINE_NUMS_STRIPPED 0x0004u
#endif
#ifndef IMAGE_FILE_LOCAL_SYMS_STRIPPED
#define IMAGE_FILE_LOCAL_SYMS_STRIPPED 0x0008u
#endif
#ifndef IMAGE_FILE_LARGE_ADDRESS_AWARE
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020u
#endif
#ifndef IMAGE_FILE_DLL
#define IMAGE_FILE_DLL 0x2000u
#endif

#ifndef IMAGE_SUBSYSTEM_WINDOWS_CUI
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3u
#endif

#ifndef IMAGE_SCN_CNT_CODE
#define IMAGE_SCN_CNT_CODE 0x00000020u
#endif
#ifndef IMAGE_SCN_CNT_INITIALIZED_DATA
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040u
#endif
#ifndef IMAGE_SCN_CNT_UNINITIALIZED_DATA
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#endif
#ifndef IMAGE_SCN_MEM_EXECUTE
#define IMAGE_SCN_MEM_EXECUTE 0x20000000u
#endif
#ifndef IMAGE_SCN_MEM_READ
#define IMAGE_SCN_MEM_READ 0x40000000u
#endif
#ifndef IMAGE_SCN_MEM_WRITE
#define IMAGE_SCN_MEM_WRITE 0x80000000u
#endif

#define PE_SIGNATURE 0x00004550u
#define PE_OPTIONAL_HEADER64_MAGIC 0x020Bu
#define PE_DOS_HEADER_SIZE 64u
#define PE_DOS_STUB_MESSAGE_SIZE 64u
#define PE_DOS_STUB_SIZE (PE_DOS_HEADER_SIZE + PE_DOS_STUB_MESSAGE_SIZE)
#define PE_FILE_HEADER_SIZE 20u
#define PE_SECTION_HEADER_SIZE 40u
#define PE_DATA_DIRECTORY_COUNT 16u
#define PE_OPTIONAL_HEADER64_SIZE 240u

#define PE_DIRECTORY_EXPORT 0u
#define PE_DIRECTORY_EXCEPTION 3u
#define PE_DIRECTORY_IMPORT 1u
#define PE_DIRECTORY_IAT 12u

#define PE_EXPORT_DIRECTORY_SIZE 40u

#define PE_IMPORT_DESCRIPTOR_SIZE 20u
#define PE_IMPORT_THUNK_SIZE 8u
#define PE_IMPORT_JUMP_STUB_SIZE 6u

#define PE_SECTION_INDEX_TEXT 0u
#define PE_SECTION_INDEX_RDATA 1u
#define PE_SECTION_INDEX_DATA 2u
#define PE_SECTION_INDEX_BSS 3u
#define PE_SECTION_INDEX_PDATA 4u
#define PE_SECTION_INDEX_XDATA 5u

#define PE_IMP_PREFIX "__imp_"
#define PE_IMP_PREFIX_LEN 6u

typedef struct {
  size_t merged_section_index;
  const char *name;
  const LinkedSection *source;
  uint32_t characteristics;
  uint32_t virtual_address;
  uint32_t virtual_size;
  uint32_t raw_offset;
  uint32_t raw_size;
} PeSectionLayout;

typedef struct {
  char *dll_name;
  size_t symbol_count;
  size_t descriptor_offset;
  size_t ilt_offset;
  size_t name_offset;
  size_t iat_offset;
} PeImportDll;

typedef struct {
  char *canonical_name;
  char *dll_name;
  size_t dll_index;
  size_t dll_slot_index;
  uint16_t ordinal_or_hint;
  uint16_t import_type;
  uint16_t name_type;
  int is_ordinal;
  int needs_thunk;
  int needs_iat_symbol;
  size_t thunk_offset;
  size_t hint_name_offset;
  size_t iat_offset;
} PeImportSymbol;

typedef struct {
  PeImportDll *dlls;
  size_t dll_count;
  size_t dll_capacity;
  PeImportSymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  size_t descriptor_table_offset;
  uint32_t import_directory_rva;
  uint32_t import_directory_size;
  uint32_t iat_rva;
  uint32_t iat_size;
  int has_imports;
} PeImportPlan;

typedef struct {
  const LinkedSymbol **exports;
  size_t *name_offsets;
  size_t count;
  size_t capacity;
  char *dll_name;
  size_t directory_offset;
  size_t function_table_offset;
  size_t name_table_offset;
  size_t ordinal_table_offset;
  size_t dll_name_offset;
  uint32_t export_directory_rva;
  uint32_t export_directory_size;
  int has_exports;
} PeExportPlan;

static const ImportLibrarySymbol *
pe_find_import_symbol(ImportLibrary **libraries, size_t library_count,
                      const char *canonical_name);
static int pe_is_imp_symbol_name(const char *symbol_name);

static int pe_text_ends_with_ignore_case(const char *text, const char *suffix) {
  size_t text_length = 0u;
  size_t suffix_length = 0u;
  size_t i = 0u;

  if (!text || !suffix) {
    return 0;
  }

  text_length = strlen(text);
  suffix_length = strlen(suffix);
  if (suffix_length > text_length) {
    return 0;
  }

  for (i = 0u; i < suffix_length; i++) {
    unsigned char text_char =
        (unsigned char)text[text_length - suffix_length + i];
    unsigned char suffix_char = (unsigned char)suffix[i];
    if (tolower(text_char) != tolower(suffix_char)) {
      return 0;
    }
  }

  return 1;
}

static void pe_write_u16_to_memory(unsigned char *bytes, uint16_t value) {
  bytes[0] = (unsigned char)(value & 0xFFu);
  bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static int pe_write_u16(FILE *file, uint16_t value) {
  unsigned char bytes[2];

  pe_write_u16_to_memory(bytes, value);
  return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
}

static int pe_write_u32(FILE *file, uint32_t value) {
  unsigned char bytes[4];

  linker_write_u32(bytes, value);
  return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
}

static int pe_write_u64(FILE *file, uint64_t value) {
  unsigned char bytes[8];

  linker_write_u64(bytes, value);
  return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
}

static int pe_write_zeros(FILE *file, size_t count) {
  static const unsigned char zeros[64] = {0};

  while (count > 0u) {
    size_t chunk = count < sizeof(zeros) ? count : sizeof(zeros);
    if (fwrite(zeros, 1u, chunk, file) != chunk) {
      return 0;
    }
    count -= chunk;
  }

  return 1;
}

static int pe_write_section_name(FILE *file, const char *name) {
  unsigned char bytes[8] = {0};
  size_t length = 0u;

  if (!name) {
    return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
  }

  length = strlen(name);
  if (length > sizeof(bytes)) {
    length = sizeof(bytes);
  }

  memcpy(bytes, name, length);
  return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
}

static uint32_t pe_characteristics_for_kind(LinkSectionKind kind) {
  switch (kind) {
  case LINK_SECTION_KIND_TEXT:
    return IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
  case LINK_SECTION_KIND_RDATA:
    return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  case LINK_SECTION_KIND_DATA:
    return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
           IMAGE_SCN_MEM_WRITE;
  case LINK_SECTION_KIND_BSS:
    return IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ |
           IMAGE_SCN_MEM_WRITE;
  case LINK_SECTION_KIND_PDATA:
  case LINK_SECTION_KIND_XDATA:
    return IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  case LINK_SECTION_KIND_UNKNOWN:
  default:
    return IMAGE_SCN_MEM_READ;
  }
}

static LinkedSymbol *pe_find_symbol_mutable(LinkResolution *resolution,
                                            const char *name) {
  size_t i = 0u;

  if (!resolution || !name) {
    return NULL;
  }

  for (i = 0u; i < resolution->symbol_count; i++) {
    if (resolution->symbols[i].name &&
        strcmp(resolution->symbols[i].name, name) == 0) {
      return &resolution->symbols[i];
    }
  }

  return NULL;
}

static int pe_section_reserve(LinkedSection *section, size_t minimum_size,
                              char **error_message_out) {
  unsigned char *grown = NULL;
  size_t new_capacity = 0u;

  if (!section) {
    return 0;
  }
  if (section->data_capacity >= minimum_size) {
    return 1;
  }

  new_capacity = section->data_capacity ? section->data_capacity : 64u;
  while (new_capacity < minimum_size) {
    new_capacity *= 2u;
  }

  grown = realloc(section->data, new_capacity);
  if (!grown) {
    mettle_set_error(error_message_out,
                 "Out of memory while growing synthetic section '%s'",
                 section->name ? section->name : "<unknown>");
    return 0;
  }

  section->data = grown;
  section->data_capacity = new_capacity;
  return 1;
}

static int pe_section_align(LinkedSection *section, size_t alignment,
                            char **error_message_out) {
  size_t aligned_size = 0u;

  if (!section || alignment <= 1u) {
    return 1;
  }

  aligned_size = linker_align_up(section->size, alignment);
  if (aligned_size == section->size) {
    return 1;
  }

  if (!pe_section_reserve(section, aligned_size, error_message_out)) {
    return 0;
  }

  memset(section->data + section->size, 0, aligned_size - section->size);
  section->size = aligned_size;
  if (section->virtual_size < section->size) {
    section->virtual_size = section->size;
  }

  return 1;
}

static int pe_section_append_bytes(LinkedSection *section, const void *data,
                                   size_t size, size_t *offset_out,
                                   char **error_message_out) {
  if (!section || (!data && size != 0u)) {
    return 0;
  }

  if (!pe_section_reserve(section, section->size + size, error_message_out)) {
    return 0;
  }

  if (offset_out) {
    *offset_out = section->size;
  }
  if (size != 0u) {
    memcpy(section->data + section->size, data, size);
    section->size += size;
  }
  if (section->virtual_size < section->size) {
    section->virtual_size = section->size;
  }

  return 1;
}

static int pe_section_append_zeros(LinkedSection *section, size_t size,
                                   size_t *offset_out,
                                   char **error_message_out) {
  if (!section) {
    return 0;
  }

  if (!pe_section_reserve(section, section->size + size, error_message_out)) {
    return 0;
  }

  if (offset_out) {
    *offset_out = section->size;
  }
  if (size != 0u) {
    memset(section->data + section->size, 0, size);
    section->size += size;
  }
  if (section->virtual_size < section->size) {
    section->virtual_size = section->size;
  }

  return 1;
}

static int pe_collect_sections(const LinkResolution *resolution,
                               PeSectionLayout *layouts,
                               size_t *layout_count_out,
                               char **error_message_out) {
  size_t section_index = 0u;
  size_t layout_count = 0u;

  if (!resolution || !layouts || !layout_count_out) {
    mettle_set_error(error_message_out, "Invalid PE emitter section state");
    return 0;
  }

  for (section_index = 0u; section_index < LINKED_SECTION_COUNT; section_index++) {
    const LinkedSection *section = &resolution->sections[section_index];

    if (section->virtual_size == 0u) {
      continue;
    }

    if (section->kind != LINK_SECTION_KIND_BSS && section->size > UINT32_MAX) {
      mettle_set_error(error_message_out,
                   "Section '%s' exceeds PE raw-size limits",
                   section->name ? section->name : "<unknown>");
      return 0;
    }
    if (section->virtual_size > UINT32_MAX) {
      mettle_set_error(error_message_out,
                   "Section '%s' exceeds PE virtual-size limits",
                   section->name ? section->name : "<unknown>");
      return 0;
    }

    layouts[layout_count].merged_section_index = section_index;
    layouts[layout_count].name = section->name;
    layouts[layout_count].source = section;
    layouts[layout_count].characteristics =
        pe_characteristics_for_kind(section->kind);
    layouts[layout_count].virtual_size = (uint32_t)section->virtual_size;
    layouts[layout_count].raw_size =
        section->kind == LINK_SECTION_KIND_BSS ? 0u : (uint32_t)section->size;
    layout_count++;
  }

  if (layout_count == 0u) {
    mettle_set_error(error_message_out, "No merged sections are available to emit");
    return 0;
  }

  *layout_count_out = layout_count;
  return 1;
}

static int pe_layout_sections(PeSectionLayout *layouts, size_t layout_count,
                              uint32_t section_alignment,
                              uint32_t file_alignment,
                              uint32_t size_of_headers,
                              uint32_t *size_of_image_out,
                              uint32_t *size_of_code_out,
                              uint32_t *size_of_init_data_out,
                              uint32_t *size_of_uninit_data_out,
                              uint32_t *base_of_code_out,
                              char **error_message_out) {
  uint32_t current_rva = 0u;
  uint32_t current_raw = 0u;
  uint32_t size_of_code = 0u;
  uint32_t size_of_init_data = 0u;
  uint32_t size_of_uninit_data = 0u;
  uint32_t base_of_code = 0u;
  size_t i = 0u;

  if (!layouts || !size_of_image_out || !size_of_code_out ||
      !size_of_init_data_out || !size_of_uninit_data_out || !base_of_code_out) {
    mettle_set_error(error_message_out, "Invalid PE layout output");
    return 0;
  }

  current_rva = (uint32_t)linker_align_up(size_of_headers, section_alignment);
  current_raw = (uint32_t)linker_align_up(size_of_headers, file_alignment);

  for (i = 0u; i < layout_count; i++) {
    uint32_t aligned_virtual_size = 0u;
    LinkSectionKind kind = layouts[i].source->kind;

    layouts[i].virtual_address = current_rva;
    layouts[i].raw_offset = layouts[i].raw_size == 0u ? 0u : current_raw;
    if (layouts[i].raw_size != 0u) {
      layouts[i].raw_size = (uint32_t)linker_align_up(layouts[i].raw_size, file_alignment);
    }

    aligned_virtual_size =
        (uint32_t)linker_align_up(layouts[i].virtual_size, section_alignment);
    current_rva += aligned_virtual_size;
    if (layouts[i].raw_size != 0u) {
      current_raw += layouts[i].raw_size;
    }

    switch (kind) {
    case LINK_SECTION_KIND_TEXT:
      size_of_code += layouts[i].raw_size;
      if (base_of_code == 0u) {
        base_of_code = layouts[i].virtual_address;
      }
      break;
    case LINK_SECTION_KIND_RDATA:
    case LINK_SECTION_KIND_PDATA:
    case LINK_SECTION_KIND_XDATA:
    case LINK_SECTION_KIND_DATA:
      size_of_init_data += layouts[i].raw_size;
      break;
    case LINK_SECTION_KIND_BSS:
      size_of_uninit_data += layouts[i].virtual_size;
      break;
    case LINK_SECTION_KIND_UNKNOWN:
    default:
      break;
    }
  }

  *size_of_image_out = (uint32_t)linker_align_up(current_rva, section_alignment);
  *size_of_code_out = size_of_code;
  *size_of_init_data_out = size_of_init_data;
  *size_of_uninit_data_out = size_of_uninit_data;
  *base_of_code_out = base_of_code;
  return 1;
}

static int pe_apply_layout_to_resolution(LinkResolution *resolution,
                                         const PeSectionLayout *layouts,
                                         size_t layout_count,
                                         uint64_t image_base,
                                         char **error_message_out) {
  size_t object_index = 0u;
  size_t symbol_index = 0u;
  size_t section_index = 0u;

  if (!resolution || !layouts) {
    mettle_set_error(error_message_out,
                 "Invalid resolution while laying out PE image");
    return 0;
  }

  for (section_index = 0u; section_index < LINKED_SECTION_COUNT; section_index++) {
    resolution->sections[section_index].virtual_address = 0u;
  }

  for (section_index = 0u; section_index < layout_count; section_index++) {
    resolution->sections[layouts[section_index].merged_section_index]
        .virtual_address = image_base + layouts[section_index].virtual_address;
  }

  for (object_index = 0u; object_index < resolution->object_count; object_index++) {
    LinkedInputObject *input = &resolution->objects[object_index];

    for (symbol_index = 0u; symbol_index < input->symbol_count; symbol_index++) {
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

  for (symbol_index = 0u; symbol_index < resolution->symbol_count; symbol_index++) {
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

static int pe_import_plan_reserve_dlls(PeImportPlan *plan, size_t minimum_count,
                                       char **error_message_out) {
  PeImportDll *grown = NULL;
  size_t new_capacity = 0u;

  if (!plan) {
    return 0;
  }
  if (plan->dll_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = plan->dll_capacity ? plan->dll_capacity : 4u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(plan->dlls, new_capacity * sizeof(PeImportDll));
  if (!grown) {
    mettle_set_error(error_message_out,
                 "Out of memory while growing import DLL table");
    return 0;
  }

  memset(grown + plan->dll_capacity, 0,
         (new_capacity - plan->dll_capacity) * sizeof(PeImportDll));
  plan->dlls = grown;
  plan->dll_capacity = new_capacity;
  return 1;
}

static int pe_import_plan_reserve_symbols(PeImportPlan *plan,
                                          size_t minimum_count,
                                          char **error_message_out) {
  PeImportSymbol *grown = NULL;
  size_t new_capacity = 0u;

  if (!plan) {
    return 0;
  }
  if (plan->symbol_capacity >= minimum_count) {
    return 1;
  }

  new_capacity = plan->symbol_capacity ? plan->symbol_capacity : 8u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }

  grown = realloc(plan->symbols, new_capacity * sizeof(PeImportSymbol));
  if (!grown) {
    mettle_set_error(error_message_out,
                 "Out of memory while growing import symbol table");
    return 0;
  }

  memset(grown + plan->symbol_capacity, 0,
         (new_capacity - plan->symbol_capacity) * sizeof(PeImportSymbol));
  plan->symbols = grown;
  plan->symbol_capacity = new_capacity;
  return 1;
}

static ssize_t pe_import_plan_find_dll(const PeImportPlan *plan,
                                       const char *dll_name) {
  size_t i = 0u;

  if (!plan || !dll_name) {
    return -1;
  }

  for (i = 0u; i < plan->dll_count; i++) {
    if (plan->dlls[i].dll_name && strcmp(plan->dlls[i].dll_name, dll_name) == 0) {
      return (ssize_t)i;
    }
  }

  return -1;
}

static ssize_t pe_import_plan_find_symbol(const PeImportPlan *plan,
                                          const char *canonical_name,
                                          const char *dll_name) {
  size_t i = 0u;

  if (!plan || !canonical_name || !dll_name) {
    return -1;
  }

  for (i = 0u; i < plan->symbol_count; i++) {
    if (plan->symbols[i].canonical_name &&
        plan->symbols[i].dll_name &&
        strcmp(plan->symbols[i].canonical_name, canonical_name) == 0 &&
        strcmp(plan->symbols[i].dll_name, dll_name) == 0) {
      return (ssize_t)i;
    }
  }

  return -1;
}

static void pe_import_plan_destroy(PeImportPlan *plan) {
  size_t i = 0u;

  if (!plan) {
    return;
  }

  for (i = 0u; i < plan->dll_count; i++) {
    free(plan->dlls[i].dll_name);
  }
  for (i = 0u; i < plan->symbol_count; i++) {
    free(plan->symbols[i].canonical_name);
    free(plan->symbols[i].dll_name);
  }

  free(plan->dlls);
  free(plan->symbols);
  memset(plan, 0, sizeof(*plan));
}

static void pe_destroy_import_libraries(ImportLibrary **libraries,
                                        size_t library_count) {
  size_t i = 0u;

  if (!libraries) {
    return;
  }

  for (i = 0u; i < library_count; i++) {
    import_library_destroy(libraries[i]);
  }

  free(libraries);
}

static int pe_import_libraries_append(ImportLibrary ***libraries_in_out,
                                      size_t *library_count_in_out,
                                      ImportLibrary *library,
                                      char **error_message_out) {
  ImportLibrary **grown = NULL;

  if (!libraries_in_out || !library_count_in_out || !library) {
    return 0;
  }

  grown = realloc(*libraries_in_out,
                  (*library_count_in_out + 1u) * sizeof(ImportLibrary *));
  if (!grown) {
    mettle_set_error(error_message_out,
                 "Out of memory while growing import library list");
    return 0;
  }

  grown[*library_count_in_out] = library;
  *libraries_in_out = grown;
  *library_count_in_out += 1u;
  return 1;
}

static int pe_load_import_libraries(const char **paths, size_t path_count,
                                    ImportLibrary ***libraries_out,
                                    size_t *library_count_out,
                                    char **error_message_out) {
  ImportLibrary **libraries = NULL;
  size_t i = 0u;
  int ok = 0;

  if (libraries_out) {
    *libraries_out = NULL;
  }
  if (library_count_out) {
    *library_count_out = 0u;
  }
  if (!paths || path_count == 0u) {
    return 1;
  }

  libraries = calloc(path_count, sizeof(ImportLibrary *));
  if (!libraries) {
    mettle_set_error(error_message_out,
                 "Out of memory while loading import libraries");
    return 0;
  }

  for (i = 0u; i < path_count; i++) {
    if (!paths[i] || paths[i][0] == '\0') {
      mettle_set_error(error_message_out,
                   "An import library path was empty");
      goto cleanup;
    }
    if (!import_library_read(paths[i], &libraries[i], error_message_out)) {
      goto cleanup;
    }
  }

  ok = 1;

cleanup:
  if (!ok) {
    pe_destroy_import_libraries(libraries, path_count);
    return 0;
  }

  *libraries_out = libraries;
  *library_count_out = path_count;
  return 1;
}

static int pe_import_library_has_symbol(const ImportLibrary *library,
                                        const char *symbol_name) {
  return import_library_find_symbol(library, symbol_name) != NULL;
}

static int pe_import_library_append_symbol(ImportLibrary *library,
                                           const char *symbol_name,
                                           const char *dll_name,
                                           char **error_message_out) {
  ImportLibrarySymbol *grown = NULL;
  size_t new_capacity = 0u;
  ImportLibrarySymbol *symbol = NULL;

  if (!library || !symbol_name || !dll_name) {
    return 0;
  }

  if (library->symbol_count == library->symbol_capacity) {
    new_capacity = library->symbol_capacity ? library->symbol_capacity * 2u : 8u;
    grown = realloc(library->symbols, new_capacity * sizeof(ImportLibrarySymbol));
    if (!grown) {
      mettle_set_error(error_message_out,
                   "Out of memory while growing synthetic import symbols");
      return 0;
    }
    memset(grown + library->symbol_capacity, 0,
           (new_capacity - library->symbol_capacity) *
               sizeof(ImportLibrarySymbol));
    library->symbols = grown;
    library->symbol_capacity = new_capacity;
  }

  symbol = &library->symbols[library->symbol_count++];
  memset(symbol, 0, sizeof(*symbol));
  symbol->symbol_name = mettle_strdup(symbol_name);
  symbol->dll_name = mettle_strdup(dll_name);
  if (!symbol->symbol_name || !symbol->dll_name) {
    free(symbol->symbol_name);
    free(symbol->dll_name);
    memset(symbol, 0, sizeof(*symbol));
    library->symbol_count--;
    mettle_set_error(error_message_out,
                 "Out of memory while storing synthetic import symbol '%s'",
                 symbol_name);
    return 0;
  }

  symbol->machine = COFF_MACHINE_AMD64;
  symbol->ordinal_or_hint = 0u;
  symbol->import_type = IMPORT_OBJECT_TYPE_CODE;
  symbol->name_type = IMPORT_OBJECT_NAME_TYPE_NAME;
  symbol->is_ordinal = 0;
  return 1;
}

static char *pe_normalize_dll_name(const char *dll_name) {
  size_t length = 0u;
  char *normalized = NULL;

  if (!dll_name || dll_name[0] == '\0') {
    return NULL;
  }

  length = strlen(dll_name);
  if (length >= 4u && pe_text_ends_with_ignore_case(dll_name, ".dll")) {
    return mettle_strdup(dll_name);
  }

  normalized = malloc(length + 5u);
  if (!normalized) {
    return NULL;
  }

  memcpy(normalized, dll_name, length);
  memcpy(normalized + length, ".dll", 5u);
  return normalized;
}

#ifdef _WIN32
static int pe_append_imports_from_dlls(LinkResolution *resolution,
                                       const char **dll_names, size_t dll_count,
                                       ImportLibrary ***libraries_in_out,
                                       size_t *library_count_in_out,
                                       char **error_message_out) {
  size_t dll_index = 0u;

  if (!resolution || !libraries_in_out || !library_count_in_out) {
    return 0;
  }

  for (dll_index = 0u; dll_index < dll_count; dll_index++) {
    ImportLibrary *library = NULL;
    HMODULE module = NULL;
    char *normalized_dll_name = NULL;
    size_t symbol_index = 0u;

    if (!dll_names || !dll_names[dll_index] || dll_names[dll_index][0] == '\0') {
      continue;
    }

    normalized_dll_name = pe_normalize_dll_name(dll_names[dll_index]);
    if (!normalized_dll_name) {
      mettle_set_error(error_message_out,
                   "Out of memory while normalizing import DLL '%s'",
                   dll_names[dll_index]);
      return 0;
    }

    module = LoadLibraryA(normalized_dll_name);
    if (!module) {
      free(normalized_dll_name);
      continue;
    }

    library = calloc(1, sizeof(*library));
    if (!library) {
      FreeLibrary(module);
      free(normalized_dll_name);
      mettle_set_error(error_message_out,
                   "Out of memory while creating synthetic import library");
      return 0;
    }

    library->path = mettle_strdup(normalized_dll_name);
    if (!library->path) {
      FreeLibrary(module);
      free(normalized_dll_name);
      import_library_destroy(library);
      mettle_set_error(error_message_out,
                   "Out of memory while storing synthetic import DLL name");
      return 0;
    }

    for (symbol_index = 0u; symbol_index < resolution->symbol_count;
         symbol_index++) {
      const LinkedSymbol *symbol = &resolution->symbols[symbol_index];
      const char *canonical_name = NULL;

      if (!symbol->is_external || symbol->is_defined || !symbol->name) {
        continue;
      }

      canonical_name =
          pe_is_imp_symbol_name(symbol->name) ? symbol->name + PE_IMP_PREFIX_LEN
                                              : symbol->name;
      if (pe_find_import_symbol(*libraries_in_out, *library_count_in_out,
                                canonical_name) ||
          pe_import_library_has_symbol(library, canonical_name)) {
        continue;
      }

      if (GetProcAddress(module, canonical_name) &&
          !pe_import_library_append_symbol(library, canonical_name,
                                           normalized_dll_name,
                                           error_message_out)) {
        FreeLibrary(module);
        free(normalized_dll_name);
        import_library_destroy(library);
        return 0;
      }
    }

    FreeLibrary(module);
    free(normalized_dll_name);

    if (library->symbol_count == 0u) {
      import_library_destroy(library);
      continue;
    }

    if (!pe_import_libraries_append(libraries_in_out, library_count_in_out, library,
                                    error_message_out)) {
      import_library_destroy(library);
      return 0;
    }
  }

  return 1;
}
#else
static int pe_append_imports_from_dlls(LinkResolution *resolution,
                                       const char **dll_names, size_t dll_count,
                                       ImportLibrary ***libraries_in_out,
                                       size_t *library_count_in_out,
                                       char **error_message_out) {
  (void)resolution;
  (void)dll_names;
  (void)dll_count;
  (void)libraries_in_out;
  (void)library_count_in_out;

  if (dll_count > 0u) {
    mettle_set_error(error_message_out,
                 "DLL export probing for PE imports requires Windows");
    return 0;
  }

  return 1;
}
#endif

static const ImportLibrarySymbol *
pe_find_import_symbol(ImportLibrary **libraries, size_t library_count,
                      const char *canonical_name) {
  size_t i = 0u;
  const ImportLibrarySymbol *symbol = NULL;

  if (!libraries || !canonical_name) {
    return NULL;
  }

  for (i = 0u; i < library_count; i++) {
    symbol = import_library_find_symbol(libraries[i], canonical_name);
    if (symbol) {
      return symbol;
    }
  }

  return NULL;
}

static int pe_is_imp_symbol_name(const char *symbol_name) {
  return symbol_name && strncmp(symbol_name, PE_IMP_PREFIX, PE_IMP_PREFIX_LEN) == 0;
}

static int pe_bind_import_symbol(LinkResolution *resolution, PeImportPlan *plan,
                                 const ImportLibrarySymbol *library_symbol,
                                 const char *canonical_name, int needs_iat_symbol,
                                 char **error_message_out) {
  ssize_t dll_index = -1;
  ssize_t symbol_index = -1;
  PeImportSymbol *symbol = NULL;

  if (!resolution || !plan || !library_symbol || !canonical_name) {
    return 0;
  }

  if (!needs_iat_symbol &&
      library_symbol->import_type != IMPORT_OBJECT_TYPE_CODE) {
    mettle_set_error(error_message_out,
                 "Direct imported data symbol '%s' is not supported",
                 canonical_name);
    return 0;
  }

  dll_index = pe_import_plan_find_dll(plan, library_symbol->dll_name);
  if (dll_index < 0) {
    if (!pe_import_plan_reserve_dlls(plan, plan->dll_count + 1u,
                                     error_message_out)) {
      return 0;
    }

    dll_index = (ssize_t)plan->dll_count++;
    memset(&plan->dlls[(size_t)dll_index], 0, sizeof(plan->dlls[0]));
    plan->dlls[(size_t)dll_index].dll_name =
        mettle_strdup(library_symbol->dll_name);
    if (!plan->dlls[(size_t)dll_index].dll_name) {
      mettle_set_error(error_message_out,
                   "Out of memory while storing import DLL name");
      return 0;
    }
  }

  symbol_index = pe_import_plan_find_symbol(plan, canonical_name,
                                            library_symbol->dll_name);
  if (symbol_index < 0) {
    if (!pe_import_plan_reserve_symbols(plan, plan->symbol_count + 1u,
                                        error_message_out)) {
      return 0;
    }

    symbol_index = (ssize_t)plan->symbol_count++;
    symbol = &plan->symbols[(size_t)symbol_index];
    memset(symbol, 0, sizeof(*symbol));
    symbol->canonical_name = mettle_strdup(canonical_name);
    symbol->dll_name = mettle_strdup(library_symbol->dll_name);
    if (!symbol->canonical_name || !symbol->dll_name) {
      mettle_set_error(error_message_out,
                   "Out of memory while storing import symbol '%s'",
                   canonical_name);
      return 0;
    }
    symbol->dll_index = (size_t)dll_index;
    symbol->dll_slot_index = plan->dlls[(size_t)dll_index].symbol_count++;
    symbol->ordinal_or_hint = library_symbol->ordinal_or_hint;
    symbol->import_type = library_symbol->import_type;
    symbol->name_type = library_symbol->name_type;
    symbol->is_ordinal = library_symbol->is_ordinal;
  } else {
    symbol = &plan->symbols[(size_t)symbol_index];
  }

  if (needs_iat_symbol) {
    symbol->needs_iat_symbol = 1;
  } else {
    symbol->needs_thunk = 1;
  }

  plan->has_imports = 1;
  return 1;
}

static int pe_build_import_plan(LinkResolution *resolution,
                                ImportLibrary **libraries,
                                size_t library_count, PeImportPlan *plan,
                                char **error_message_out) {
  size_t symbol_index = 0u;

  if (!resolution || !plan) {
    return 0;
  }

  for (symbol_index = 0u; symbol_index < resolution->symbol_count;
       symbol_index++) {
    LinkedSymbol *symbol = &resolution->symbols[symbol_index];
    const char *lookup_name = NULL;
    const ImportLibrarySymbol *library_symbol = NULL;
    int needs_iat_symbol = 0;

    if (!symbol->is_external || symbol->is_defined || !symbol->name) {
      continue;
    }

    needs_iat_symbol = pe_is_imp_symbol_name(symbol->name);
    lookup_name = needs_iat_symbol ? symbol->name + PE_IMP_PREFIX_LEN : symbol->name;
    library_symbol = pe_find_import_symbol(libraries, library_count, lookup_name);
    if (!library_symbol) {
      continue;
    }

    if (!pe_bind_import_symbol(resolution, plan, library_symbol, lookup_name,
                               needs_iat_symbol, error_message_out)) {
      return 0;
    }
  }

  return 1;
}

static int pe_append_import_hint_name(LinkedSection *rdata,
                                      const PeImportSymbol *symbol,
                                      size_t *offset_out,
                                      char **error_message_out) {
  unsigned char hint[2];

  if (!rdata || !symbol || !offset_out) {
    return 0;
  }

  if (!pe_section_align(rdata, 2u, error_message_out)) {
    return 0;
  }

  hint[0] = (unsigned char)(symbol->ordinal_or_hint & 0xFFu);
  hint[1] = (unsigned char)((symbol->ordinal_or_hint >> 8u) & 0xFFu);
  if (!pe_section_append_bytes(rdata, hint, sizeof(hint), offset_out,
                               error_message_out) ||
      !pe_section_append_bytes(rdata, symbol->canonical_name,
                               strlen(symbol->canonical_name) + 1u, NULL,
                               error_message_out)) {
    return 0;
  }

  return 1;
}

static int pe_emit_import_storage(LinkResolution *resolution,
                                  PeImportPlan *plan,
                                  char **error_message_out) {
  LinkedSection *text = NULL;
  LinkedSection *rdata = NULL;
  size_t i = 0u;

  if (!resolution || !plan || !plan->has_imports) {
    return 1;
  }

  text = &resolution->sections[PE_SECTION_INDEX_TEXT];
  rdata = &resolution->sections[PE_SECTION_INDEX_RDATA];

  if (!pe_section_align(rdata, 8u, error_message_out) ||
      !pe_section_append_zeros(
          rdata, (plan->dll_count + 1u) * PE_IMPORT_DESCRIPTOR_SIZE,
          &plan->descriptor_table_offset, error_message_out)) {
    return 0;
  }

  for (i = 0u; i < plan->dll_count; i++) {
    PeImportDll *dll = &plan->dlls[i];

    if (!pe_section_align(rdata, 8u, error_message_out) ||
        !pe_section_append_zeros(
            rdata, (dll->symbol_count + 1u) * PE_IMPORT_THUNK_SIZE,
            &dll->ilt_offset, error_message_out)) {
      return 0;
    }
  }

  for (i = 0u; i < plan->symbol_count; i++) {
    if (plan->symbols[i].is_ordinal) {
      continue;
    }
    if (!pe_append_import_hint_name(rdata, &plan->symbols[i],
                                    &plan->symbols[i].hint_name_offset,
                                    error_message_out)) {
      return 0;
    }
  }

  for (i = 0u; i < plan->dll_count; i++) {
    PeImportDll *dll = &plan->dlls[i];

    if (!pe_section_append_bytes(rdata, dll->dll_name, strlen(dll->dll_name) + 1u,
                                 &dll->name_offset, error_message_out)) {
      return 0;
    }
  }

  if (!pe_section_align(rdata, 8u, error_message_out)) {
    return 0;
  }

  for (i = 0u; i < plan->dll_count; i++) {
    PeImportDll *dll = &plan->dlls[i];

    if (!pe_section_append_zeros(
            rdata, (dll->symbol_count + 1u) * PE_IMPORT_THUNK_SIZE,
            &dll->iat_offset, error_message_out)) {
      return 0;
    }
  }

  for (i = 0u; i < plan->symbol_count; i++) {
    PeImportSymbol *symbol = &plan->symbols[i];
    PeImportDll *dll = &plan->dlls[symbol->dll_index];
    LinkedSymbol *global = NULL;

    symbol->iat_offset = dll->iat_offset +
                         (symbol->dll_slot_index * PE_IMPORT_THUNK_SIZE);
    if (symbol->needs_iat_symbol) {
      char imp_name[512];

      if (snprintf(imp_name, sizeof(imp_name), "__imp_%s",
                   symbol->canonical_name) >= (int)sizeof(imp_name)) {
        mettle_set_error(error_message_out,
                     "Imported symbol name '%s' is too long",
                     symbol->canonical_name);
        return 0;
      }

      global = pe_find_symbol_mutable(resolution, imp_name);
      if (global) {
        global->is_defined = 1;
        global->merged_section_index = PE_SECTION_INDEX_RDATA;
        global->merged_offset = symbol->iat_offset;
      }
    }

    if (symbol->needs_thunk) {
      if (!pe_section_align(text, 16u, error_message_out) ||
          !pe_section_append_zeros(text, PE_IMPORT_JUMP_STUB_SIZE,
                                   &symbol->thunk_offset, error_message_out)) {
        return 0;
      }

      global = pe_find_symbol_mutable(resolution, symbol->canonical_name);
      if (global) {
        global->is_defined = 1;
        global->merged_section_index = PE_SECTION_INDEX_TEXT;
        global->merged_offset = symbol->thunk_offset;
      }
    }
  }

  return 1;
}

static const PeSectionLayout *pe_find_layout(const PeSectionLayout *layouts,
                                             size_t layout_count,
                                             size_t merged_section_index) {
  size_t i = 0u;

  if (!layouts) {
    return NULL;
  }

  for (i = 0u; i < layout_count; i++) {
    if (layouts[i].merged_section_index == merged_section_index) {
      return &layouts[i];
    }
  }

  return NULL;
}

static int pe_finalize_imports(LinkResolution *resolution, PeImportPlan *plan,
                               const PeSectionLayout *layouts, size_t layout_count,
                               uint64_t image_base,
                               char **error_message_out) {
  const PeSectionLayout *text_layout = NULL;
  const PeSectionLayout *rdata_layout = NULL;
  LinkedSection *text = NULL;
  LinkedSection *rdata = NULL;
  size_t i = 0u;
  size_t iat_begin = 0u;
  size_t iat_end = 0u;

  if (!plan || !plan->has_imports) {
    return 1;
  }
  if (!resolution || !layouts) {
    mettle_set_error(error_message_out,
                 "Invalid layout while finalizing import tables");
    return 0;
  }

  text_layout = pe_find_layout(layouts, layout_count, PE_SECTION_INDEX_TEXT);
  rdata_layout = pe_find_layout(layouts, layout_count, PE_SECTION_INDEX_RDATA);
  if (!text_layout || !rdata_layout) {
    mettle_set_error(error_message_out,
                 "Imported images require both .text and .rdata sections");
    return 0;
  }

  text = &resolution->sections[PE_SECTION_INDEX_TEXT];
  rdata = &resolution->sections[PE_SECTION_INDEX_RDATA];
  iat_begin = plan->dlls[0].iat_offset;
  iat_end = iat_begin;

  for (i = 0u; i < plan->symbol_count; i++) {
    const PeImportSymbol *symbol = &plan->symbols[i];
    PeImportDll *dll = &plan->dlls[symbol->dll_index];
    uint64_t import_name_rva = 0u;
    uint64_t thunk_value = 0u;
    size_t ilt_offset = dll->ilt_offset +
                        (symbol->dll_slot_index * PE_IMPORT_THUNK_SIZE);
    size_t iat_offset = symbol->iat_offset;

    if (!symbol->is_ordinal) {
      import_name_rva =
          rdata_layout->virtual_address + (uint32_t)symbol->hint_name_offset;
      thunk_value = import_name_rva;
    } else {
      thunk_value = 0x8000000000000000ull | symbol->ordinal_or_hint;
    }

    linker_write_u64(rdata->data + ilt_offset, thunk_value);
    linker_write_u64(rdata->data + iat_offset, thunk_value);

    if (symbol->needs_thunk) {
      uint64_t thunk_va = text_layout->virtual_address + (uint32_t)symbol->thunk_offset +
                          image_base;
      uint64_t iat_va = rdata_layout->virtual_address + (uint32_t)iat_offset +
                        image_base;
      int64_t displacement = (int64_t)iat_va - (int64_t)(thunk_va + 6u);
      unsigned char *stub = text->data + symbol->thunk_offset;

      if (displacement < INT32_MIN || displacement > INT32_MAX) {
        mettle_set_error(error_message_out,
                     "Import thunk for '%s' is out of range",
                     symbol->canonical_name);
        return 0;
      }

      stub[0] = 0xFFu;
      stub[1] = 0x25u;
      linker_write_u32(stub + 2u, (uint32_t)(int32_t)displacement);
    }

  }

  for (i = 0u; i < plan->dll_count; i++) {
    PeImportDll *dll = &plan->dlls[i];
    unsigned char *descriptor =
        rdata->data + plan->descriptor_table_offset +
        (i * PE_IMPORT_DESCRIPTOR_SIZE);
    uint32_t ilt_rva =
        rdata_layout->virtual_address + (uint32_t)dll->ilt_offset;
    uint32_t dll_name_rva =
        rdata_layout->virtual_address + (uint32_t)dll->name_offset;
    uint32_t iat_rva =
        rdata_layout->virtual_address + (uint32_t)dll->iat_offset;

    linker_write_u32(descriptor, ilt_rva);
    linker_write_u32(descriptor + 4u, 0u);
    linker_write_u32(descriptor + 8u, 0u);
    linker_write_u32(descriptor + 12u, dll_name_rva);
    linker_write_u32(descriptor + 16u, iat_rva);

    if (dll->iat_offset + ((dll->symbol_count + 1u) * PE_IMPORT_THUNK_SIZE) >
        iat_end) {
      iat_end =
          dll->iat_offset + ((dll->symbol_count + 1u) * PE_IMPORT_THUNK_SIZE);
    }
  }

  plan->import_directory_rva =
      rdata_layout->virtual_address + (uint32_t)plan->descriptor_table_offset;
  plan->import_directory_size =
      (uint32_t)((plan->dll_count + 1u) * PE_IMPORT_DESCRIPTOR_SIZE);
  plan->iat_rva = rdata_layout->virtual_address + (uint32_t)iat_begin;
  plan->iat_size = (uint32_t)(iat_end - iat_begin);
  return 1;
}

static int pe_is_exportable_name(const char *name) {
  if (!name || name[0] == '\0') {
    return 0;
  }
  if (pe_is_imp_symbol_name(name)) {
    return 0;
  }
  if (strcmp(name, "main") == 0 || strcmp(name, "mettle_start") == 0 ||
      strcmp(name, "_start") == 0) {
    return 0;
  }
  /* Compiler-owned tables (crash/debug/profile metadata) live in the user
   * object so they resolve without a startup link, but they are not part of
   * the DLL interface. Only `export fn/var` names cross it. */
  if (strncmp(name, "mettle_", 7) == 0) {
    return 0;
  }
  return 1;
}

static int pe_export_name_compare(const void *left, const void *right) {
  const LinkedSymbol *const *left_symbol =
      (const LinkedSymbol *const *)left;
  const LinkedSymbol *const *right_symbol =
      (const LinkedSymbol *const *)right;
  const char *left_name =
      *left_symbol && (*left_symbol)->name ? (*left_symbol)->name : "";
  const char *right_name =
      *right_symbol && (*right_symbol)->name ? (*right_symbol)->name : "";
  return strcmp(left_name, right_name);
}

static void pe_export_plan_destroy(PeExportPlan *plan) {
  if (!plan) {
    return;
  }
  free(plan->exports);
  free(plan->name_offsets);
  free(plan->dll_name);
  memset(plan, 0, sizeof(*plan));
}

static int pe_export_plan_reserve(PeExportPlan *plan, size_t minimum_count,
                                  char **error_message_out) {
  const LinkedSymbol **grown_exports = NULL;
  size_t *grown_offsets = NULL;
  size_t new_capacity = 0u;

  if (!plan) {
    return 0;
  }
  if (plan->capacity >= minimum_count) {
    return 1;
  }
  new_capacity = plan->capacity ? plan->capacity : 16u;
  while (new_capacity < minimum_count) {
    new_capacity *= 2u;
  }
  grown_exports =
      realloc(plan->exports, new_capacity * sizeof(*plan->exports));
  if (!grown_exports) {
    mettle_set_error(error_message_out,
                 "Out of memory while growing PE export table");
    return 0;
  }
  plan->exports = grown_exports;
  grown_offsets =
      realloc(plan->name_offsets, new_capacity * sizeof(*plan->name_offsets));
  if (!grown_offsets) {
    mettle_set_error(error_message_out,
                 "Out of memory while growing PE export names");
    return 0;
  }
  plan->name_offsets = grown_offsets;
  memset(plan->exports + plan->capacity, 0,
         (new_capacity - plan->capacity) * sizeof(*plan->exports));
  memset(plan->name_offsets + plan->capacity, 0,
         (new_capacity - plan->capacity) * sizeof(*plan->name_offsets));
  plan->capacity = new_capacity;
  return 1;
}

static const char *pe_output_basename(const char *path) {
  const char *slash = NULL;
  const char *back = NULL;
  const char *base = NULL;

  if (!path) {
    return "output.dll";
  }
  slash = strrchr(path, '/');
  back = strrchr(path, '\\');
  base = slash;
  if (!base || (back && back > base)) {
    base = back;
  }
  if (base) {
    base++;
  } else {
    base = path;
  }
  if (base[0] == '\0') {
    return "output.dll";
  }
  return base;
}

static int pe_collect_exports(const LinkResolution *resolution,
                              const char *output_path, const char *dll_name_opt,
                              PeExportPlan *plan, char **error_message_out) {
  size_t i = 0u;
  const char *dll_name = NULL;

  if (!resolution || !plan) {
    return 0;
  }
  memset(plan, 0, sizeof(*plan));

  for (i = 0u; i < resolution->symbol_count; i++) {
    const LinkedSymbol *symbol = &resolution->symbols[i];

    if (!symbol->is_defined || !symbol->is_external || !symbol->name ||
        symbol->merged_section_index == LINKED_SECTION_INDEX_NONE) {
      continue;
    }
    if (!pe_is_exportable_name(symbol->name)) {
      continue;
    }
    if (symbol->defining_object_index < resolution->object_count &&
        resolution->objects[symbol->defining_object_index]
            .is_runtime_default) {
      continue;
    }
    if (!pe_export_plan_reserve(plan, plan->count + 1u, error_message_out)) {
      pe_export_plan_destroy(plan);
      return 0;
    }
    plan->exports[plan->count++] = symbol;
  }

  if (plan->count == 0u) {
    mettle_set_error(error_message_out,
                 "DLL emission requires at least one exported symbol; mark "
                 "functions with `export fn`");
    return 0;
  }

  qsort(plan->exports, plan->count, sizeof(*plan->exports),
        pe_export_name_compare);

  if (dll_name_opt && dll_name_opt[0] != '\0') {
    dll_name = dll_name_opt;
  } else {
    dll_name = pe_output_basename(output_path);
  }
  plan->dll_name = mettle_strdup(dll_name);
  if (!plan->dll_name) {
    mettle_set_error(error_message_out,
                 "Out of memory while storing DLL export name");
    pe_export_plan_destroy(plan);
    return 0;
  }
  plan->has_exports = 1;
  return 1;
}

static int pe_emit_export_storage(LinkResolution *resolution,
                                  PeExportPlan *plan,
                                  char **error_message_out) {
  LinkedSection *rdata = NULL;
  size_t i = 0u;

  if (!resolution || !plan || !plan->has_exports) {
    return 1;
  }
  if (plan->count == 0u || plan->count > UINT32_MAX) {
    mettle_set_error(error_message_out, "Invalid PE export count");
    return 0;
  }

  rdata = &resolution->sections[PE_SECTION_INDEX_RDATA];
  if (!pe_section_align(rdata, 4u, error_message_out) ||
      !pe_section_append_zeros(rdata, PE_EXPORT_DIRECTORY_SIZE,
                               &plan->directory_offset, error_message_out) ||
      !pe_section_append_zeros(rdata, plan->count * 4u,
                               &plan->function_table_offset,
                               error_message_out) ||
      !pe_section_append_zeros(rdata, plan->count * 4u,
                               &plan->name_table_offset, error_message_out)) {
    return 0;
  }
  if (!pe_section_align(rdata, 2u, error_message_out) ||
      !pe_section_append_zeros(rdata, plan->count * 2u,
                               &plan->ordinal_table_offset,
                               error_message_out)) {
    return 0;
  }
  for (i = 0u; i < plan->count; i++) {
    const char *name =
        plan->exports[i] && plan->exports[i]->name ? plan->exports[i]->name
                                                   : "";
    if (!pe_section_append_bytes(rdata, name, strlen(name) + 1u,
                                 &plan->name_offsets[i], error_message_out)) {
      return 0;
    }
  }
  if (!pe_section_append_bytes(rdata, plan->dll_name,
                               strlen(plan->dll_name) + 1u,
                               &plan->dll_name_offset, error_message_out)) {
    return 0;
  }
  return 1;
}

static int pe_finalize_exports(LinkResolution *resolution, PeExportPlan *plan,
                               const PeSectionLayout *layouts,
                               size_t layout_count, uint64_t image_base,
                               char **error_message_out) {
  const PeSectionLayout *rdata_layout = NULL;
  LinkedSection *rdata = NULL;
  unsigned char *directory = NULL;
  size_t i = 0u;

  if (!plan || !plan->has_exports) {
    return 1;
  }
  if (!resolution || !layouts) {
    mettle_set_error(error_message_out,
                 "Invalid layout while finalizing PE exports");
    return 0;
  }
  rdata_layout = pe_find_layout(layouts, layout_count, PE_SECTION_INDEX_RDATA);
  if (!rdata_layout) {
    mettle_set_error(error_message_out,
                 "DLL exports require an .rdata section");
    return 0;
  }
  rdata = &resolution->sections[PE_SECTION_INDEX_RDATA];

  for (i = 0u; i < plan->count; i++) {
    const LinkedSymbol *symbol = plan->exports[i];
    uint64_t function_va = 0u;
    uint32_t function_rva = 0u;
    uint32_t name_rva = 0u;

    if (!symbol || !symbol->is_defined) {
      mettle_set_error(error_message_out, "PE export went missing");
      return 0;
    }
    if (symbol->virtual_address < image_base ||
        symbol->virtual_address - image_base > UINT32_MAX) {
      mettle_set_error(error_message_out,
                   "Exported symbol '%s' is outside the PE image",
                   symbol->name ? symbol->name : "<unnamed>");
      return 0;
    }
    function_va = symbol->virtual_address;
    function_rva = (uint32_t)(function_va - image_base);
    name_rva = rdata_layout->virtual_address +
               (uint32_t)plan->name_offsets[i];
    linker_write_u32(rdata->data + plan->function_table_offset + (i * 4u),
                     function_rva);
    linker_write_u32(rdata->data + plan->name_table_offset + (i * 4u),
                     name_rva);
    rdata->data[plan->ordinal_table_offset + (i * 2u)] =
        (unsigned char)(i & 0xFFu);
    rdata->data[plan->ordinal_table_offset + (i * 2u) + 1u] =
        (unsigned char)((i >> 8) & 0xFFu);
  }

  directory = rdata->data + plan->directory_offset;
  linker_write_u32(directory, 0u);
  linker_write_u32(directory + 4u, 0u);
  directory[8] = 0u;
  directory[9] = 0u;
  directory[10] = 0u;
  directory[11] = 0u;
  linker_write_u32(directory + 12u, rdata_layout->virtual_address +
                                        (uint32_t)plan->dll_name_offset);
  linker_write_u32(directory + 16u, 1u);
  linker_write_u32(directory + 20u, (uint32_t)plan->count);
  linker_write_u32(directory + 24u, (uint32_t)plan->count);
  linker_write_u32(directory + 28u, rdata_layout->virtual_address +
                                        (uint32_t)plan->function_table_offset);
  linker_write_u32(directory + 32u, rdata_layout->virtual_address +
                                        (uint32_t)plan->name_table_offset);
  linker_write_u32(directory + 36u, rdata_layout->virtual_address +
                                        (uint32_t)plan->ordinal_table_offset);

  plan->export_directory_rva =
      rdata_layout->virtual_address + (uint32_t)plan->directory_offset;
  plan->export_directory_size = PE_EXPORT_DIRECTORY_SIZE;
  return 1;
}

static int pe_validate_unresolved_externals(const LinkResolution *resolution,
                                            char **error_message_out) {
  size_t i = 0u;

  if (!resolution) {
    return 0;
  }

  for (i = 0u; i < resolution->symbol_count; i++) {
    const LinkedSymbol *symbol = &resolution->symbols[i];
    if (symbol->is_external && !symbol->is_defined) {
      mettle_set_error(error_message_out,
                   "Unresolved external symbol '%s'",
                   symbol->name ? symbol->name : "<unnamed>");
      return 0;
    }
  }

  return 1;
}

static int pe_write_dos_stub(FILE *file) {
  static const unsigned char dos_header[PE_DOS_HEADER_SIZE] = {
      0x4Du, 0x5Au, 0x90u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u,
      0x00u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0xB8u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
      0x80u, 0x00u, 0x00u, 0x00u};
  static const unsigned char dos_message[PE_DOS_STUB_MESSAGE_SIZE] = {
      0x0Eu, 0x1Fu, 0xBAu, 0x0Eu, 0x00u, 0xB4u, 0x09u, 0xCDu, 0x21u, 0xB8u,
      0x01u, 0x4Cu, 0xCDu, 0x21u, 'T',   'h',   'i',   's',   ' ',   'p',
      'r',   'o',   'g',   'r',   'a',   'm',   ' ',   'c',   'a',   'n',
      'n',   'o',   't',   ' ',   'b',   'e',   ' ',   'r',   'u',   'n',
      ' ',   'i',   'n',   ' ',   'D',   'O',   'S',   ' ',   'm',   'o',
      'd',   'e',   '.',   '\r',  '\r',  '\n',  '$',   0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u};

  return fwrite(dos_header, 1u, sizeof(dos_header), file) == sizeof(dos_header) &&
         fwrite(dos_message, 1u, sizeof(dos_message), file) ==
             sizeof(dos_message);
}

static int pe_write_headers(FILE *file, const PeSectionLayout *layouts,
                            size_t layout_count, uint64_t image_base,
                            uint32_t size_of_headers, uint32_t size_of_image,
                            uint32_t size_of_code, uint32_t size_of_init_data,
                            uint32_t size_of_uninit_data,
                            uint32_t base_of_code, uint32_t entry_rva,
                            uint32_t section_alignment,
                            uint32_t file_alignment, uint16_t subsystem,
                            int is_dll,
                            uint32_t export_rva, uint32_t export_size,
                            uint32_t exception_rva, uint32_t exception_size,
                            uint32_t import_rva, uint32_t import_size,
                            uint32_t iat_rva, uint32_t iat_size,
                            char **error_message_out) {
  uint16_t characteristics = IMAGE_FILE_RELOCS_STRIPPED |
                             IMAGE_FILE_EXECUTABLE_IMAGE |
                             IMAGE_FILE_LINE_NUMS_STRIPPED |
                             IMAGE_FILE_LOCAL_SYMS_STRIPPED |
                             IMAGE_FILE_LARGE_ADDRESS_AWARE;
  size_t i = 0u;
  long header_end = 0;

  if (is_dll) {
    characteristics |= IMAGE_FILE_DLL;
  }

  if (!file || !layouts) {
    mettle_set_error(error_message_out, "Invalid PE header state");
    return 0;
  }

  if (!pe_write_dos_stub(file) || !pe_write_u32(file, PE_SIGNATURE) ||
      !pe_write_u16(file, COFF_MACHINE_AMD64) ||
      !pe_write_u16(file, (uint16_t)layout_count) || !pe_write_u32(file, 0u) ||
      !pe_write_u32(file, 0u) || !pe_write_u32(file, 0u) ||
      !pe_write_u16(file, PE_OPTIONAL_HEADER64_SIZE) ||
      !pe_write_u16(file, characteristics) ||
      !pe_write_u16(file, PE_OPTIONAL_HEADER64_MAGIC) ||
      fputc(1, file) == EOF || fputc(0, file) == EOF ||
      !pe_write_u32(file, size_of_code) ||
      !pe_write_u32(file, size_of_init_data) ||
      !pe_write_u32(file, size_of_uninit_data) ||
      !pe_write_u32(file, entry_rva) || !pe_write_u32(file, base_of_code) ||
      !pe_write_u64(file, image_base) ||
      !pe_write_u32(file, section_alignment) ||
      !pe_write_u32(file, file_alignment) || !pe_write_u16(file, 6u) ||
      !pe_write_u16(file, 0u) || !pe_write_u16(file, 0u) ||
      !pe_write_u16(file, 0u) || !pe_write_u16(file, 6u) ||
      !pe_write_u16(file, 0u) || !pe_write_u32(file, 0u) ||
      !pe_write_u32(file, size_of_image) ||
      !pe_write_u32(file, size_of_headers) || !pe_write_u32(file, 0u) ||
      !pe_write_u16(file, subsystem) || !pe_write_u16(file, 0u) ||
      !pe_write_u64(file, 0x100000u) || !pe_write_u64(file, 0x1000u) ||
      !pe_write_u64(file, 0x100000u) || !pe_write_u64(file, 0x1000u) ||
      !pe_write_u32(file, 0u) ||
      !pe_write_u32(file, PE_DATA_DIRECTORY_COUNT)) {
    mettle_set_error(error_message_out, "Failed while writing PE headers");
    return 0;
  }

  for (i = 0u; i < PE_DATA_DIRECTORY_COUNT; i++) {
    uint32_t directory_rva = 0u;
    uint32_t directory_size = 0u;

    if (i == PE_DIRECTORY_EXPORT) {
      directory_rva = export_rva;
      directory_size = export_size;
    } else if (i == PE_DIRECTORY_EXCEPTION) {
      directory_rva = exception_rva;
      directory_size = exception_size;
    } else if (i == PE_DIRECTORY_IMPORT) {
      directory_rva = import_rva;
      directory_size = import_size;
    } else if (i == PE_DIRECTORY_IAT) {
      directory_rva = iat_rva;
      directory_size = iat_size;
    }

    if (!pe_write_u32(file, directory_rva) ||
        !pe_write_u32(file, directory_size)) {
      mettle_set_error(error_message_out,
                   "Failed while writing PE data directories");
      return 0;
    }
  }

  for (i = 0u; i < layout_count; i++) {
    if (!pe_write_section_name(file, layouts[i].name) ||
        !pe_write_u32(file, layouts[i].virtual_size) ||
        !pe_write_u32(file, layouts[i].virtual_address) ||
        !pe_write_u32(file, layouts[i].raw_size) ||
        !pe_write_u32(file, layouts[i].raw_offset) ||
        !pe_write_u32(file, 0u) || !pe_write_u32(file, 0u) ||
        !pe_write_u16(file, 0u) || !pe_write_u16(file, 0u) ||
        !pe_write_u32(file, layouts[i].characteristics)) {
      mettle_set_error(error_message_out,
                   "Failed while writing PE section headers");
      return 0;
    }
  }

  header_end = ftell(file);
  if (header_end < 0 || (uint32_t)header_end > size_of_headers ||
      !pe_write_zeros(file, size_of_headers - (uint32_t)header_end)) {
    mettle_set_error(error_message_out, "Failed while padding PE headers");
    return 0;
  }

  return 1;
}

static int pe_write_payloads(FILE *file, const PeSectionLayout *layouts,
                             size_t layout_count, char **error_message_out) {
  size_t i = 0u;

  if (!file || !layouts) {
    mettle_set_error(error_message_out, "Invalid PE payload state");
    return 0;
  }

  for (i = 0u; i < layout_count; i++) {
    const LinkedSection *section = layouts[i].source;
    long position = ftell(file);
    uint32_t file_offset = 0u;

    if (layouts[i].raw_size == 0u) {
      continue;
    }
    if (position < 0) {
      mettle_set_error(error_message_out,
                   "Failed to measure PE payload position");
      return 0;
    }
    file_offset = (uint32_t)position;

    if (file_offset > layouts[i].raw_offset ||
        !pe_write_zeros(file, layouts[i].raw_offset - file_offset)) {
      mettle_set_error(error_message_out,
                   "Failed while aligning section payload '%s'",
                   layouts[i].name ? layouts[i].name : "<unknown>");
      return 0;
    }

    if (section->size > 0u &&
        fwrite(section->data, 1u, section->size, file) != section->size) {
      mettle_set_error(error_message_out,
                   "Failed while writing section payload '%s'",
                   layouts[i].name ? layouts[i].name : "<unknown>");
      return 0;
    }

    if (layouts[i].raw_size < section->size ||
        !pe_write_zeros(file, layouts[i].raw_size - (uint32_t)section->size)) {
      mettle_set_error(error_message_out,
                   "Failed while padding section payload '%s'",
                   layouts[i].name ? layouts[i].name : "<unknown>");
      return 0;
    }
  }

  return 1;
}

int pe_emit_executable(LinkResolution *resolution, const char *output_path,
                       const PeEmissionOptions *options,
                       char **error_message_out) {
  PeSectionLayout layouts[LINKED_SECTION_COUNT];
  size_t layout_count = 0u;
  FILE *file = NULL;
  ImportLibrary **libraries = NULL;
  size_t library_count = 0u;
  PeImportPlan import_plan;
  PeExportPlan export_plan;
  uint64_t image_base = 0x140000000ull;
  uint32_t section_alignment = 0x1000u;
  uint32_t file_alignment = 0x200u;
  uint16_t subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
  int is_dll = 0;
  const char *dll_name_opt = NULL;
  uint32_t size_of_headers = 0u;
  uint32_t size_of_image = 0u;
  uint32_t size_of_code = 0u;
  uint32_t size_of_init_data = 0u;
  uint32_t size_of_uninit_data = 0u;
  uint32_t base_of_code = 0u;
  uint32_t entry_rva = 0u;
  uint32_t exception_rva = 0u;
  uint32_t exception_size = 0u;
  LinkRelocationOptions relocation_options;
  int ok = 0;

  memset(&import_plan, 0, sizeof(import_plan));
  memset(&export_plan, 0, sizeof(export_plan));

  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }

  if (!resolution || !output_path || output_path[0] == '\0') {
    mettle_set_error(error_message_out,
                 "A link resolution and output path are required");
    return 0;
  }
  if (options && options->produce_shared_library) {
    is_dll = 1;
    dll_name_opt = options->dll_name;
  }
  if (!is_dll) {
    if (!resolution->entry_symbol || !resolution->entry_symbol->is_defined ||
        resolution->entry_symbol->merged_section_index ==
            LINKED_SECTION_INDEX_NONE) {
      mettle_set_error(error_message_out,
                   "PE emission requires a resolved entry point");
      return 0;
    }
  }

  if (options) {
    if (options->image_base != 0u) {
      image_base = options->image_base;
    }
    if (options->section_alignment >= 0x200u) {
      section_alignment = options->section_alignment;
    }
    if (options->file_alignment >= 0x200u) {
      file_alignment = options->file_alignment;
    }
    if (options->subsystem != 0u) {
      subsystem = options->subsystem;
    }
  }

  if (section_alignment < file_alignment) {
    mettle_set_error(error_message_out,
                 "PE section alignment must be at least the file alignment");
    return 0;
  }

  if (options && options->import_library_paths &&
      options->import_library_count > 0u &&
      !pe_load_import_libraries(options->import_library_paths,
                                options->import_library_count, &libraries,
                                &library_count, error_message_out)) {
    goto cleanup;
  }
  if (options && options->import_dll_names && options->import_dll_count > 0u &&
      !pe_append_imports_from_dlls(resolution, options->import_dll_names,
                                   options->import_dll_count, &libraries,
                                   &library_count, error_message_out)) {
    goto cleanup;
  }

  if (!pe_build_import_plan(resolution, libraries, library_count, &import_plan,
                            error_message_out) ||
      !pe_emit_import_storage(resolution, &import_plan, error_message_out)) {
    goto cleanup;
  }
  if (is_dll) {
    if (!pe_collect_exports(resolution, output_path, dll_name_opt,
                            &export_plan, error_message_out) ||
        !pe_emit_export_storage(resolution, &export_plan, error_message_out)) {
      goto cleanup;
    }
  }
  if (!pe_validate_unresolved_externals(resolution, error_message_out) ||
      !pe_collect_sections(resolution, layouts, &layout_count,
                           error_message_out)) {
    goto cleanup;
  }

  size_of_headers = (uint32_t)linker_align_up(
      PE_DOS_STUB_SIZE + 4u + PE_FILE_HEADER_SIZE + PE_OPTIONAL_HEADER64_SIZE +
          ((uint32_t)layout_count * PE_SECTION_HEADER_SIZE),
      file_alignment);
  if (!pe_layout_sections(layouts, layout_count, section_alignment, file_alignment,
                          size_of_headers, &size_of_image, &size_of_code,
                          &size_of_init_data, &size_of_uninit_data,
                          &base_of_code, error_message_out) ||
      !pe_apply_layout_to_resolution(resolution, layouts, layout_count, image_base,
                                     error_message_out) ||
      !pe_finalize_imports(resolution, &import_plan, layouts, layout_count,
                           image_base, error_message_out) ||
      !pe_finalize_exports(resolution, &export_plan, layouts, layout_count,
                           image_base, error_message_out)) {
    goto cleanup;
  }

  if (!is_dll) {
    entry_rva =
        (uint32_t)(resolution->entry_symbol->virtual_address - image_base);
  } else if (resolution->entry_symbol && resolution->entry_symbol->is_defined &&
             resolution->entry_symbol->merged_section_index !=
                 LINKED_SECTION_INDEX_NONE) {
    entry_rva =
        (uint32_t)(resolution->entry_symbol->virtual_address - image_base);
  } else {
    entry_rva = 0u;
  }
  {
    const PeSectionLayout *exception_layout =
        pe_find_layout(layouts, layout_count, PE_SECTION_INDEX_PDATA);
    if (exception_layout && exception_layout->source &&
        exception_layout->source->size > 0u) {
      exception_rva = exception_layout->virtual_address;
      exception_size = (uint32_t)exception_layout->source->size;
    }
  }
  relocation_options.image_base = image_base;
  if (!link_apply_relocations(resolution, &relocation_options, error_message_out)) {
    goto cleanup;
  }

  /* Read/write, so the ownership check below can read the image back through
   * this handle. Opening a finished executable is what a virus scanner charges
   * for, and it was costing more than the whole link: remove any previous
   * output first so this open finds no old image to inspect, and never open
   * the file a second time. */
  remove(output_path);
  file = fopen(output_path, "w+b");
  if (!file) {
    mettle_set_error(error_message_out, "Failed to open PE output file '%s'",
                 output_path);
    goto cleanup;
  }

  if (!pe_write_headers(file, layouts, layout_count, image_base, size_of_headers,
                        size_of_image, size_of_code, size_of_init_data,
                        size_of_uninit_data, base_of_code, entry_rva,
                        section_alignment, file_alignment, subsystem, is_dll,
                        export_plan.export_directory_rva,
                        export_plan.export_directory_size,
                        exception_rva, exception_size,
                        import_plan.import_directory_rva,
                        import_plan.import_directory_size, import_plan.iat_rva,
                        import_plan.iat_size, error_message_out) ||
      !pe_write_payloads(file, layouts, layout_count, error_message_out)) {
    goto cleanup;
  }

  {
    char ownership_error[256];
    long written = 0;
    unsigned char *image = NULL;

    if (fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0 ||
        (written = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
      mettle_set_error(error_message_out,
                   "Failed to re-read the emitted PE image for verification");
      goto cleanup;
    }
    image = (unsigned char *)malloc((size_t)written ? (size_t)written : 1u);
    if (!image ||
        fread(image, 1u, (size_t)written, file) != (size_t)written) {
      free(image);
      mettle_set_error(error_message_out,
                   "Failed to load the emitted PE image for verification");
      goto cleanup;
    }
    ownership_error[0] = '\0';
    ok = mettle_verify_owned_image(image, (size_t)written, ownership_error,
                                   sizeof(ownership_error));
    free(image);
    if (!ok) {
      mettle_set_error(error_message_out, "%s",
                   ownership_error[0] ? ownership_error
                                      : "emitted image is not owned");
      goto cleanup;
    }
  }

cleanup:
  if (file) {
    fclose(file);
    if (!ok) {
      /* Half an image, or one that failed the ownership check: leaving it
       * behind would let a later step mistake it for a finished executable. */
      remove(output_path);
    }
  }
  pe_import_plan_destroy(&import_plan);
  pe_export_plan_destroy(&export_plan);
  pe_destroy_import_libraries(libraries, library_count);
  return ok;
}
