#ifndef MTLC_TARGET_H
#define MTLC_TARGET_H

#include "binary_emitter.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MTLC_TARGET_ARCH_X86_64 = 0,
  MTLC_TARGET_ARCH_X86_32,
  MTLC_TARGET_ARCH_X86_16,
  MTLC_TARGET_ARCH_AARCH64
} MtlcTargetArch;

typedef enum {
  MTLC_TARGET_OS_WINDOWS = 0,
  MTLC_TARGET_OS_LINUX,
  MTLC_TARGET_OS_NONE
} MtlcTargetOs;

typedef struct {
  MtlcTargetArch arch;
  MtlcTargetOs os;
  int code_bits;
  BinaryTargetFormat format;
  int freestanding;
  int explicit_triple;
  uint64_t image_base;
  int image_base_set;
  uint64_t section_alignment;
  char triple[64];
} MtlcTarget;

const MtlcTarget *mtlc_target(void);

int mtlc_target_select(const char *triple, char *error, size_t error_size);

void mtlc_target_set_image_base(uint64_t base);

const char *mtlc_target_triple_list(void);

int mtlc_target_is_object_capable(const MtlcTarget *target);

#define MTLC_SYSCALL_MAX_ARGUMENTS_SYSV 6
#define MTLC_SYSCALL_MAX_ARGUMENTS_SVC 6
#define MTLC_SYSCALL_MAX_ARGUMENTS_NT 15

int mtlc_target_syscall_max_arguments(const MtlcTarget *target);

MtlcTargetOs mtlc_target_host_os(void);

const char *mtlc_target_os_name(MtlcTargetOs os);

#define MTLC_TARGET_DESC_MAX_REGS 16
#define MTLC_TARGET_DESC_MAX_NAMES 8

typedef struct {
  char name[64];
  char arch[16];
  char os[16];
  char format[16];
  int pointer_bits;
  int stack_alignment;
  int shadow_space;
  int red_zone;
  char int_args[MTLC_TARGET_DESC_MAX_REGS][8];
  size_t int_arg_count;
  char float_args[MTLC_TARGET_DESC_MAX_REGS][8];
  size_t float_arg_count;
  char indirect_return[8];
  int separate_classes;
  int widths[MTLC_TARGET_DESC_MAX_NAMES];
  size_t width_count;
  int vector_width;
  char address_spaces[MTLC_TARGET_DESC_MAX_NAMES][16];
  size_t address_space_count;
  int described_convention;
} MtlcTargetDescription;

int mtlc_target_description_of(const char *triple, MtlcTargetDescription *out,
                               char *error, size_t error_size);

int mtlc_target_describe(const MtlcTargetDescription *description, char *error,
                         size_t error_size);

const MtlcTargetDescription *mtlc_target_current_description(void);

void mtlc_target_print_description(FILE *out,
                                   const MtlcTargetDescription *description);

#ifdef __cplusplus
}
#endif

#endif
