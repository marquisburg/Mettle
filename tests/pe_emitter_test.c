#include "binary_emitter.h"
#include "linker/pe_emitter.h"
#include "linker/symbol_resolve.h"
#include "test_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
  char name[9];
  uint32_t virtual_size;
  uint32_t virtual_address;
  uint32_t raw_size;
  uint32_t raw_offset;
  uint32_t characteristics;
} ParsedSection;

static uint16_t read_u16(const unsigned char *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void write_u16(unsigned char *bytes, uint16_t value) {
  bytes[0] = (unsigned char)(value & 0xFFu);
  bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void write_u32(unsigned char *bytes, uint32_t value) {
  bytes[0] = (unsigned char)(value & 0xFFu);
  bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
  bytes[2] = (unsigned char)((value >> 16) & 0xFFu);
  bytes[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static int contains_bytes(const unsigned char *haystack, size_t haystack_size,
                          const char *needle) {
  size_t needle_size = 0u;
  size_t i = 0u;

  if (!haystack || !needle) {
    return 0;
  }

  needle_size = strlen(needle);
  if (needle_size == 0u || needle_size > haystack_size) {
    return 0;
  }

  for (i = 0u; i + needle_size <= haystack_size; i++) {
    if (memcmp(haystack + i, needle, needle_size) == 0) {
      return 1;
    }
  }

  return 0;
}

static int contains_sequence(const unsigned char *haystack, size_t haystack_size,
                             const unsigned char *needle, size_t needle_size) {
  size_t i = 0u;

  if (!haystack || !needle || needle_size == 0u || needle_size > haystack_size) {
    return 0;
  }

  for (i = 0u; i + needle_size <= haystack_size; i++) {
    if (memcmp(haystack + i, needle, needle_size) == 0) {
      return 1;
    }
  }

  return 0;
}

static int read_file(const char *path, unsigned char **data_out, size_t *size_out) {
  FILE *file = NULL;
  unsigned char *data = NULL;
  long size = 0;

  if (data_out) {
    *data_out = NULL;
  }
  if (size_out) {
    *size_out = 0u;
  }
  if (!path || !data_out || !size_out) {
    return 0;
  }

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }

  size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = malloc((size_t)size);
  if (!data) {
    fclose(file);
    return 0;
  }

  if (size > 0 && fread(data, 1u, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return 0;
  }

  fclose(file);
  *data_out = data;
  *size_out = (size_t)size;
  return 1;
}

static int write_archive_member(FILE *file, const char *member_name,
                                const unsigned char *member_data,
                                size_t member_size) {
  unsigned char header[60];
  char field[32];

  if (!file || !member_name || (!member_data && member_size != 0u)) {
    return 0;
  }

  memset(header, ' ', sizeof(header));

  snprintf(field, sizeof(field), "%-16.16s", member_name);
  memcpy(header, field, 16u);
  snprintf(field, sizeof(field), "%-12d", 0);
  memcpy(header + 16u, field, 12u);
  snprintf(field, sizeof(field), "%-6d", 0);
  memcpy(header + 28u, field, 6u);
  memcpy(header + 34u, field, 6u);
  snprintf(field, sizeof(field), "%-8o", 0644);
  memcpy(header + 40u, field, 8u);
  snprintf(field, sizeof(field), "%-10zu", member_size);
  memcpy(header + 48u, field, 10u);
  header[58] = '`';
  header[59] = '\n';

  if (fwrite(header, 1u, sizeof(header), file) != sizeof(header)) {
    return 0;
  }
  if (member_size != 0u &&
      fwrite(member_data, 1u, member_size, file) != member_size) {
    return 0;
  }
  if ((member_size & 1u) != 0u && fputc('\n', file) == EOF) {
    return 0;
  }

  return 1;
}

static int write_short_import_member(FILE *file, const char *member_name,
                                     const char *symbol_name,
                                     const char *dll_name,
                                     uint16_t ordinal_hint) {
  unsigned char *member_data = NULL;
  size_t symbol_length = 0u;
  size_t dll_length = 0u;
  size_t size_of_data = 0u;
  size_t member_size = 0u;
  int ok = 0;

  if (!file || !member_name || !symbol_name || !dll_name) {
    return 0;
  }

  symbol_length = strlen(symbol_name);
  dll_length = strlen(dll_name);
  size_of_data = symbol_length + 1u + dll_length + 1u;
  member_size = 20u + size_of_data;
  member_data = calloc(1u, member_size);
  if (!member_data) {
    return 0;
  }

  write_u16(member_data, 0u);
  write_u16(member_data + 2u, 0xFFFFu);
  write_u16(member_data + 4u, 0u);
  write_u16(member_data + 6u, PE_MACHINE_AMD64);
  write_u32(member_data + 8u, 0u);
  write_u32(member_data + 12u, (uint32_t)size_of_data);
  write_u16(member_data + 16u, ordinal_hint);
  write_u16(member_data + 18u, 0x0004u);
  memcpy(member_data + 20u, symbol_name, symbol_length + 1u);
  memcpy(member_data + 20u + symbol_length + 1u, dll_name, dll_length + 1u);

  ok = write_archive_member(file, member_name, member_data, member_size);
  free(member_data);
  return ok;
}

static int write_object(BinaryEmitter *emitter, const char *path) {
  if (!binary_emitter_write_object_file(emitter, path)) {
    fprintf(stderr, "Emitter write failed for %s: %s\n", path,
            binary_emitter_get_error(emitter)
                ? binary_emitter_get_error(emitter)
                : "unknown error");
    return 0;
  }
  return 1;
}

static int create_minimal_program_object(const char *path) {
  BinaryEmitter *emitter = NULL;
  size_t text = 0u;
  size_t rdata = 0u;
  size_t data = 0u;
  size_t bss = 0u;
  static const unsigned char code[] = {0x31u, 0xC0u, 0xC3u};
  static const unsigned char rodata[] = {'P', 'E', ' ', 't', 'e', 's', 't', 0u};
  static const unsigned char init_data[] = {0x44u, 0x33u, 0x22u, 0x11u};
  int ok = 0;

  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  if (!emitter) {
    return 0;
  }

  text = binary_emitter_get_or_create_section(emitter, ".text", BINARY_SECTION_TEXT,
                                              0, 16u);
  rdata = binary_emitter_get_or_create_section(emitter, ".rdata",
                                               BINARY_SECTION_RDATA, 0, 8u);
  data = binary_emitter_get_or_create_section(emitter, ".data", BINARY_SECTION_DATA,
                                              0, 4u);
  bss = binary_emitter_get_or_create_section(emitter, ".bss", BINARY_SECTION_BSS,
                                             0, 16u);
  if (text == (size_t)-1 || rdata == (size_t)-1 || data == (size_t)-1 ||
      bss == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, text, code, sizeof(code), NULL) ||
      !binary_emitter_append_bytes(emitter, rdata, rodata, sizeof(rodata), NULL) ||
      !binary_emitter_append_bytes(emitter, data, init_data, sizeof(init_data), NULL) ||
      !binary_emitter_set_section_virtual_size(emitter, bss, 16u) ||
      !binary_emitter_define_symbol(emitter, TEST_ENTRY_SYMBOL, BINARY_SYMBOL_GLOBAL,
                                    text, 0u, sizeof(code)) ||
      !binary_emitter_define_symbol(emitter, "test_message", BINARY_SYMBOL_LOCAL,
                                    rdata, 0u, sizeof(rodata)) ||
      !binary_emitter_define_symbol(emitter, "initialized_value",
                                    BINARY_SYMBOL_LOCAL, data, 0u,
                                    sizeof(init_data)) ||
      !binary_emitter_define_symbol(emitter, "scratch_space", BINARY_SYMBOL_LOCAL,
                                    bss, 0u, 16u)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_import_program_object(const char *path) {
  BinaryEmitter *emitter = NULL;
  size_t text = 0u;
  static const unsigned char code[] = {
      0x48u, 0x83u, 0xECu, 0x28u, 0x48u, 0x8Bu, 0x05u, 0x00u, 0x00u, 0x00u,
      0x00u, 0xFFu, 0xD0u, 0x31u, 0xC9u, 0xE8u, 0x00u, 0x00u, 0x00u, 0x00u};
  int ok = 0;

  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  if (!emitter) {
    return 0;
  }

  text = binary_emitter_get_or_create_section(emitter, ".text", BINARY_SECTION_TEXT,
                                              0, 16u);
  if (text == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, text, code, sizeof(code), NULL) ||
      !binary_emitter_define_symbol(emitter, TEST_ENTRY_SYMBOL, BINARY_SYMBOL_GLOBAL,
                                    text, 0u, sizeof(code)) ||
      !binary_emitter_declare_external(emitter, "__imp_GetCurrentProcessId") ||
      !binary_emitter_declare_external(emitter, "ExitProcess") ||
      !binary_emitter_add_relocation(emitter, text, 7u, BINARY_RELOCATION_REL32,
                                     "__imp_GetCurrentProcessId", 0) ||
      !binary_emitter_add_relocation(emitter, text, 16u, BINARY_RELOCATION_REL32,
                                     "ExitProcess", 0)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_import_library(const char *path) {  FILE *file = NULL;
  int ok = 0;

  if (!path) {
    return 0;
  }

  file = fopen(path, "wb");
  if (!file) {
    return 0;
  }

  if (fwrite("!<arch>\n", 1u, 8u, file) != 8u ||
      !write_short_import_member(file, "exitproc/", "ExitProcess", "kernel32.dll",
                                 0u) ||
      !write_short_import_member(file, "getpid/", "GetCurrentProcessId",
                                 "kernel32.dll", 1u)) {
    goto cleanup;
  }

  ok = 1;

cleanup:
  fclose(file);
  return ok;
}

static int parse_section(const unsigned char *bytes, size_t file_size,
                         size_t section_offset, ParsedSection *section_out) {
  if (!bytes || !section_out || section_offset + 40u > file_size) {
    return 0;
  }

  memset(section_out, 0, sizeof(*section_out));
  memcpy(section_out->name, bytes + section_offset, 8u);
  section_out->name[8] = '\0';
  section_out->virtual_size = test_read_u32(bytes + section_offset + 8u);
  section_out->virtual_address = test_read_u32(bytes + section_offset + 12u);
  section_out->raw_size = test_read_u32(bytes + section_offset + 16u);
  section_out->raw_offset = test_read_u32(bytes + section_offset + 20u);
  section_out->characteristics = test_read_u32(bytes + section_offset + 36u);
  return 1;
}

static int rva_to_file_offset(uint32_t rva, const ParsedSection *sections,
                              size_t section_count, size_t file_size,
                              size_t *offset_out) {
  size_t i = 0u;

  if (offset_out) {
    *offset_out = 0u;
  }
  if (!sections || !offset_out) {
    return 0;
  }

  for (i = 0u; i < section_count; i++) {
    uint32_t span = sections[i].virtual_size;

    if (sections[i].raw_size > span) {
      span = sections[i].raw_size;
    }
    if (rva < sections[i].virtual_address ||
        rva >= sections[i].virtual_address + span ||
        sections[i].raw_offset == 0u) {
      continue;
    }

    *offset_out =
        (size_t)sections[i].raw_offset + (size_t)(rva - sections[i].virtual_address);
    if (*offset_out > file_size) {
      return 0;
    }
    return 1;
  }

  return 0;
}

static int verify_pe_image(const char *exe_path) {
  unsigned char *data = NULL;
  size_t size = 0u;
  uint32_t pe_offset = 0u;
  uint16_t section_count = 0u;
  uint16_t optional_header_size = 0u;
  uint16_t subsystem = 0u;
  uint32_t entry_rva = 0u;
  uint32_t base_of_code = 0u;
  uint64_t image_base = 0u;
  uint32_t section_alignment = 0u;
  uint32_t file_alignment = 0u;
  uint32_t size_of_headers = 0u;
  uint32_t size_of_image = 0u;
  uint32_t number_of_directories = 0u;
  ParsedSection text = {0};
  ParsedSection rdata = {0};
  ParsedSection sdata = {0};
  ParsedSection bss = {0};
  int result = 1;

  if (!read_file(exe_path, &data, &size)) {
    return report_failure("Failed to read emitted PE image", exe_path);
  }
  if (size < 0x200u) {
    result = report_failure("Emitted PE image is too small", exe_path);
    goto cleanup;
  }
  if (data[0] != 'M' || data[1] != 'Z') {
    result = report_failure("Emitted image is missing the DOS signature", exe_path);
    goto cleanup;
  }
  if (!contains_bytes(data, size, "This program cannot be run in DOS mode")) {
    result = report_failure("DOS stub message was not written", exe_path);
    goto cleanup;
  }

  pe_offset = test_read_u32(data + 0x3Cu);
  if (pe_offset + 24u > size || memcmp(data + pe_offset, "PE\0\0", 4u) != 0) {
    result = report_failure("PE signature is missing or out of range", exe_path);
    goto cleanup;
  }

  if (read_u16(data + pe_offset + 4u) != PE_MACHINE_AMD64) {
    result = report_failure("PE machine type is not AMD64", exe_path);
    goto cleanup;
  }

  section_count = read_u16(data + pe_offset + 6u);
  optional_header_size = read_u16(data + pe_offset + 20u);
  if (section_count != 4u || optional_header_size != 240u) {
    result = report_failure("PE headers used an unexpected section/header size",
                            exe_path);
    goto cleanup;
  }

  if ((read_u16(data + pe_offset + 22u) & 0x0002u) == 0u) {
    result = report_failure("PE headers did not mark the image executable",
                            exe_path);
    goto cleanup;
  }

  if (read_u16(data + pe_offset + 24u) != PE_OPTIONAL_MAGIC_PE32PLUS) {
    result = report_failure("PE optional header is not PE32+", exe_path);
    goto cleanup;
  }

  entry_rva = test_read_u32(data + pe_offset + 40u);
  base_of_code = test_read_u32(data + pe_offset + 44u);
  image_base = test_read_u64(data + pe_offset + 48u);
  section_alignment = test_read_u32(data + pe_offset + 56u);
  file_alignment = test_read_u32(data + pe_offset + 60u);
  size_of_image = test_read_u32(data + pe_offset + 80u);
  size_of_headers = test_read_u32(data + pe_offset + 84u);
  subsystem = read_u16(data + pe_offset + 92u);
  number_of_directories = test_read_u32(data + pe_offset + 132u);
  if (image_base != 0x140000000ull || section_alignment != 0x1000u ||
      file_alignment != 0x200u || subsystem != 3u ||
      number_of_directories != 16u) {
    result = report_failure("PE optional header fields were not emitted as expected",
                            exe_path);
    goto cleanup;
  }
  if (test_read_u32(data + pe_offset + 136u) != 0u ||
      test_read_u32(data + pe_offset + 140u) != 0u ||
      test_read_u32(data + pe_offset + 144u) != 0u ||
      test_read_u32(data + pe_offset + 148u) != 0u) {
    result = report_failure("PE data directories were expected to be empty",
                            exe_path);
    goto cleanup;
  }
  if (size_of_headers != 0x400u || size_of_image != 0x5000u) {
    result = report_failure("PE header/image sizing was not aligned as expected",
                            exe_path);
    goto cleanup;
  }

  if (!parse_section(data, size, pe_offset + 24u + optional_header_size, &text) ||
      !parse_section(data, size,
                     pe_offset + 24u + optional_header_size + 40u, &rdata) ||
      !parse_section(data, size,
                     pe_offset + 24u + optional_header_size + 80u, &sdata) ||
      !parse_section(data, size,
                     pe_offset + 24u + optional_header_size + 120u, &bss)) {
    result = report_failure("Failed to parse PE section headers", exe_path);
    goto cleanup;
  }

  if (strcmp(text.name, ".text") != 0 || strcmp(rdata.name, ".rdata") != 0 ||
      strcmp(sdata.name, ".data") != 0 || strcmp(bss.name, ".bss") != 0) {
    result = report_failure("PE section names were not emitted in linker order",
                            exe_path);
    goto cleanup;
  }
  if (text.virtual_address != 0x1000u || rdata.virtual_address != 0x2000u ||
      sdata.virtual_address != 0x3000u || bss.virtual_address != 0x4000u) {
    result = report_failure("PE section RVAs were not aligned to 0x1000 pages",
                            exe_path);
    goto cleanup;
  }
  if (entry_rva != text.virtual_address || base_of_code != text.virtual_address) {
    result = report_failure("PE entry point was not assigned to mainCRTStartup",
                            exe_path);
    goto cleanup;
  }
  if (text.virtual_size != 3u || text.raw_size != 0x200u ||
      text.raw_offset != 0x400u) {
    result = report_failure("PE .text layout is incorrect", exe_path);
    goto cleanup;
  }
  if (rdata.virtual_size != 8u || rdata.raw_size != 0x200u ||
      rdata.raw_offset != 0x600u) {
    result = report_failure("PE .rdata layout is incorrect", exe_path);
    goto cleanup;
  }
  if (sdata.virtual_size != 4u || sdata.raw_size != 0x200u ||
      sdata.raw_offset != 0x800u) {
    result = report_failure("PE .data layout is incorrect", exe_path);
    goto cleanup;
  }
  if (bss.virtual_size != 16u || bss.raw_size != 0u || bss.raw_offset != 0u) {
    result = report_failure("PE .bss layout is incorrect", exe_path);
    goto cleanup;
  }
  if ((text.characteristics & 0x60000020u) != 0x60000020u ||
      (rdata.characteristics & 0x40000040u) != 0x40000040u ||
      (sdata.characteristics & 0xC0000040u) != 0xC0000040u ||
      (bss.characteristics & 0xC0000080u) != 0xC0000080u) {
    result = report_failure("PE section characteristics were not assigned correctly",
                            exe_path);
    goto cleanup;
  }
  if (text.raw_offset + 3u > size || rdata.raw_offset + 8u > size ||
      sdata.raw_offset + 4u > size) {
    result = report_failure("PE section payloads were truncated", exe_path);
    goto cleanup;
  }
  if (data[text.raw_offset] != 0x31u || data[text.raw_offset + 1u] != 0xC0u ||
      data[text.raw_offset + 2u] != 0xC3u) {
    result = report_failure("PE .text payload bytes were not preserved", exe_path);
    goto cleanup;
  }
  if (memcmp(data + rdata.raw_offset, "PE test", 7u) != 0 ||
      data[sdata.raw_offset] != 0x44u || data[sdata.raw_offset + 1u] != 0x33u ||
      data[sdata.raw_offset + 2u] != 0x22u ||
      data[sdata.raw_offset + 3u] != 0x11u) {
    result = report_failure("PE data payload bytes were not preserved", exe_path);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(data);
  return result;
}

static int verify_import_pe_image(const char *exe_path) {
  unsigned char *data = NULL;
  size_t size = 0u;
  uint32_t pe_offset = 0u;
  uint16_t section_count = 0u;
  uint16_t optional_header_size = 0u;
  uint32_t import_rva = 0u;
  uint32_t import_size = 0u;
  uint32_t iat_rva = 0u;
  uint32_t iat_size = 0u;
  ParsedSection sections[8];
  ParsedSection *text = NULL;
  ParsedSection *rdata = NULL;
  size_t import_offset = 0u;
  size_t ilt_offset = 0u;
  size_t iat_offset = 0u;
  size_t dll_name_offset = 0u;
  size_t hint_name_offset = 0u;
  size_t i = 0u;
  int saw_exit_process = 0;
  int saw_get_current_process_id = 0;
  static const unsigned char jump_stub_prefix[] = {0xFFu, 0x25u};
  int result = 1;

  memset(sections, 0, sizeof(sections));

  if (!read_file(exe_path, &data, &size)) {
    return report_failure("Failed to read imported PE image", exe_path);
  }
  if (size < 0x200u || data[0] != 'M' || data[1] != 'Z') {
    result = report_failure("Imported PE image is not a valid DOS executable",
                            exe_path);
    goto cleanup;
  }

  pe_offset = test_read_u32(data + 0x3Cu);
  if (pe_offset + 24u > size || memcmp(data + pe_offset, "PE\0\0", 4u) != 0) {
    result = report_failure("Imported PE image is missing the PE signature",
                            exe_path);
    goto cleanup;
  }

  section_count = read_u16(data + pe_offset + 6u);
  optional_header_size = read_u16(data + pe_offset + 20u);
  if (section_count != 2u || optional_header_size != 240u ||
      read_u16(data + pe_offset + 24u) != PE_OPTIONAL_MAGIC_PE32PLUS) {
    result = report_failure("Imported PE image headers were not emitted as expected",
                            exe_path);
    goto cleanup;
  }

  import_rva = test_read_u32(data + pe_offset + 144u);
  import_size = test_read_u32(data + pe_offset + 148u);
  iat_rva = test_read_u32(data + pe_offset + 232u);
  iat_size = test_read_u32(data + pe_offset + 236u);
  if (import_rva == 0u || import_size != 40u || iat_rva == 0u || iat_size != 24u) {
    result = report_failure("Imported PE data directories were not emitted correctly",
                            exe_path);
    goto cleanup;
  }
  if (!contains_bytes(data, size, "kernel32.dll") ||
      !contains_bytes(data, size, "ExitProcess") ||
      !contains_bytes(data, size, "GetCurrentProcessId")) {
    result = report_failure("Imported PE image is missing import strings", exe_path);
    goto cleanup;
  }

  for (i = 0u; i < section_count; i++) {
    if (!parse_section(data, size,
                       pe_offset + 24u + optional_header_size + (i * 40u),
                       &sections[i])) {
      result = report_failure("Failed to parse imported PE section headers",
                              exe_path);
      goto cleanup;
    }
    if (strcmp(sections[i].name, ".text") == 0) {
      text = &sections[i];
    } else if (strcmp(sections[i].name, ".rdata") == 0) {
      rdata = &sections[i];
    }
  }

  if (!text || !rdata || text->virtual_address != 0x1000u ||
      rdata->virtual_address != 0x2000u) {
    result = report_failure("Imported PE sections were not laid out correctly",
                            exe_path);
    goto cleanup;
  }
  if (!contains_sequence(data + text->raw_offset, text->raw_size, jump_stub_prefix,
                         sizeof(jump_stub_prefix))) {
    result = report_failure("Imported PE .text section is missing the import thunk",
                            exe_path);
    goto cleanup;
  }

  if (!rva_to_file_offset(import_rva, sections, section_count, size,
                          &import_offset) ||
      !rva_to_file_offset(iat_rva, sections, section_count, size, &iat_offset)) {
    result = report_failure("Imported PE directories do not map into a file section",
                            exe_path);
    goto cleanup;
  }

  if (test_read_u32(data + import_offset) == 0u ||
      test_read_u32(data + import_offset + 12u) == 0u ||
      test_read_u32(data + import_offset + 16u) != iat_rva ||
      memcmp(data + import_offset + 20u,
             "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20u) != 0) {
    result = report_failure("Imported PE descriptor table is malformed", exe_path);
    goto cleanup;
  }

  if (!rva_to_file_offset(test_read_u32(data + import_offset + 12u), sections,
                          section_count, size, &dll_name_offset) ||
      strcmp((const char *)(data + dll_name_offset), "kernel32.dll") != 0) {
    result = report_failure("Imported PE descriptor did not point at kernel32.dll",
                            exe_path);
    goto cleanup;
  }

  if (!rva_to_file_offset(test_read_u32(data + import_offset), sections, section_count,
                          size, &ilt_offset)) {
    result = report_failure("Imported PE ILT did not map into the file", exe_path);
    goto cleanup;
  }

  for (i = 0u; i < 2u; i++) {
    uint64_t ilt_entry = test_read_u64(data + ilt_offset + (i * 8u));
    uint64_t iat_entry = test_read_u64(data + iat_offset + (i * 8u));
    const char *import_name = NULL;

    if (ilt_entry == 0u || ilt_entry != iat_entry ||
        (ilt_entry & 0x8000000000000000ull) != 0u ||
        !rva_to_file_offset((uint32_t)ilt_entry, sections, section_count, size,
                            &hint_name_offset) ||
        hint_name_offset + 2u >= size) {
      result = report_failure("Imported PE thunk tables were not emitted correctly",
                              exe_path);
      goto cleanup;
    }

    import_name = (const char *)(data + hint_name_offset + 2u);
    if (strcmp(import_name, "ExitProcess") == 0) {
      saw_exit_process = 1;
    } else if (strcmp(import_name, "GetCurrentProcessId") == 0) {
      saw_get_current_process_id = 1;
    }
  }

  if (test_read_u64(data + ilt_offset + 16u) != 0u ||
      test_read_u64(data + iat_offset + 16u) != 0u ||
      !saw_exit_process || !saw_get_current_process_id) {
    result = report_failure("Imported PE thunk names were not emitted as expected",
                            exe_path);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(data);
  return result;
}

#ifdef _WIN32
static int run_executable_and_expect_zero(const char *path) {
  STARTUPINFOA startup_info;
  PROCESS_INFORMATION process_info;
  DWORD exit_code = 1u;
  char command_line[MAX_PATH * 2u];

  memset(&startup_info, 0, sizeof(startup_info));
  memset(&process_info, 0, sizeof(process_info));
  startup_info.cb = sizeof(startup_info);

  if (!path || strlen(path) + 3u >= sizeof(command_line)) {
    return report_failure("Executable path is too long to launch", path);
  }

  snprintf(command_line, sizeof(command_line), "\"%s\"", path);
  if (!CreateProcessA(NULL, command_line, NULL, NULL, FALSE, 0, NULL, NULL,
                      &startup_info, &process_info)) {
    return report_failure("CreateProcessA failed for emitted PE image", path);
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);
  if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return report_failure("Failed to query emitted PE exit code", path);
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);

  if (exit_code != 0u) {
    return report_failure("Emitted PE image exited with a non-zero code", path);
  }

  return 0;
}
#else
static int run_executable_and_expect_zero(const char *path) {
  (void)path;
  return 0;
}
#endif

static int expect_pe_emission(const char *object_path, const char *exe_path) {
  const char *paths[1] = {object_path};
  LinkResolutionOptions options = {TEST_ENTRY_SYMBOL, 16u, 0};
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  int result = 1;

  if (!link_resolution_build(paths, 1u, &options, &resolution, &error_message)) {
    result = report_failure("PE emission resolution failed", error_message);
    goto cleanup;
  }

  if (!pe_emit_executable(resolution, exe_path, NULL, &error_message)) {
    result = report_failure("PE emission failed", error_message);
    goto cleanup;
  }

  if (verify_pe_image(exe_path) != 0 ||
      run_executable_and_expect_zero(exe_path) != 0) {
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

static int expect_import_pe_emission(const char *object_path,
                                     const char *import_library_path,
                                     const char *exe_path) {
  const char *object_paths[1] = {object_path};
  const char *import_library_paths[1] = {import_library_path};
  LinkResolutionOptions resolution_options = {TEST_ENTRY_SYMBOL, 16u, 1};
  PeEmissionOptions emission_options;
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  int result = 1;

  memset(&emission_options, 0, sizeof(emission_options));
  emission_options.import_library_paths = import_library_paths;
  emission_options.import_library_count = 1u;

  if (!link_resolution_build(object_paths, 1u, &resolution_options, &resolution,
                             &error_message)) {
    result =
        report_failure("Import-backed PE emission resolution failed", error_message);
    goto cleanup;
  }

  if (!pe_emit_executable(resolution, exe_path, &emission_options,
                          &error_message)) {
    result = report_failure("Import-backed PE emission failed", error_message);
    goto cleanup;
  }

  if (verify_import_pe_image(exe_path) != 0 ||
      run_executable_and_expect_zero(exe_path) != 0) {
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

static int expect_dll_probe_pe_emission(const char *object_path,
                                        const char *dll_name,
                                        const char *exe_path) {
  const char *object_paths[1] = {object_path};
  const char *import_dll_names[1] = {dll_name};
  LinkResolutionOptions resolution_options = {TEST_ENTRY_SYMBOL, 16u, 1};
  PeEmissionOptions emission_options;
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  int result = 1;

  memset(&emission_options, 0, sizeof(emission_options));
  emission_options.import_dll_names = import_dll_names;
  emission_options.import_dll_count = 1u;

  if (!link_resolution_build(object_paths, 1u, &resolution_options, &resolution,
                             &error_message)) {
    result =
        report_failure("DLL-probed PE emission resolution failed", error_message);
    goto cleanup;
  }

  if (!pe_emit_executable(resolution, exe_path, &emission_options,
                          &error_message)) {
    result = report_failure("DLL-probed PE emission failed", error_message);
    goto cleanup;
  }

  if (verify_import_pe_image(exe_path) != 0 ||
      run_executable_and_expect_zero(exe_path) != 0) {
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

static int create_dll_program_object(const char *path) {
  BinaryEmitter *emitter = NULL;
  size_t text = 0u;
  size_t data = 0u;
  static const unsigned char code[] = {0x31u, 0xC0u, 0xC3u};
  static const unsigned char init_data[] = {0x29u, 0x00u, 0x00u, 0x00u};
  int ok = 0;

  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  if (!emitter) {
    return 0;
  }

  text = binary_emitter_get_or_create_section(emitter, ".text", BINARY_SECTION_TEXT,
                                              0, 16u);
  data = binary_emitter_get_or_create_section(emitter, ".data", BINARY_SECTION_DATA,
                                              0, 4u);
  if (text == (size_t)-1 || data == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, text, code, sizeof(code), NULL) ||
      !binary_emitter_append_bytes(emitter, data, init_data, sizeof(init_data),
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, "dll_add", BINARY_SYMBOL_GLOBAL,
                                    text, 0u, sizeof(code)) ||
      !binary_emitter_define_symbol(emitter, "dll_value", BINARY_SYMBOL_GLOBAL,
                                    data, 0u, sizeof(init_data))) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int verify_dll_pe_image(const char *dll_path) {
  unsigned char *data = NULL;
  size_t size = 0u;
  uint32_t pe_offset = 0u;
  uint16_t characteristics = 0u;
  uint32_t entry_rva = 0u;
  uint32_t export_rva = 0u;
  uint32_t export_size = 0u;
  int result = 1;

  if (!read_file(dll_path, &data, &size)) {
    return report_failure("Failed to read emitted DLL image", dll_path);
  }
  if (size < 0x200u || data[0] != 'M' || data[1] != 'Z') {
    result = report_failure("Emitted DLL is not a valid DOS executable", dll_path);
    goto cleanup;
  }

  pe_offset = test_read_u32(data + 0x3Cu);
  if (pe_offset + 24u > size || memcmp(data + pe_offset, "PE\0\0", 4u) != 0) {
    result = report_failure("Emitted DLL is missing the PE signature", dll_path);
    goto cleanup;
  }

  characteristics = read_u16(data + pe_offset + 22u);
  if ((characteristics & 0x2000u) == 0u) {
    result = report_failure("PE headers did not mark the image as a DLL", dll_path);
    goto cleanup;
  }
  if ((characteristics & 0x0002u) == 0u) {
    result = report_failure("PE headers did not mark the DLL executable", dll_path);
    goto cleanup;
  }

  entry_rva = test_read_u32(data + pe_offset + 40u);
  if (entry_rva != 0u) {
    result = report_failure("DLL image should have no entry point", dll_path);
    goto cleanup;
  }

  export_rva = test_read_u32(data + pe_offset + 136u);
  export_size = test_read_u32(data + pe_offset + 140u);
  if (export_rva == 0u || export_size != 40u) {
    result = report_failure("DLL export directory was not emitted", dll_path);
    goto cleanup;
  }
  if (!contains_bytes(data, size, "dll_add") ||
      !contains_bytes(data, size, "dll_value")) {
    result = report_failure("DLL image is missing export names", dll_path);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(data);
  return result;
}

static int expect_dll_pe_emission(const char *object_path, const char *dll_path) {
  const char *object_paths[1] = {object_path};
  LinkResolutionOptions resolution_options = {NULL, 16u, 1};
  PeEmissionOptions emission_options;
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  int result = 1;

  memset(&emission_options, 0, sizeof(emission_options));
  emission_options.produce_shared_library = 1;

  if (!link_resolution_build(object_paths, 1u, &resolution_options, &resolution,
                             &error_message)) {
    result =
        report_failure("DLL PE emission resolution failed", error_message);
    goto cleanup;
  }

  if (!pe_emit_executable(resolution, dll_path, &emission_options,
                          &error_message)) {
    result = report_failure("DLL PE emission failed", error_message);
    goto cleanup;
  }

  if (verify_dll_pe_image(dll_path) != 0) {
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

int main(int argc, char **argv) {
  char *object_path = NULL;
  char *exe_path = NULL;
  char *import_object_path = NULL;
  char *import_library_path = NULL;
  char *import_exe_path = NULL;
  char *dll_probe_exe_path = NULL;
  char *dll_object_path = NULL;
  char *dll_path = NULL;
  int result = 1;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <temp-dir>\n", argv[0]);
    return 1;
  }

  object_path = join_path(argv[1], "pe_emitter_input.obj");
  exe_path = join_path(argv[1], "pe_emitter_output.exe");
  import_object_path = join_path(argv[1], "pe_import_input.obj");
  import_library_path = join_path(argv[1], "pe_import_kernel32.lib");
  import_exe_path = join_path(argv[1], "pe_import_output.exe");
  dll_probe_exe_path = join_path(argv[1], "pe_import_probe_output.exe");
  dll_object_path = join_path(argv[1], "pe_dll_input.obj");
  dll_path = join_path(argv[1], "pe_dll_output.dll");
  if (!object_path || !exe_path || !import_object_path || !import_library_path ||
      !import_exe_path || !dll_probe_exe_path || !dll_object_path || !dll_path) {
    result = report_failure("Failed to allocate PE emitter test paths", NULL);
    goto cleanup;
  }

  if (!create_minimal_program_object(object_path)) {
    result = report_failure("Failed to create PE emitter input object", object_path);
    goto cleanup;
  }

  if (expect_pe_emission(object_path, exe_path) != 0) {
    goto cleanup;
  }

  if (!create_import_library(import_library_path)) {
    result = report_failure("Failed to create import library fixture",
                            import_library_path);
    goto cleanup;
  }
  if (!create_import_program_object(import_object_path)) {
    result = report_failure("Failed to create import PE input object",
                            import_object_path);
    goto cleanup;
  }
  if (expect_import_pe_emission(import_object_path, import_library_path,
                                import_exe_path) != 0) {
    goto cleanup;
  }
  if (expect_dll_probe_pe_emission(import_object_path, "kernel32.dll",
                                   dll_probe_exe_path) != 0) {
    goto cleanup;
  }
  if (!create_dll_program_object(dll_object_path)) {
    result = report_failure("Failed to create DLL PE input object",
                            dll_object_path);
    goto cleanup;
  }
  if (expect_dll_pe_emission(dll_object_path, dll_path) != 0) {
    goto cleanup;
  }

  result = 0;

cleanup:
  free(object_path);
  free(exe_path);
  free(import_object_path);
  free(import_library_path);
  free(import_exe_path);
  free(dll_probe_exe_path);
  free(dll_object_path);
  free(dll_path);
  return result;
}
