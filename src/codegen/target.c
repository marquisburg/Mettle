#include "target.h"
#include "binary/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  MtlcTargetArch arch;
  MtlcTargetOs os;
  int code_bits;
  BinaryTargetFormat format;
  int freestanding;
} MtlcTargetEntry;

static const MtlcTargetEntry TARGET_TABLE[] = {
    {"x86_64-windows", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_WINDOWS, 64,
     BINARY_TARGET_FORMAT_COFF_WIN64, 0},
    {"x86_64-pc-windows", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_WINDOWS, 64,
     BINARY_TARGET_FORMAT_COFF_WIN64, 0},
    {"x86_64-linux", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_LINUX, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 0},
    {"x86_64-unknown-linux", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_LINUX, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 0},
    {"x86_64-none", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_NONE, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"x86_64-freestanding", MTLC_TARGET_ARCH_X86_64, MTLC_TARGET_OS_NONE, 64,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"aarch64-linux", MTLC_TARGET_ARCH_AARCH64, MTLC_TARGET_OS_LINUX, 64,
     BINARY_TARGET_FORMAT_ELF_ARM64, 0},
    {"aarch64-unknown-linux", MTLC_TARGET_ARCH_AARCH64, MTLC_TARGET_OS_LINUX,
     64, BINARY_TARGET_FORMAT_ELF_ARM64, 0},
    {"aarch64-none", MTLC_TARGET_ARCH_AARCH64, MTLC_TARGET_OS_NONE, 64,
     BINARY_TARGET_FORMAT_ELF_ARM64, 1},
    {"i386-none", MTLC_TARGET_ARCH_X86_32, MTLC_TARGET_OS_NONE, 32,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"i686-none", MTLC_TARGET_ARCH_X86_32, MTLC_TARGET_OS_NONE, 32,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"i8086-none", MTLC_TARGET_ARCH_X86_16, MTLC_TARGET_OS_NONE, 16,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
    {"i8086", MTLC_TARGET_ARCH_X86_16, MTLC_TARGET_OS_NONE, 16,
     BINARY_TARGET_FORMAT_ELF_X64, 1},
};

static MtlcTarget g_target;
static int g_target_initialized;

static void mtlc_target_init_host(void) {
  BinaryTargetFormat format = binary_target_format_host_default();
  memset(&g_target, 0, sizeof(g_target));
  g_target.format = format;
  g_target.code_bits = 64;
  g_target.section_alignment = 0;
  switch (format) {
  case BINARY_TARGET_FORMAT_ELF_ARM64:
    g_target.arch = MTLC_TARGET_ARCH_AARCH64;
    g_target.os = MTLC_TARGET_OS_LINUX;
    snprintf(g_target.triple, sizeof(g_target.triple), "aarch64-linux");
    break;
  case BINARY_TARGET_FORMAT_ELF_X64:
    g_target.arch = MTLC_TARGET_ARCH_X86_64;
    g_target.os = MTLC_TARGET_OS_LINUX;
    snprintf(g_target.triple, sizeof(g_target.triple), "x86_64-linux");
    break;
  case BINARY_TARGET_FORMAT_COFF_WIN64:
  default:
    g_target.arch = MTLC_TARGET_ARCH_X86_64;
    g_target.os = MTLC_TARGET_OS_WINDOWS;
    snprintf(g_target.triple, sizeof(g_target.triple), "x86_64-windows");
    break;
  }
  g_target_initialized = 1;
}

const MtlcTarget *mtlc_target(void) {
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  return &g_target;
}

int mtlc_target_select(const char *triple, char *error, size_t error_size) {
  size_t i;
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  if (!triple || triple[0] == '\0') {
    snprintf(error, error_size, "--target needs a triple");
    return 0;
  }
  for (i = 0; i < sizeof(TARGET_TABLE) / sizeof(TARGET_TABLE[0]); i++) {
    const MtlcTargetEntry *entry = &TARGET_TABLE[i];
    size_t name_length = strlen(entry->name);
    if (strncmp(triple, entry->name, name_length) != 0) {
      continue;
    }
    if (triple[name_length] != '\0' && triple[name_length] != '-') {
      continue;
    }
    g_target.arch = entry->arch;
    g_target.os = entry->os;
    g_target.code_bits = entry->code_bits;
    g_target.format = entry->format;
    g_target.freestanding = entry->freestanding;
    g_target.explicit_triple = 1;
    snprintf(g_target.triple, sizeof(g_target.triple), "%s", triple);
    return 1;
  }
  snprintf(error, error_size, "unknown target `%s`; known targets are %s",
           triple, mtlc_target_triple_list());
  return 0;
}

void mtlc_target_set_image_base(uint64_t base) {
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  g_target.image_base = base;
  g_target.image_base_set = 1;
}

const char *mtlc_target_triple_list(void) {
  return "x86_64-windows, x86_64-linux, x86_64-none, aarch64-linux, "
         "aarch64-none, i386-none, i686-none, i8086-none";
}

MtlcTargetOs mtlc_target_host_os(void) {
  switch (binary_target_format_host_default()) {
  case BINARY_TARGET_FORMAT_ELF_X64:
  case BINARY_TARGET_FORMAT_ELF_ARM64:
    return MTLC_TARGET_OS_LINUX;
  case BINARY_TARGET_FORMAT_COFF_WIN64:
  default:
    return MTLC_TARGET_OS_WINDOWS;
  }
}

const char *mtlc_target_os_name(MtlcTargetOs os) {
  switch (os) {
  case MTLC_TARGET_OS_LINUX:
    return "Linux";
  case MTLC_TARGET_OS_WINDOWS:
    return "Windows";
  case MTLC_TARGET_OS_NONE:
  default:
    return "freestanding";
  }
}

int mtlc_target_is_object_capable(const MtlcTarget *target) {
  if (!target) {
    return 0;
  }
  return target->code_bits == 64;
}

static MtlcTargetDescription g_description;
static int g_description_valid;

static const char *arch_name(MtlcTargetArch arch) {
  switch (arch) {
  case MTLC_TARGET_ARCH_X86_64:
    return "x86_64";
  case MTLC_TARGET_ARCH_X86_32:
    return "i386";
  case MTLC_TARGET_ARCH_X86_16:
    return "i8086";
  case MTLC_TARGET_ARCH_AARCH64:
  default:
    return "aarch64";
  }
}

static const char *os_short_name(MtlcTargetOs os) {
  switch (os) {
  case MTLC_TARGET_OS_WINDOWS:
    return "windows";
  case MTLC_TARGET_OS_LINUX:
    return "linux";
  case MTLC_TARGET_OS_NONE:
  default:
    return "none";
  }
}

static const char *format_name(BinaryTargetFormat format) {
  switch (format) {
  case BINARY_TARGET_FORMAT_COFF_WIN64:
    return "coff";
  case BINARY_TARGET_FORMAT_ELF_ARM64:
  case BINARY_TARGET_FORMAT_ELF_X64:
  default:
    return "elf";
  }
}

static void set_names(char (*slots)[8], size_t *count, const char *const *names,
                      size_t name_count) {
  size_t i;
  *count = 0;
  for (i = 0; i < name_count && i < MTLC_TARGET_DESC_MAX_REGS; i++) {
    snprintf(slots[i], 8, "%s", names[i]);
    (*count)++;
  }
}

static void describe_entry(const MtlcTargetEntry *entry, const char *triple,
                           MtlcTargetDescription *out) {
  static const char *const win_int[] = {"rcx", "rdx", "r8", "r9"};
  static const char *const win_float[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
  static const char *const sysv_int[] = {"rdi", "rsi", "rdx",
                                         "rcx", "r8",  "r9"};
  static const char *const sysv_float[] = {"xmm0", "xmm1", "xmm2", "xmm3",
                                           "xmm4", "xmm5", "xmm6", "xmm7"};
  static const char *const arm_int[] = {"x0", "x1", "x2", "x3",
                                        "x4", "x5", "x6", "x7"};
  static const char *const arm_float[] = {"v0", "v1", "v2", "v3",
                                          "v4", "v5", "v6", "v7"};
  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "%s", triple);
  snprintf(out->arch, sizeof(out->arch), "%s", arch_name(entry->arch));
  snprintf(out->os, sizeof(out->os), "%s", os_short_name(entry->os));
  snprintf(out->format, sizeof(out->format), "%s",
           entry->code_bits == 64 ? format_name(entry->format) : "flat");
  out->pointer_bits = entry->code_bits;
  out->width_count = 0;
  if (entry->code_bits == 64) {
    out->widths[0] = 8;
    out->widths[1] = 16;
    out->widths[2] = 32;
    out->widths[3] = 64;
    out->width_count = 4;
  } else if (entry->code_bits == 32) {
    out->widths[0] = 8;
    out->widths[1] = 16;
    out->widths[2] = 32;
    out->width_count = 3;
  } else {
    out->widths[0] = 8;
    out->widths[1] = 16;
    out->width_count = 2;
  }
  if (entry->arch == MTLC_TARGET_ARCH_X86_64) {
    out->stack_alignment = 16;
    out->vector_width = 256;
    if (entry->os == MTLC_TARGET_OS_WINDOWS) {
      out->shadow_space = 32;
      out->red_zone = 0;
      set_names(out->int_args, &out->int_arg_count, win_int, 4);
      set_names(out->float_args, &out->float_arg_count, win_float, 4);
      snprintf(out->indirect_return, sizeof(out->indirect_return), "rcx");
      out->separate_classes = 0;
    } else {
      out->shadow_space = 0;
      out->red_zone = entry->os == MTLC_TARGET_OS_LINUX ? 128 : 0;
      set_names(out->int_args, &out->int_arg_count, sysv_int, 6);
      set_names(out->float_args, &out->float_arg_count, sysv_float, 8);
      snprintf(out->indirect_return, sizeof(out->indirect_return), "rdi");
      out->separate_classes = 1;
    }
  } else if (entry->arch == MTLC_TARGET_ARCH_AARCH64) {
    out->stack_alignment = 16;
    out->vector_width = 128;
    out->shadow_space = 0;
    out->red_zone = 0;
    set_names(out->int_args, &out->int_arg_count, arm_int, 8);
    set_names(out->float_args, &out->float_arg_count, arm_float, 8);
    snprintf(out->indirect_return, sizeof(out->indirect_return), "x8");
    out->separate_classes = 1;
  } else {
    out->stack_alignment = entry->code_bits == 32 ? 4 : 2;
    out->vector_width = 0;
    out->shadow_space = 0;
    out->red_zone = 0;
    out->int_arg_count = 0;
    out->float_arg_count = 0;
    out->indirect_return[0] = '\0';
    out->separate_classes = 0;
  }
}

static const MtlcTargetEntry *find_entry(const char *triple) {
  size_t i;
  if (!triple) {
    return NULL;
  }
  for (i = 0; i < sizeof(TARGET_TABLE) / sizeof(TARGET_TABLE[0]); i++) {
    const MtlcTargetEntry *entry = &TARGET_TABLE[i];
    size_t name_length = strlen(entry->name);
    if (strncmp(triple, entry->name, name_length) == 0 &&
        (triple[name_length] == '\0' || triple[name_length] == '-')) {
      return entry;
    }
  }
  return NULL;
}

int mtlc_target_description_of(const char *triple, MtlcTargetDescription *out,
                               char *error, size_t error_size) {
  const MtlcTargetEntry *entry = find_entry(triple);
  if (!out) {
    return 0;
  }
  if (!entry) {
    snprintf(error, error_size, "unknown target `%s`; known targets are %s",
             triple ? triple : "", mtlc_target_triple_list());
    return 0;
  }
  describe_entry(entry, triple, out);
  return 1;
}

static int gp_register_by_name(const char *name, BinaryGpRegister *out) {
  static const struct {
    const char *name;
    BinaryGpRegister reg;
  } table[] = {
      {"rax", BINARY_GP_RAX}, {"rcx", BINARY_GP_RCX}, {"rdx", BINARY_GP_RDX},
      {"rbx", BINARY_GP_RBX}, {"rsi", BINARY_GP_RSI}, {"rdi", BINARY_GP_RDI},
      {"r8", BINARY_GP_R8},   {"r9", BINARY_GP_R9},   {"r10", BINARY_GP_R10},
      {"r11", BINARY_GP_R11}, {"r12", BINARY_GP_R12}, {"r13", BINARY_GP_R13},
      {"r14", BINARY_GP_R14}, {"r15", BINARY_GP_R15},
  };
  size_t i;
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(table[i].name, name) == 0) {
      *out = table[i].reg;
      return 1;
    }
  }
  return 0;
}

static int xmm_register_by_name(const char *name, BinaryXmmRegister *out) {
  long value = 0;
  const char *digits;
  if (strncmp(name, "xmm", 3) != 0 || name[3] == '\0') {
    return 0;
  }
  for (digits = name + 3; *digits; digits++) {
    if (*digits < '0' || *digits > '9') {
      return 0;
    }
    value = value * 10 + (*digits - '0');
    if (value > 15) {
      return 0;
    }
  }
  *out = (BinaryXmmRegister)value;
  return 1;
}

static int names_equal(const char (*a)[8], size_t a_count,
                       const char (*b)[8], size_t b_count) {
  size_t i;
  if (a_count != b_count) {
    return 0;
  }
  for (i = 0; i < a_count; i++) {
    if (strcmp(a[i], b[i]) != 0) {
      return 0;
    }
  }
  return 1;
}

static int convention_matches(const MtlcTargetDescription *d,
                              const MtlcTargetDescription *builtin) {
  return names_equal(d->int_args, d->int_arg_count, builtin->int_args,
                     builtin->int_arg_count) &&
         names_equal(d->float_args, d->float_arg_count, builtin->float_args,
                     builtin->float_arg_count) &&
         strcmp(d->indirect_return, builtin->indirect_return) == 0 &&
         (d->separate_classes != 0) == (builtin->separate_classes != 0) &&
         d->shadow_space == builtin->shadow_space;
}

int mtlc_target_describe(const MtlcTargetDescription *description, char *error,
                         size_t error_size) {
  const MtlcTargetEntry *entry = NULL;
  MtlcTargetDescription builtin;
  char triple[64];
  size_t i;
  if (!description) {
    return 0;
  }
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  snprintf(triple, sizeof(triple), "%s-%s", description->arch,
           description->os);
  entry = find_entry(triple);
  if (!entry) {
    snprintf(error, error_size,
             "no machine emitter for arch `%s` with os `%s`; the compiler "
             "emits %s",
             description->arch, description->os, mtlc_target_triple_list());
    return 0;
  }
  describe_entry(entry, triple, &builtin);
  if (description->pointer_bits != builtin.pointer_bits) {
    snprintf(error, error_size,
             "`%s` code has %d-bit pointers; the description says %d",
             description->arch, builtin.pointer_bits,
             description->pointer_bits);
    return 0;
  }
  if (strcmp(description->format, builtin.format) != 0) {
    snprintf(error, error_size,
             "`%s-%s` objects are `%s`; the description says `%s`",
             description->arch, description->os, builtin.format,
             description->format);
    return 0;
  }
  if (description->stack_alignment != builtin.stack_alignment) {
    snprintf(error, error_size,
             "`%s` keeps the stack aligned to %d; the description says %d, "
             "which no emitter honours",
             description->arch, builtin.stack_alignment,
             description->stack_alignment);
    return 0;
  }
  if (description->width_count != builtin.width_count) {
    snprintf(error, error_size,
             "`%s` has %zu integer widths; the description lists %zu",
             description->arch, builtin.width_count, description->width_count);
    return 0;
  }
  for (i = 0; i < description->width_count; i++) {
    if (description->widths[i] != builtin.widths[i]) {
      snprintf(error, error_size,
               "`%s` has no %d-bit integers in this position; the widths are "
               "the machine's, not the description's",
               description->arch, description->widths[i]);
      return 0;
    }
  }
  if (description->vector_width != builtin.vector_width) {
    snprintf(error, error_size,
             "the `%s` emitter vectorizes at %d bits and takes no other "
             "width; the description says %d",
             description->arch, builtin.vector_width,
             description->vector_width);
    return 0;
  }
  if (description->address_space_count != 0) {
    snprintf(error, error_size,
             "a CPU target has no named address spaces; those belong to the "
             "GPU targets, which are not described this way");
    return 0;
  }
  if (entry->os != MTLC_TARGET_OS_NONE || entry->arch != MTLC_TARGET_ARCH_X86_64) {
    if (!convention_matches(description, &builtin)) {
      snprintf(error, error_size,
               entry->os != MTLC_TARGET_OS_NONE
                   ? "a hosted target's calling convention is the platform's, "
                     "and `%s` is hosted; describe a freestanding x86_64 "
                     "target to choose one"
                   : "the `%s` emitter takes only its own calling "
                     "convention; describe a freestanding x86_64 target to "
                     "choose one",
               triple);
      return 0;
    }
  }
  {
    BinaryGpRegister int_regs[MTLC_TARGET_DESC_MAX_REGS];
    BinaryXmmRegister float_regs[MTLC_TARGET_DESC_MAX_REGS];
    BinaryGpRegister indirect = BINARY_GP_RDI;
    size_t j;
    if (entry->arch == MTLC_TARGET_ARCH_X86_64) {
      if (description->int_arg_count == 0 || description->int_arg_count > 6) {
        snprintf(error, error_size,
                 "an x86_64 convention passes between 1 and 6 integer "
                 "arguments in registers; the description lists %zu",
                 description->int_arg_count);
        return 0;
      }
      if (description->float_arg_count == 0 ||
          description->float_arg_count > 8) {
        snprintf(error, error_size,
                 "an x86_64 convention passes between 1 and 8 float "
                 "arguments in registers; the description lists %zu",
                 description->float_arg_count);
        return 0;
      }
      for (i = 0; i < description->int_arg_count; i++) {
        if (!gp_register_by_name(description->int_args[i], &int_regs[i]) ||
            int_regs[i] == BINARY_GP_RAX || int_regs[i] == BINARY_GP_R10 ||
            int_regs[i] == BINARY_GP_R11 || int_regs[i] == BINARY_GP_RBX ||
            int_regs[i] == BINARY_GP_RBP || int_regs[i] == BINARY_GP_R12 ||
            int_regs[i] == BINARY_GP_R13 || int_regs[i] == BINARY_GP_R14 ||
            int_regs[i] == BINARY_GP_R15) {
          snprintf(error, error_size,
                   "`%s` cannot carry an integer argument: the argument "
                   "registers are drawn from rdi, rsi, rdx, rcx, r8 and r9",
                   description->int_args[i]);
          return 0;
        }
        for (j = 0; j < i; j++) {
          if (int_regs[j] == int_regs[i]) {
            snprintf(error, error_size,
                     "`%s` is listed twice among the integer argument "
                     "registers",
                     description->int_args[i]);
            return 0;
          }
        }
      }
      for (i = 0; i < description->float_arg_count; i++) {
        if (!xmm_register_by_name(description->float_args[i],
                                  &float_regs[i]) ||
            float_regs[i] > BINARY_XMM7) {
          snprintf(error, error_size,
                   "`%s` cannot carry a float argument: the argument "
                   "registers are drawn from xmm0 to xmm7",
                   description->float_args[i]);
          return 0;
        }
        for (j = 0; j < i; j++) {
          if (float_regs[j] == float_regs[i]) {
            snprintf(error, error_size,
                     "`%s` is listed twice among the float argument "
                     "registers",
                     description->float_args[i]);
            return 0;
          }
        }
      }
      if (!gp_register_by_name(description->indirect_return, &indirect) ||
          indirect != int_regs[0]) {
        snprintf(error, error_size,
                 "the indirect return register is the first integer "
                 "argument register, `%s`; the description says `%s`",
                 description->int_args[0], description->indirect_return);
        return 0;
      }
      if (description->shadow_space != 0 && description->shadow_space != 32) {
        snprintf(error, error_size,
                 "shadow space is 0 or 32 bytes; the description says %d",
                 description->shadow_space);
        return 0;
      }
      if (description->red_zone != builtin.red_zone) {
        snprintf(error, error_size,
                 "`%s` has a red zone of %d bytes and the emitter does not "
                 "vary it; the description says %d",
                 triple, builtin.red_zone, description->red_zone);
        return 0;
      }
    }
    g_target.arch = entry->arch;
    g_target.os = entry->os;
    g_target.code_bits = entry->code_bits;
    g_target.format = entry->format;
    g_target.freestanding = entry->freestanding;
    g_target.explicit_triple = 1;
    snprintf(g_target.triple, sizeof(g_target.triple), "%s", description->name);
    g_description = *description;
    g_description.described_convention =
        entry->arch == MTLC_TARGET_ARCH_X86_64 &&
        !convention_matches(description, &builtin);
    g_description_valid = 1;
    if (entry->arch == MTLC_TARGET_ARCH_X86_64) {
      code_generator_binary_describe_abi(
          int_regs, description->int_arg_count, float_regs,
          description->float_arg_count, description->shadow_space, indirect,
          description->separate_classes);
    }
  }
  return 1;
}

const MtlcTargetDescription *mtlc_target_current_description(void) {
  char error[128];
  if (g_description_valid) {
    return &g_description;
  }
  if (!g_target_initialized) {
    mtlc_target_init_host();
  }
  if (mtlc_target_description_of(g_target.triple, &g_description, error,
                                 sizeof(error))) {
    g_description_valid = 1;
    return &g_description;
  }
  return NULL;
}

static void print_names(FILE *out, const char (*names)[8], size_t count) {
  size_t i;
  fputc('[', out);
  for (i = 0; i < count; i++) {
    fprintf(out, "%s\"%s\"", i ? ", " : "", names[i]);
  }
  fputc(']', out);
}

void mtlc_target_print_description(FILE *out,
                                   const MtlcTargetDescription *d) {
  size_t i;
  if (!out || !d) {
    return;
  }
  fprintf(out, "import \"std/target\";\n\n");
  fprintf(out, "export const TARGET: TargetDesc = {\n");
  fprintf(out, "  name: \"%s\",\n", d->name);
  fprintf(out, "  arch: \"%s\",\n", d->arch);
  fprintf(out, "  os: \"%s\",\n", d->os);
  fprintf(out, "  format: \"%s\",\n", d->format);
  fprintf(out, "  pointer_bits: %d,\n", d->pointer_bits);
  fprintf(out, "  stack_alignment: %d,\n", d->stack_alignment);
  fprintf(out, "  shadow_space: %d,\n", d->shadow_space);
  fprintf(out, "  red_zone: %d,\n", d->red_zone);
  fprintf(out, "  int_args: ");
  print_names(out, d->int_args, d->int_arg_count);
  fprintf(out, ",\n  float_args: ");
  print_names(out, d->float_args, d->float_arg_count);
  fprintf(out, ",\n  indirect_return: \"%s\",\n", d->indirect_return);
  fprintf(out, "  separate_classes: %s,\n",
          d->separate_classes ? "true" : "false");
  fprintf(out, "  widths: [");
  for (i = 0; i < d->width_count; i++) {
    fprintf(out, "%s%d", i ? ", " : "", d->widths[i]);
  }
  fprintf(out, "],\n  vector_width: %d,\n", d->vector_width);
  fprintf(out, "  address_spaces: [");
  for (i = 0; i < d->address_space_count; i++) {
    fprintf(out, "%s\"%s\"", i ? ", " : "", d->address_spaces[i]);
  }
  fprintf(out, "]\n};\n");
}

int mtlc_target_syscall_max_arguments(const MtlcTarget *target) {
  if (!target || target->code_bits != 64) {
    return -1;
  }
  if (target->arch == MTLC_TARGET_ARCH_AARCH64) {
    return MTLC_SYSCALL_MAX_ARGUMENTS_SVC;
  }
  if (target->arch != MTLC_TARGET_ARCH_X86_64) {
    return -1;
  }
  return target->os == MTLC_TARGET_OS_WINDOWS
             ? MTLC_SYSCALL_MAX_ARGUMENTS_NT
             : MTLC_SYSCALL_MAX_ARGUMENTS_SYSV;
}
