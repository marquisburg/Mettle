#include "codegen/binary/internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const BinaryGpRegister BINARY_WIN64_INT_PARAM_REGISTERS[] = {
    BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};
const BinaryXmmRegister BINARY_WIN64_FLOAT_PARAM_REGISTERS[] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3};

/* SYSCALL (0F 05): invoke a kernel system call on x86-64. Used by the Linux
 * self-contained _start to call exit() without libc. */
int binary_emit_syscall(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }
  return binary_code_buffer_append_u8(buffer, 0x0f) &&
         binary_code_buffer_append_u8(buffer, 0x05);
}

int binary_emit_rex(BinaryCodeBuffer *buffer, int w, int r, int x,
                           int b) {
  unsigned char rex = (unsigned char)(0x40 | (w ? 0x08 : 0) |
                                      (r ? 0x04 : 0) | (x ? 0x02 : 0) |
                                      (b ? 0x01 : 0));
  if (rex == 0x40) {
    return 1;
  }
  return binary_code_buffer_append_u8(buffer, rex);
}

static int binary_emit_rex_maybe_forced(BinaryCodeBuffer *buffer, int w, int r,
                                        int x, int b, int force) {
  unsigned char rex = (unsigned char)(0x40 | (w ? 0x08 : 0) |
                                      (r ? 0x04 : 0) | (x ? 0x02 : 0) |
                                      (b ? 0x01 : 0));
  if (rex == 0x40 && !force) {
    return 1;
  }
  return binary_code_buffer_append_u8(buffer, rex);
}

int binary_emit_push_reg(BinaryCodeBuffer *buffer,
                                BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }
  if ((int)reg >= 8 && !binary_emit_rex(buffer, 0, 0, 0, 1)) {
    return 0;
  }
  return binary_code_buffer_append_u8(buffer, (unsigned char)(0x50 + (reg & 7)));
}

int binary_emit_pop_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }
  if ((int)reg >= 8 && !binary_emit_rex(buffer, 0, 0, 0, 1)) {
    return 0;
  }
  return binary_code_buffer_append_u8(buffer, (unsigned char)(0x58 + (reg & 7)));
}

int binary_emit_mov_reg_reg(BinaryCodeBuffer *buffer,
                                   BinaryGpRegister destination,
                                   BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  if (destination == source) {
    return 1;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x8B) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_mov_reg_reg32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister destination,
                                     BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  if (destination == source) {
    return 1;
  }
  return binary_emit_movzx_reg_reg32(buffer, destination, source);
}

/* 32-bit reg-reg mov that ALWAYS emits, even for destination == source.
 * `mov r32, r32` zero-extends into the upper half, so the same-register form
 * is the canonical uint32 zero-extension, callers relying on that zeroing
 * must use this, not binary_emit_mov_reg_reg32 (whose same-register no-op
 * would silently skip it). */
/* 32-bit-operand-size forms of the ALU and unary encoders below. On x86-64 a
 * 32-bit result is zero-extended to the full register for free, so computing a
 * uint32 expression at operand size 32 makes the truncation the language
 * requires implicit -- no separate zero-extend instruction, and no cycle for it
 * on the dependence chain. Widening to 32-bit immediates also becomes free:
 * `and eax, 0xedb88320` is one instruction, where the 64-bit form has to
 * materialize the constant into a register first because it cannot be
 * expressed as a sign-extended imm32. */

int binary_emit_alu_reg_reg32(BinaryCodeBuffer *buffer, unsigned char opcode,
                              BinaryGpRegister destination,
                              BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  if (!binary_emit_rex(buffer, 0, source >> 3, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(buffer, opcode) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((source & 7) << 3) | (destination & 7)))) {
    return 0;
  }
  return 1;
}

/* Unlike the 64-bit form this never elides a no-op immediate (`add r,0` and
 * friends): at operand size 32 the instruction still performs the zero-extend
 * that the caller is relying on. */
int binary_emit_alu_reg_imm_w32(BinaryCodeBuffer *buffer,
                                unsigned char subopcode, BinaryGpRegister reg,
                                uint32_t immediate) {
  if (!buffer) {
    return 0;
  }

  int32_t signed_immediate = (int32_t)immediate;
  if (signed_immediate >= INT8_MIN && signed_immediate <= INT8_MAX) {
    if (!binary_emit_rex(buffer, 0, 0, 0, reg >> 3) ||
        !binary_code_buffer_append_u8(buffer, 0x83) ||
        !binary_code_buffer_append_u8(
            buffer,
            (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7))) ||
        !binary_code_buffer_append_u8(buffer,
                                      (unsigned char)(int8_t)signed_immediate)) {
      return 0;
    }
    return 1;
  }

  if (!binary_emit_rex(buffer, 0, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x81) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7))) ||
      !binary_code_buffer_append_u32(buffer, immediate)) {
    return 0;
  }
  return 1;
}

int binary_emit_unary_reg32(BinaryCodeBuffer *buffer, unsigned char subopcode,
                            BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }
  if (!binary_emit_rex(buffer, 0, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0xF7) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7)))) {
    return 0;
  }
  return 1;
}

int binary_emit_neg_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  return binary_emit_unary_reg32(buffer, 3, reg);
}

int binary_emit_not_reg32(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  return binary_emit_unary_reg32(buffer, 2, reg);
}

int binary_emit_movzx_reg_reg32(BinaryCodeBuffer *buffer,
                                BinaryGpRegister destination,
                                BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  if (!binary_emit_rex(buffer, 0, source >> 3, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x89) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((source & 7) << 3) |
                                  (destination & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_mov_reg_imm32_zero_extend(BinaryCodeBuffer *buffer,
                                                 BinaryGpRegister destination,
                                                 uint32_t immediate) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 0, 0, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xB8 + (destination & 7))) ||
      !binary_code_buffer_append_u32(buffer, immediate)) {
    return 0;
  }

  return 1;
}

int binary_emit_xor_reg_reg32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 0, reg >> 3, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x31) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((reg & 7) << 3) | (reg & 7)))) {
    return 0;
  }

  return 1;
}


int binary_emit_alu_rsp_imm32(BinaryCodeBuffer *buffer,
                                     unsigned char subopcode,
                                     uint32_t immediate) {
  if (!buffer) {
    return 0;
  }
  if (immediate == 0) {
    return 1;
  }

  int32_t signed_immediate = (int32_t)immediate;
  if (signed_immediate >= INT8_MIN && signed_immediate <= INT8_MAX) {
    if (!binary_emit_rex(buffer, 1, 0, 0, 0) ||
        !binary_code_buffer_append_u8(buffer, 0x83) ||
        !binary_code_buffer_append_u8(
            buffer, (unsigned char)(0xC0 | ((subopcode & 7) << 3) |
                                    (BINARY_GP_RSP & 7))) ||
        !binary_code_buffer_append_u8(buffer,
                                      (unsigned char)(int8_t)signed_immediate)) {
      return 0;
    }
    return 1;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, 0) ||
      !binary_code_buffer_append_u8(buffer, 0x81) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((subopcode & 7) << 3) |
                                  (BINARY_GP_RSP & 7))) ||
      !binary_code_buffer_append_u32(buffer, immediate)) {
    return 0;
  }

  return 1;
}

int binary_emit_sub_rsp_imm32(BinaryCodeBuffer *buffer,
                                     uint32_t immediate) {
  return binary_emit_alu_rsp_imm32(buffer, 5, immediate);
}

int binary_emit_add_rsp_imm32(BinaryCodeBuffer *buffer,
                                     uint32_t immediate) {
  return binary_emit_alu_rsp_imm32(buffer, 0, immediate);
}

int binary_emit_alu_reg_imm32(BinaryCodeBuffer *buffer,
                                     unsigned char subopcode,
                                     BinaryGpRegister reg, uint32_t immediate) {
  if (!buffer) {
    return 0;
  }
  if ((subopcode == 0 || subopcode == 1 || subopcode == 5 ||
       subopcode == 6) &&
      immediate == 0) {
    return 1;
  }
  if (subopcode == 4 && immediate == UINT32_MAX) {
    return 1;
  }

  int32_t signed_immediate = (int32_t)immediate;
  if (signed_immediate >= INT8_MIN && signed_immediate <= INT8_MAX) {
    if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
        !binary_code_buffer_append_u8(buffer, 0x83) ||
        !binary_code_buffer_append_u8(
            buffer,
            (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7))) ||
        !binary_code_buffer_append_u8(buffer,
                                      (unsigned char)(int8_t)signed_immediate)) {
      return 0;
    }
    return 1;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x81) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7))) ||
      !binary_code_buffer_append_u32(buffer, immediate)) {
    return 0;
  }

  return 1;
}

int binary_emit_add_reg_imm32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg,
                                     uint32_t immediate) {
  return binary_emit_alu_reg_imm32(buffer, 0, reg, immediate);
}

int binary_emit_sub_reg_imm32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg,
                                     uint32_t immediate) {
  return binary_emit_alu_reg_imm32(buffer, 5, reg, immediate);
}

int binary_emit_and_reg_imm32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg,
                                     uint32_t immediate) {
  return binary_emit_alu_reg_imm32(buffer, 4, reg, immediate);
}

int binary_emit_or_reg_imm32(BinaryCodeBuffer *buffer,
                                    BinaryGpRegister reg, uint32_t immediate) {
  return binary_emit_alu_reg_imm32(buffer, 1, reg, immediate);
}

int binary_emit_xor_reg_imm32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg,
                                     uint32_t immediate) {
  return binary_emit_alu_reg_imm32(buffer, 6, reg, immediate);
}

int binary_emit_cmp_reg_imm32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg,
                                     uint32_t immediate) {
  if (immediate == 0) {
    return binary_emit_test_reg_reg(buffer, reg);
  }
  return binary_emit_alu_reg_imm32(buffer, 7, reg, immediate);
}

/* 32-bit `cmp r/m32, imm` (no REX.W), so a 4-byte int32/uint32 value is compared
 * against an immediate without the 64-bit sign-extension of the W=1 form -- and
 * without staging the constant through a scratch register. Uses the imm8 short
 * form (0x83 /7) when the value fits, else the imm32 form (0x81 /7). */
int binary_emit_cmp_reg_imm_w32(BinaryCodeBuffer *buffer, BinaryGpRegister reg,
                                uint32_t immediate) {
  if (!buffer) {
    return 0;
  }
  int32_t s = (int32_t)immediate;
  if (s >= INT8_MIN && s <= INT8_MAX) {
    return binary_emit_rex(buffer, 0, 0, 0, reg >> 3) &&
           binary_code_buffer_append_u8(buffer, 0x83) &&
           binary_code_buffer_append_u8(
               buffer, (unsigned char)(0xC0 | (7 << 3) | (reg & 7))) &&
           binary_code_buffer_append_u8(buffer, (unsigned char)(int8_t)s);
  }
  return binary_emit_rex(buffer, 0, 0, 0, reg >> 3) &&
         binary_code_buffer_append_u8(buffer, 0x81) &&
         binary_code_buffer_append_u8(
             buffer, (unsigned char)(0xC0 | (7 << 3) | (reg & 7))) &&
         binary_code_buffer_append_u32(buffer, immediate);
}

int binary_emit_mov_reg_imm64(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister destination,
                                     uint64_t immediate) {
  if (!buffer) {
    return 0;
  }
  if (immediate == 0) {
    /* mov, not xor: xor sets ZF and breaks cmov/cc sequences that load a
     * zero immediate between compare and conditional move. */
    return binary_emit_mov_reg_imm32_zero_extend(buffer, destination, 0);
  }
  if (immediate <= UINT32_MAX) {
    return binary_emit_mov_reg_imm32_zero_extend(buffer, destination,
                                                (uint32_t)immediate);
  }
  if (immediate >= UINT64_C(0xffffffff80000000)) {
    int32_t signed_immediate = (int32_t)immediate;
    if (!binary_emit_rex(buffer, 1, 0, 0, destination >> 3) ||
        !binary_code_buffer_append_u8(buffer, 0xC7) ||
        !binary_code_buffer_append_u8(
            buffer, (unsigned char)(0xC0 | (destination & 7))) ||
        !binary_code_buffer_append_u32(buffer, (uint32_t)signed_immediate)) {
      return 0;
    }
    return 1;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xB8 + (destination & 7))) ||
      !binary_code_buffer_append_u64(buffer, immediate)) {
    return 0;
  }

  return 1;
}

static int binary_emit_memory_access_ex_internal(
    BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w,
    unsigned char opcode1, int has_opcode2, unsigned char opcode2,
    BinaryGpRegister reg, BinaryGpRegister base, int displacement,
    int force_rex) {
  if (!buffer) {
    return 0;
  }

  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char mod = use_disp8 ? 1 : 2;
  unsigned char rm = (unsigned char)(base & 7);
  unsigned char modrm =
      (unsigned char)((mod << 6) | ((reg & 7) << 3) |
                      ((rm == (BINARY_GP_RSP & 7)) ? 4 : rm));

  if ((operand_size_prefix &&
       !binary_code_buffer_append_u8(buffer, 0x66)) ||
      !binary_emit_rex_maybe_forced(buffer, rex_w, reg >> 3, 0, base >> 3,
                                    force_rex) ||
      !binary_code_buffer_append_u8(buffer, opcode1) ||
      (has_opcode2 && !binary_code_buffer_append_u8(buffer, opcode2)) ||
      !binary_code_buffer_append_u8(buffer, modrm)) {
    return 0;
  }

  if (rm == (BINARY_GP_RSP & 7)) {
    unsigned char sib =
        (unsigned char)((0 << 6) | (4 << 3) | (base & 7));
    if (!binary_code_buffer_append_u8(buffer, sib)) {
      return 0;
    }
  }

  if (use_disp8) {
    return binary_code_buffer_append_u8(buffer, (unsigned char)(int8_t)displacement);
  }

  return binary_code_buffer_append_u32(buffer, (uint32_t)(int32_t)displacement);
}

int binary_emit_memory_access_ex(BinaryCodeBuffer *buffer,
                                        int operand_size_prefix, int rex_w,
                                        unsigned char opcode1,
                                        int has_opcode2,
                                        unsigned char opcode2,
                                        BinaryGpRegister reg,
                                        BinaryGpRegister base,
                                        int displacement) {
  return binary_emit_memory_access_ex_internal(
      buffer, operand_size_prefix, rex_w, opcode1, has_opcode2, opcode2, reg,
      base, displacement, 0);
}

/* Like binary_emit_memory_access_ex, but forcing an (empty) REX prefix. A byte
 * op whose register operand encodes as 4..7 means SPL/BPL/SIL/DIL only under a
 * REX prefix; without one those encodings name AH/CH/DH/BH. */
int binary_emit_memory_access_ex_forced(BinaryCodeBuffer *buffer,
                                        int operand_size_prefix, int rex_w,
                                        unsigned char opcode1, int has_opcode2,
                                        unsigned char opcode2,
                                        BinaryGpRegister reg,
                                        BinaryGpRegister base,
                                        int displacement) {
  return binary_emit_memory_access_ex_internal(
      buffer, operand_size_prefix, rex_w, opcode1, has_opcode2, opcode2, reg,
      base, displacement, 1);
}

/* prefetcht0 [base + disp]: 0F 18 /1. Advisory -- never faults, so a bad
 * (speculative, out-of-range) address costs nothing but the hint. */
int binary_emit_prefetcht0_mem(BinaryCodeBuffer *buffer,
                               BinaryGpRegister base, int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 0, 0x0F, 1, 0x18, 1, base,
                                      displacement);
}

/* Like binary_emit_memory_access_ex but with a scaled-index SIB address
 * [base + index*scale + disp]. `reg` is the ModRM.reg operand (load dest or
 * store source). scale must be 1/2/4/8 and index must not be RSP. */
static int binary_emit_memory_access_sib_internal(
    BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w,
    unsigned char opcode1, int has_opcode2, unsigned char opcode2,
    BinaryGpRegister reg, BinaryGpRegister base, BinaryGpRegister index,
    int scale, int displacement, int force_rex) {
  if (!buffer || index == BINARY_GP_RSP) {
    return 0;
  }
  unsigned char scale_bits;
  switch (scale) {
  case 1: scale_bits = 0; break;
  case 2: scale_bits = 1; break;
  case 4: scale_bits = 2; break;
  case 8: scale_bits = 3; break;
  default: return 0;
  }
  /* mod==00 has no displacement, but base low-3 == 5 (RBP/R13) forces disp8. */
  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char mod;
  if (displacement == 0 && (base & 7) != (BINARY_GP_RBP & 7)) {
    mod = 0;
  } else {
    mod = use_disp8 ? 1 : 2;
  }
  if ((operand_size_prefix && !binary_code_buffer_append_u8(buffer, 0x66)) ||
      !binary_emit_rex_maybe_forced(buffer, rex_w, reg >> 3, index >> 3,
                                    base >> 3, force_rex) ||
      !binary_code_buffer_append_u8(buffer, opcode1) ||
      (has_opcode2 && !binary_code_buffer_append_u8(buffer, opcode2)) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)((mod << 6) | ((reg & 7) << 3) | 4)) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)((scale_bits << 6) | ((index & 7) << 3) |
                                  (base & 7)))) {
    return 0;
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(buffer,
                                        (unsigned char)(int8_t)displacement);
  }
  if (mod == 2) {
    return binary_code_buffer_append_u32(buffer,
                                         (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int binary_emit_mov_mem_imm_width(BinaryCodeBuffer *buffer,
                                  BinaryGpRegister base, int has_index,
                                  BinaryGpRegister index, int scale,
                                  int displacement, long long value,
                                  int width) {
  unsigned char opcode = (width == 1) ? 0xC6 : 0xC7;
  int prefix16 = (width == 2);
  int rex_w = (width == 8);
  if (width != 1 && width != 2 && width != 4 && width != 8) {
    return 0;
  }
  if (rex_w && (value < -2147483648LL || value > 2147483647LL)) {
    return 0;
  }
  if (has_index) {
    if (!binary_emit_memory_access_sib(buffer, prefix16, rex_w, opcode, 0, 0,
                                       (BinaryGpRegister)0, base, index, scale,
                                       displacement)) {
      return 0;
    }
  } else if (!binary_emit_memory_access_ex(buffer, prefix16, rex_w, opcode, 0,
                                           0, (BinaryGpRegister)0, base,
                                           displacement)) {
    return 0;
  }
  if (width == 1) {
    return binary_code_buffer_append_u8(buffer, (unsigned char)value);
  }
  if (width == 2) {
    return binary_code_buffer_append_u8(buffer, (unsigned char)(value & 0xFF)) &&
           binary_code_buffer_append_u8(buffer,
                                        (unsigned char)((value >> 8) & 0xFF));
  }
  return binary_code_buffer_append_u32(buffer, (uint32_t)(int32_t)value);
}

int binary_emit_memory_access_sib(BinaryCodeBuffer *buffer,
                                  int operand_size_prefix, int rex_w,
                                  unsigned char opcode1, int has_opcode2,
                                  unsigned char opcode2, BinaryGpRegister reg,
                                  BinaryGpRegister base, BinaryGpRegister index,
                                  int scale, int displacement) {
  return binary_emit_memory_access_sib_internal(
      buffer, operand_size_prefix, rex_w, opcode1, has_opcode2, opcode2, reg,
      base, index, scale, displacement, 0);
}

/* Forced-REX variant: a byte op whose register operand encodes as 4..7 means
 * SPL/BPL/SIL/DIL only under a REX prefix; without one those encodings name
 * AH/CH/DH/BH. */
int binary_emit_memory_access_sib_forced(
    BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w,
    unsigned char opcode1, int has_opcode2, unsigned char opcode2,
    BinaryGpRegister reg, BinaryGpRegister base, BinaryGpRegister index,
    int scale, int displacement) {
  return binary_emit_memory_access_sib_internal(
      buffer, operand_size_prefix, rex_w, opcode1, has_opcode2, opcode2, reg,
      base, index, scale, displacement, 1);
}

int binary_emit_memory_access(BinaryCodeBuffer *buffer,
                                     unsigned char opcode,
                                     BinaryGpRegister reg,
                                     BinaryGpRegister base, int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 1, opcode, 0, 0, reg, base,
                                      displacement);
}

int binary_emit_mov_reg_mem(BinaryCodeBuffer *buffer,
                                   BinaryGpRegister destination,
                                   BinaryGpRegister base, int displacement) {
  return binary_emit_memory_access(buffer, 0x8B, destination, base,
                                   displacement);
}

int binary_emit_mov_mem_reg(BinaryCodeBuffer *buffer,
                                   BinaryGpRegister base, int displacement,
                                   BinaryGpRegister source) {
  return binary_emit_memory_access(buffer, 0x89, source, base, displacement);
}

/* Pad the code buffer up to a `boundary`-byte alignment with NOPs.
 *
 * Uses the canonical multi-byte NOP forms rather than a run of 0x90: a 15-byte
 * gap is then two instructions instead of fifteen. That matters because
 * alignment padding sits on the fall-through path into a loop, so it is
 * decoded (once) rather than jumped over.
 *
 * `max_pad` caps how far the buffer will be pushed; a gap wider than that is
 * left alone, since the padding would cost more instruction bytes than the
 * alignment is worth. boundary must be a power of two. */
int binary_emit_align_code(BinaryCodeBuffer *buffer, size_t boundary,
                           size_t max_pad) {
  static const unsigned char kNops[10][9] = {
      {0},
      {0x90},
      {0x66, 0x90},
      {0x0F, 0x1F, 0x00},
      {0x0F, 0x1F, 0x40, 0x00},
      {0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
      {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
  };

  if (!buffer || boundary < 2 || (boundary & (boundary - 1)) != 0) {
    return 1;
  }
  size_t pad = (boundary - (buffer->size & (boundary - 1))) & (boundary - 1);
  if (pad == 0 || pad > max_pad) {
    return 1;
  }
  while (pad > 0) {
    size_t chunk = pad > 9 ? 9 : pad;
    for (size_t i = 0; i < chunk; i++) {
      if (!binary_code_buffer_append_u8(buffer, kNops[chunk][i])) {
        return 0;
      }
    }
    pad -= chunk;
  }
  return 1;
}

/* mov qword [base+disp], imm32 (sign-extended to 64) : REX.W C7 /0 id.
 *
 * Only the sign-extending form exists, so the caller must have checked the
 * immediate fits in a signed 32-bit field. Storing a constant to a stack slot
 * would otherwise cost a scratch register plus two instructions. */
int binary_emit_mov_mem_imm32(BinaryCodeBuffer *buffer, BinaryGpRegister base,
                              int displacement, int32_t immediate) {
  /* ModRM.reg carries the /0 sub-opcode, not a register. */
  return binary_emit_memory_access_ex(buffer, 0, 1, 0xC7, 0, 0,
                                      (BinaryGpRegister)0, base,
                                      displacement) &&
         binary_code_buffer_append_u32(buffer, (uint32_t)immediate);
}

int binary_emit_movzx_reg_mem8(BinaryCodeBuffer *buffer,
                                      BinaryGpRegister destination,
                                      BinaryGpRegister base,
                                      int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 0, 0x0F, 1, 0xB6,
                                      destination, base, displacement);
}

int binary_emit_movzx_reg_mem16(BinaryCodeBuffer *buffer,
                                       BinaryGpRegister destination,
                                       BinaryGpRegister base,
                                       int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 0, 0x0F, 1, 0xB7,
                                      destination, base, displacement);
}

int binary_emit_mov_reg_mem32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister destination,
                                     BinaryGpRegister base, int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 0, 0x8B, 0, 0, destination,
                                      base, displacement);
}

/* movsx r64, byte [base+disp] : REX.W 0F BE /r */
int binary_emit_movsx_reg_mem8(BinaryCodeBuffer *buffer,
                                      BinaryGpRegister destination,
                                      BinaryGpRegister base,
                                      int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 1, 0x0F, 1, 0xBE,
                                      destination, base, displacement);
}

/* movsx r64, word [base+disp] : REX.W 0F BF /r */
int binary_emit_movsx_reg_mem16(BinaryCodeBuffer *buffer,
                                       BinaryGpRegister destination,
                                       BinaryGpRegister base,
                                       int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 1, 0x0F, 1, 0xBF,
                                      destination, base, displacement);
}

/* movsxd r64, dword [base+disp] : REX.W 63 /r */
int binary_emit_movsxd_reg_mem(BinaryCodeBuffer *buffer,
                                      BinaryGpRegister destination,
                                      BinaryGpRegister base,
                                      int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 1, 0x63, 0, 0, destination,
                                      base, displacement);
}

int binary_emit_mov_mem_reg8(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister base, int displacement,
                                     BinaryGpRegister source) {
  /* REX is mandatory for SPL/BPL/SIL/DIL; without it ModRM reg codes 4..7
   * name AH/CH/DH/BH instead. */
  int force_rex = source >= BINARY_GP_RSP && source <= BINARY_GP_RDI;
  return binary_emit_memory_access_ex_internal(
      buffer, 0, 0, 0x88, 0, 0, source, base, displacement, force_rex);
}

int binary_emit_mov_mem_reg16(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister base, int displacement,
                                     BinaryGpRegister source) {
  return binary_emit_memory_access_ex(buffer, 1, 0, 0x89, 0, 0, source, base,
                                      displacement);
}

int binary_emit_mov_mem_reg32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister base, int displacement,
                                     BinaryGpRegister source) {
  return binary_emit_memory_access_ex(buffer, 0, 0, 0x89, 0, 0, source, base,
                                      displacement);
}

int binary_emit_lea_reg_mem(BinaryCodeBuffer *buffer,
                                   BinaryGpRegister destination,
                                   BinaryGpRegister base, int displacement) {
  return binary_emit_memory_access(buffer, 0x8D, destination, base,
                                   displacement);
}

int binary_emit_lea_reg_base_index_scale_disp(
    BinaryCodeBuffer *buffer, BinaryGpRegister destination,
    BinaryGpRegister base, BinaryGpRegister index, int scale,
    int displacement) {
  if (!buffer || index == BINARY_GP_RSP) {
    return 0;
  }

  unsigned char scale_bits = 0;
  switch (scale) {
  case 1:
    scale_bits = 0;
    break;
  case 2:
    scale_bits = 1;
    break;
  case 4:
    scale_bits = 2;
    break;
  case 8:
    scale_bits = 3;
    break;
  default:
    return 0;
  }

  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char mod = 0;
  if (displacement == 0 &&
      (base & 7) != (BINARY_GP_RBP & 7)) {
    mod = 0;
  } else {
    mod = use_disp8 ? 1 : 2;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, index >> 3, base >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x8D) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)((mod << 6) | ((destination & 7) << 3) | 4)) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)((scale_bits << 6) | ((index & 7) << 3) |
                                  (base & 7)))) {
    return 0;
  }

  if (mod == 1) {
    return binary_code_buffer_append_u8(buffer,
                                        (unsigned char)(int8_t)displacement);
  }
  if (mod == 2) {
    return binary_code_buffer_append_u32(buffer,
                                         (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int binary_emit_lea32_reg_mem(BinaryCodeBuffer *buffer,
                              BinaryGpRegister destination,
                              BinaryGpRegister base, int displacement) {
  return binary_emit_memory_access_ex(buffer, 0, 0, 0x8D, 0, 0, destination,
                                      base, displacement);
}

int binary_emit_lea32_reg_base_index_scale_disp(
    BinaryCodeBuffer *buffer, BinaryGpRegister destination,
    BinaryGpRegister base, BinaryGpRegister index, int scale,
    int displacement) {
  if (!buffer || index == BINARY_GP_RSP) {
    return 0;
  }

  unsigned char scale_bits = 0;
  switch (scale) {
  case 1:
    scale_bits = 0;
    break;
  case 2:
    scale_bits = 1;
    break;
  case 4:
    scale_bits = 2;
    break;
  case 8:
    scale_bits = 3;
    break;
  default:
    return 0;
  }

  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char mod = 0;
  if (displacement == 0 &&
      (base & 7) != (BINARY_GP_RBP & 7)) {
    mod = 0;
  } else {
    mod = use_disp8 ? 1 : 2;
  }

  if (!binary_emit_rex(buffer, 0, destination >> 3, index >> 3, base >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x8D) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)((mod << 6) | ((destination & 7) << 3) | 4)) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)((scale_bits << 6) | ((index & 7) << 3) |
                                  (base & 7)))) {
    return 0;
  }

  if (mod == 1) {
    return binary_code_buffer_append_u8(buffer,
                                        (unsigned char)(int8_t)displacement);
  }
  if (mod == 2) {
    return binary_code_buffer_append_u32(buffer,
                                         (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int binary_emit_shift_reg_imm8_32(BinaryCodeBuffer *buffer,
                                  unsigned char subopcode,
                                  BinaryGpRegister reg,
                                  unsigned char immediate) {
  if (!buffer) {
    return 0;
  }
  if (immediate == 0) {
    return 1;
  }
  if (!binary_emit_rex(buffer, 0, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, immediate == 1 ? 0xD1 : 0xC1) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7)))) {
    return 0;
  }
  if (immediate != 1 && !binary_code_buffer_append_u8(buffer, immediate)) {
    return 0;
  }
  return 1;
}

int binary_emit_lea_reg_reg(BinaryCodeBuffer *buffer,
                                   BinaryGpRegister destination,
                                   BinaryGpRegister lhs,
                                   BinaryGpRegister rhs) {
  if (rhs != BINARY_GP_RSP) {
    return binary_emit_lea_reg_base_index_scale_disp(buffer, destination, lhs,
                                                    rhs, 1, 0);
  }
  if (lhs != BINARY_GP_RSP) {
    return binary_emit_lea_reg_base_index_scale_disp(buffer, destination, rhs,
                                                    lhs, 1, 0);
  }
  return 0;
}

int binary_emit_lea_reg_rip_placeholder(BinaryCodeBuffer *buffer,
                                               BinaryGpRegister destination,
                                               size_t *displacement_offset_out) {
  if (!buffer || !displacement_offset_out) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, 0) ||
      !binary_code_buffer_append_u8(buffer, 0x8D) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0x05 | ((destination & 7) << 3)))) {
    return 0;
  }

  *displacement_offset_out = buffer->size;
  return binary_code_buffer_append_u32(buffer, 0);
}

static int binary_emit_rip_relative_access_ex_internal(
    BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w,
    unsigned char opcode1, int has_opcode2, unsigned char opcode2,
    BinaryGpRegister reg, size_t *displacement_offset_out, int force_rex) {
  if (!buffer || !displacement_offset_out) {
    return 0;
  }

  if ((operand_size_prefix &&
       !binary_code_buffer_append_u8(buffer, 0x66)) ||
      !binary_emit_rex_maybe_forced(buffer, rex_w, reg >> 3, 0, 0,
                                    force_rex) ||
      !binary_code_buffer_append_u8(buffer, opcode1) ||
      (has_opcode2 && !binary_code_buffer_append_u8(buffer, opcode2)) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0x05 | ((reg & 7) << 3)))) {
    return 0;
  }

  *displacement_offset_out = buffer->size;
  return binary_code_buffer_append_u32(buffer, 0);
}

int binary_emit_rip_relative_access_ex(
    BinaryCodeBuffer *buffer, int operand_size_prefix, int rex_w,
    unsigned char opcode1, int has_opcode2, unsigned char opcode2,
    BinaryGpRegister reg, size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex_internal(
      buffer, operand_size_prefix, rex_w, opcode1, has_opcode2, opcode2, reg,
      displacement_offset_out, 0);
}

int binary_emit_mov_reg_rip_mem(BinaryCodeBuffer *buffer,
                                       BinaryGpRegister destination,
                                       size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 0, 1, 0x8B, 0, 0,
                                            destination,
                                            displacement_offset_out);
}

int binary_emit_mov_reg32_rip_mem(BinaryCodeBuffer *buffer,
                                         BinaryGpRegister destination,
                                         size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 0, 0, 0x8B, 0, 0,
                                            destination,
                                            displacement_offset_out);
}

int binary_emit_mov_mem_rip_reg8(BinaryCodeBuffer *buffer,
                                         BinaryGpRegister source,
                                         size_t *displacement_offset_out) {
  int force_rex = source >= BINARY_GP_RSP && source <= BINARY_GP_RDI;
  return binary_emit_rip_relative_access_ex_internal(
      buffer, 0, 0, 0x88, 0, 0, source, displacement_offset_out, force_rex);
}

int binary_emit_mov_mem_rip_reg16(BinaryCodeBuffer *buffer,
                                         BinaryGpRegister source,
                                         size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 1, 0, 0x89, 0, 0, source,
                                            displacement_offset_out);
}

int binary_emit_mov_mem_rip_reg32(BinaryCodeBuffer *buffer,
                                         BinaryGpRegister source,
                                         size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 0, 0, 0x89, 0, 0, source,
                                            displacement_offset_out);
}

int binary_emit_mov_mem_rip_reg(BinaryCodeBuffer *buffer,
                                       BinaryGpRegister source,
                                       size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 0, 1, 0x89, 0, 0, source,
                                            displacement_offset_out);
}

int binary_emit_movzx_reg_rip_mem8(BinaryCodeBuffer *buffer,
                                          BinaryGpRegister destination,
                                          size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 0, 0, 0x0F, 1, 0xB6,
                                            destination,
                                            displacement_offset_out);
}

int binary_emit_movzx_reg_rip_mem16(BinaryCodeBuffer *buffer,
                                           BinaryGpRegister destination,
                                           size_t *displacement_offset_out) {
  return binary_emit_rip_relative_access_ex(buffer, 0, 0, 0x0F, 1, 0xB7,
                                            destination,
                                            displacement_offset_out);
}

int binary_emit_test_reg_reg(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, reg >> 3, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x85) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((reg & 7) << 3) | (reg & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_test_reg_imm32(BinaryCodeBuffer *buffer,
                                      BinaryGpRegister reg,
                                      uint32_t immediate) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0xF7) ||
      !binary_code_buffer_append_u8(buffer,
                                    (unsigned char)(0xC0 | (reg & 7))) ||
      !binary_code_buffer_append_u32(buffer, immediate)) {
    return 0;
  }

  return 1;
}

int binary_emit_cmp_reg_reg(BinaryCodeBuffer *buffer,
                                   BinaryGpRegister lhs,
                                   BinaryGpRegister rhs) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, rhs >> 3, 0, lhs >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x39) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((rhs & 7) << 3) | (lhs & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_cmp_reg_reg32(BinaryCodeBuffer *buffer,
                                     BinaryGpRegister lhs,
                                     BinaryGpRegister rhs) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 0, rhs >> 3, 0, lhs >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x39) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((rhs & 7) << 3) | (lhs & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_cmovcc_reg_reg(BinaryCodeBuffer *buffer,
                                      unsigned char opcode,
                                      BinaryGpRegister destination,
                                      BinaryGpRegister source) {
  if (!buffer || opcode < 0x40 || opcode > 0x4F) {
    return 0;
  }

  return binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) &&
         binary_code_buffer_append_u8(buffer, 0x0F) &&
         binary_code_buffer_append_u8(buffer, opcode) &&
         binary_code_buffer_append_u8(
             buffer,
             (unsigned char)(0xC0 | ((destination & 7) << 3) |
                             (source & 7)));
}

int binary_emit_alu_reg_mem(BinaryCodeBuffer *buffer, unsigned char opcode,
                            BinaryGpRegister destination,
                            BinaryGpRegister base, int displacement,
                            int width) {
  if (!buffer || (width != 4 && width != 8)) {
    return 0;
  }
  return binary_emit_memory_access_ex(buffer, 0, width == 8,
                                      (unsigned char)(opcode | 0x02), 0, 0,
                                      destination, base, displacement);
}

int binary_emit_alu_reg_reg(BinaryCodeBuffer *buffer,
                                   unsigned char opcode,
                                   BinaryGpRegister destination,
                                   BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, source >> 3, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(buffer, opcode) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((source & 7) << 3) | (destination & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_imul_reg_reg32(BinaryCodeBuffer *buffer,
                               BinaryGpRegister destination,
                               BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  return binary_emit_rex(buffer, 0, destination >> 3, 0, source >> 3) &&
         binary_code_buffer_append_u8(buffer, 0x0F) &&
         binary_code_buffer_append_u8(buffer, 0xAF) &&
         binary_code_buffer_append_u8(
             buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                     (source & 7)));
}

int binary_emit_imul_reg_reg_imm32_w32(BinaryCodeBuffer *buffer,
                                       BinaryGpRegister destination,
                                       BinaryGpRegister source,
                                       uint32_t immediate) {
  if (!buffer) {
    return 0;
  }
  return binary_emit_rex(buffer, 0, destination >> 3, 0, source >> 3) &&
         binary_code_buffer_append_u8(buffer, 0x69) &&
         binary_code_buffer_append_u8(
             buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                     (source & 7))) &&
         binary_code_buffer_append_u32(buffer, immediate);
}

int binary_emit_imul_reg_reg(BinaryCodeBuffer *buffer,
                                    BinaryGpRegister destination,
                                    BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xAF) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_immediate_positive_power_of_two_i32(int32_t value,
                                                      unsigned char *shift_out) {
  if (!shift_out || value <= 0 || (value & (value - 1)) != 0) {
    return 0;
  }

  unsigned char shift = 0;
  uint32_t uvalue = (uint32_t)value;
  while (uvalue > 1u) {
    uvalue >>= 1u;
    shift++;
  }
  *shift_out = shift;
  return 1;
}

int binary_emit_imul_reg_reg_small_imm(BinaryCodeBuffer *buffer,
                                              BinaryGpRegister destination,
                                              BinaryGpRegister source,
                                              int32_t immediate) {
  int negate = 0;
  if (immediate < 0) {
    if (immediate == INT32_MIN) {
      return 0;
    }
    negate = 1;
    immediate = -immediate;
  }

  int scale = 0;
  if (immediate == 3) {
    scale = 2;
  } else if (immediate == 5) {
    scale = 4;
  } else if (immediate == 9) {
    scale = 8;
  } else {
    return 0;
  }

  if (!binary_emit_lea_reg_base_index_scale_disp(buffer, destination, source,
                                                 source, scale, 0)) {
    return 0;
  }
  if (negate && !binary_emit_neg_reg(buffer, destination)) {
    return 0;
  }
  return 1;
}

static int emit_mov_w(BinaryCodeBuffer *buffer, BinaryGpRegister dst,
                      BinaryGpRegister src, int w) {
  return w ? binary_emit_mov_reg_reg(buffer, dst, src)
           : binary_emit_mov_reg_reg32(buffer, dst, src);
}

static int emit_neg_w(BinaryCodeBuffer *buffer, BinaryGpRegister reg, int w) {
  return w ? binary_emit_neg_reg(buffer, reg)
           : binary_emit_neg_reg32(buffer, reg);
}

static int emit_shl_w(BinaryCodeBuffer *buffer, BinaryGpRegister reg,
                      unsigned char shift, int w) {
  return w ? binary_emit_shift_reg_imm8(buffer, 4, reg, shift)
           : binary_emit_shift_reg_imm8_32(buffer, 4, reg, shift);
}

static int emit_alu_w(BinaryCodeBuffer *buffer, unsigned char opcode,
                      BinaryGpRegister dst, BinaryGpRegister src, int w) {
  return w ? binary_emit_alu_reg_reg(buffer, opcode, dst, src)
           : binary_emit_alu_reg_reg32(buffer, opcode, dst, src);
}

static int emit_lea_scaled_w(BinaryCodeBuffer *buffer, BinaryGpRegister dst,
                             BinaryGpRegister base, BinaryGpRegister index,
                             int scale, int w) {
  return w ? binary_emit_lea_reg_base_index_scale_disp(buffer, dst, base,
                                                       index, scale, 0)
           : binary_emit_lea32_reg_base_index_scale_disp(buffer, dst, base,
                                                         index, scale, 0);
}

static int emit_imul_small_imm_w(BinaryCodeBuffer *buffer,
                                 BinaryGpRegister destination,
                                 BinaryGpRegister source, int32_t immediate,
                                 int w) {
  if (w) {
    return binary_emit_imul_reg_reg_small_imm(buffer, destination, source,
                                              immediate);
  }
  int negate = 0;
  if (immediate < 0) {
    if (immediate == INT32_MIN) {
      return 0;
    }
    negate = 1;
    immediate = -immediate;
  }
  int scale = 0;
  if (immediate == 3) {
    scale = 2;
  } else if (immediate == 5) {
    scale = 4;
  } else if (immediate == 9) {
    scale = 8;
  } else {
    return 0;
  }
  if (!binary_emit_lea32_reg_base_index_scale_disp(buffer, destination, source,
                                                   source, scale, 0)) {
    return 0;
  }
  if (negate && !binary_emit_neg_reg32(buffer, destination)) {
    return 0;
  }
  return 1;
}

static int binary_emit_imul_imm_scratch_width(BinaryCodeBuffer *buffer,
                                              BinaryGpRegister destination,
                                              BinaryGpRegister source,
                                              uint32_t immediate,
                                              int have_scratch,
                                              BinaryGpRegister scratch,
                                              int w) {
  if (!buffer) {
    return 0;
  }
  int32_t signed_immediate = (int32_t)immediate;
  if (signed_immediate == 0) {
    return binary_emit_xor_reg_reg32(buffer, destination);
  }
  if (signed_immediate == 1) {
    if (!w && destination == source) {
      return binary_emit_movzx_reg_reg32(buffer, destination, source);
    }
    return emit_mov_w(buffer, destination, source, w);
  }
  if (signed_immediate == -1) {
    return emit_mov_w(buffer, destination, source, w) &&
           emit_neg_w(buffer, destination, w);
  }

  unsigned char shift = 0;
  if (binary_immediate_positive_power_of_two_i32(signed_immediate,
                                                 &shift)) {
    return emit_mov_w(buffer, destination, source, w) &&
           emit_shl_w(buffer, destination, shift, w);
  }
  if (signed_immediate != INT32_MIN &&
      binary_immediate_positive_power_of_two_i32(-signed_immediate,
                                                 &shift)) {
    return emit_mov_w(buffer, destination, source, w) &&
           emit_shl_w(buffer, destination, shift, w) &&
           emit_neg_w(buffer, destination, w);
  }
  /* C = 2^k + 1 (3,5,9,17,33,...): source*(2^k+1) = (source<<k) + source. For
   * k<=3 a single LEA does it (scale 2/4/8), valid even when dst==src (LEA reads
   * both inputs before writing). For larger k, mov+shl+add: the mov is
   * register-renamed to zero latency, so the dependency chain is shl+add -- two
   * cycles, shorter than imul's 3-cycle latency and off the single multiply
   * port. The result is bit-identical to imul (both are mod 2^width; the narrow
   * canonicalization that follows is unchanged). mov+shl+add needs dst != src so
   * `source` survives the final add. */
  if (signed_immediate >= 3 &&
      binary_immediate_positive_power_of_two_i32(signed_immediate - 1, &shift)) {
    if (shift >= 1 && shift <= 3 && source != BINARY_GP_RSP) {
      return emit_lea_scaled_w(buffer, destination, source, source, 1 << shift, w);
    }
    if (destination != source) {
      return emit_mov_w(buffer, destination, source, w) &&
             emit_shl_w(buffer, destination, shift, w) &&
             emit_alu_w(buffer, 0x01, destination, source, w);
    }
    /* dst == src: shift a COPY instead, so the original survives the add. The
     * chain is still shl+add, and the caller's scratch is free here. Without
     * this an in-place `h = h * 33` -- the shape a hash loop's recurrence takes
     * once its temp is folded into its destination -- falls back to imul and
     * pays a 3-cycle loop-carried latency instead of 2. */
    if (have_scratch && scratch != destination && scratch != BINARY_GP_RSP) {
      return emit_mov_w(buffer, scratch, source, w) &&
             emit_shl_w(buffer, scratch, shift, w) &&
             emit_alu_w(buffer, 0x01, destination, scratch, w);
    }
  }
  /* C = 2^k - 1 (7,15,31,63,...): source*(2^k-1) = (source<<k) - source. Same
   * rationale and constraints (C==3 is already handled above as 2^1+1).
   *
   * There is no dst==src form here. SUB does not commute, so the scratch would
   * have to hold the result and be moved back -- four instructions to save one
   * cycle of latency, past the point where the trade is worth making. */
  if (signed_immediate >= 7 && destination != source &&
      binary_immediate_positive_power_of_two_i32(signed_immediate + 1,
                                                 &shift)) {
    return emit_mov_w(buffer, destination, source, w) &&
           emit_shl_w(buffer, destination, shift, w) &&
           emit_alu_w(buffer, 0x29, destination, source, w);
  }

  if (emit_imul_small_imm_w(buffer, destination, source, signed_immediate,
                            w)) {
    return 1;
  }

  /* C = (3|5|9) * 2^k (6,10,12,18,20,24,36,40,...): lea [src+src*(C'-1)] then
   * shl by k. LEA reads both inputs before writing, so dst==src is fine; the
   * chain is lea+shl (~1.5 cycles) vs imul's 3-cycle latency, and stays off
   * the multiply port. Negative C appends a neg. */
  {
    int32_t magnitude = signed_immediate;
    int negate = 0;
    if (magnitude < 0 && magnitude != INT32_MIN) {
      magnitude = -magnitude;
      negate = 1;
    }
    if (magnitude > 0 && source != BINARY_GP_RSP) {
      unsigned char k = 0;
      int32_t odd = magnitude;
      while ((odd & 1) == 0) {
        odd >>= 1;
        k++;
      }
      if (k > 0 && (odd == 3 || odd == 5 || odd == 9)) {
        if (emit_lea_scaled_w(buffer, destination, source, source, (int)(odd - 1),
                w) &&
            emit_shl_w(buffer, destination, k, w) &&
            (!negate || emit_neg_w(buffer, destination, w))) {
          return 1;
        }
        return 0;
      }
    }
  }

  unsigned char opcode = signed_immediate >= INT8_MIN &&
                                 signed_immediate <= INT8_MAX
                             ? 0x6B
                             : 0x69;
  if (!binary_emit_rex(buffer, w, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, opcode) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7))) ||
      (opcode == 0x6B
           ? !binary_code_buffer_append_u8(
                 buffer, (unsigned char)(int8_t)signed_immediate)
           : !binary_code_buffer_append_u32(buffer, immediate))) {
    return 0;
  }

  return 1;
}

int binary_emit_imul_reg_reg_imm32_scratch(BinaryCodeBuffer *buffer,
                                           BinaryGpRegister destination,
                                           BinaryGpRegister source,
                                           uint32_t immediate,
                                           int have_scratch,
                                           BinaryGpRegister scratch) {
  return binary_emit_imul_imm_scratch_width(buffer, destination, source,
                                            immediate, have_scratch, scratch,
                                            1);
}

int binary_emit_imul_reg_reg_imm32_scratch_w32(BinaryCodeBuffer *buffer,
                                               BinaryGpRegister destination,
                                               BinaryGpRegister source,
                                               uint32_t immediate,
                                               int have_scratch,
                                               BinaryGpRegister scratch) {
  return binary_emit_imul_imm_scratch_width(buffer, destination, source,
                                            immediate, have_scratch, scratch,
                                            0);
}

int binary_emit_imul_reg_reg_imm32(BinaryCodeBuffer *buffer,
                                          BinaryGpRegister destination,
                                          BinaryGpRegister source,
                                          uint32_t immediate) {
  return binary_emit_imul_reg_reg_imm32_scratch(buffer, destination, source,
                                                immediate, 0, BINARY_GP_RAX);
}

int binary_emit_unary_reg(BinaryCodeBuffer *buffer,
                                 unsigned char subopcode,
                                 BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0xF7) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_neg_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  return binary_emit_unary_reg(buffer, 3, reg);
}

int binary_emit_not_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  return binary_emit_unary_reg(buffer, 2, reg);
}

int binary_emit_idiv_reg(BinaryCodeBuffer *buffer,
                                BinaryGpRegister divisor) {
  return binary_emit_unary_reg(buffer, 7, divisor);
}

/* Signed division with the one case x86 traps on folded out. IDIV raises #DE
 * when the quotient does not fit, which for a 64-bit divide means exactly
 * INT64_MIN / -1. The narrower widths never reach it (they divide sign-extended
 * at 64 bits and truncate after), so trapping there made one width of the same
 * expression kill the process. Dividing by -1 is a negation and leaves no
 * remainder, and negation wraps INT64_MIN to itself, so the guarded path is the
 * wrapping answer the other widths already give. Division by zero still traps.
 * RAX = dividend on entry; RAX = quotient, RDX = remainder on exit. */
int binary_emit_idiv_wrapping(BinaryCodeBuffer *buffer,
                              BinaryGpRegister divisor) {
  size_t to_wrap = 0;
  size_t to_done = 0;
  if (!binary_emit_cmp_reg_imm32(buffer, divisor, 0xFFFFFFFFu) ||
      !wcs_jcc(buffer, 0x84 /* je */, &to_wrap) || !binary_emit_cqo(buffer) ||
      !binary_emit_idiv_reg(buffer, divisor) ||
      !wcs_jcc(buffer, 0 /* jmp */, &to_done)) {
    return 0;
  }
  if (!wcs_patch_here(buffer, to_wrap) ||
      !binary_emit_neg_reg(buffer, BINARY_GP_RAX) ||
      !binary_emit_xor_reg_reg32(buffer, BINARY_GP_RDX)) {
    return 0;
  }
  return wcs_patch_here(buffer, to_done);
}

/* Unsigned one-operand DIV (F7 /6): RAX = RDX:RAX / src, RDX = remainder.
 * Caller must zero RDX (xor edx,edx) first. */
int binary_emit_div_reg(BinaryCodeBuffer *buffer, BinaryGpRegister divisor) {
  return binary_emit_unary_reg(buffer, 6, divisor);
}

int binary_emit_mul_reg(BinaryCodeBuffer *buffer, BinaryGpRegister src) {
  return binary_emit_unary_reg(buffer, 4, src);
}

int binary_emit_imul_reg(BinaryCodeBuffer *buffer, BinaryGpRegister src) {
  return binary_emit_unary_reg(buffer, 5, src);
}

int binary_emit_shift_reg_cl(BinaryCodeBuffer *buffer,
                                    unsigned char subopcode,
                                    BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0xD3) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_shift_reg_imm8(BinaryCodeBuffer *buffer,
                                      unsigned char subopcode,
                                      BinaryGpRegister reg,
                                      unsigned char immediate) {
  if (!buffer) {
    return 0;
  }
  if (immediate == 0) {
    return 1;
  }
  if (immediate == 1) {
    if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
        !binary_code_buffer_append_u8(buffer, 0xD1) ||
        !binary_code_buffer_append_u8(
            buffer,
            (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7)))) {
      return 0;
    }
    return 1;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, reg >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0xC1) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((subopcode & 7) << 3) | (reg & 7))) ||
      !binary_code_buffer_append_u8(buffer, immediate)) {
    return 0;
  }

  return 1;
}

int binary_emit_cqo(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, 0) ||
      !binary_code_buffer_append_u8(buffer, 0x99)) {
    return 0;
  }

  return 1;
}

int binary_emit_setcc_al(BinaryCodeBuffer *buffer,
                                unsigned char condition_opcode) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, condition_opcode) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_movzx_eax_al(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xB6) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_movzx_eax_ax(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xB7) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_movsx_rax_al(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, 0) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xBE) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_movsx_rax_ax(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, 0) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xBF) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_movsx_reg_reg8(BinaryCodeBuffer *buffer,
                                      BinaryGpRegister destination,
                                      BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xBE) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((destination & 7) << 3) | (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_movsx_reg_reg16(BinaryCodeBuffer *buffer,
                                       BinaryGpRegister destination,
                                       BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xBF) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((destination & 7) << 3) | (source & 7)))) {
    return 0;
  }

  return 1;
}

/* dst(64) <- zero-extend(low byte of source). 0F B6 /r with REX.W. */
int binary_emit_movzx_reg_reg8(BinaryCodeBuffer *buffer,
                               BinaryGpRegister destination,
                               BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xB6) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((destination & 7) << 3) | (source & 7)))) {
    return 0;
  }
  return 1;
}

/* dst(64) <- zero-extend(low word of source). 0F B7 /r with REX.W. */
int binary_emit_movzx_reg_reg16(BinaryCodeBuffer *buffer,
                                BinaryGpRegister destination,
                                BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }
  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0xB7) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((destination & 7) << 3) | (source & 7)))) {
    return 0;
  }
  return 1;
}

int binary_emit_movsxd_rax_eax(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, 0, 0, 0) ||
      !binary_code_buffer_append_u8(buffer, 0x63) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_movsxd_reg_reg32(BinaryCodeBuffer *buffer,
                                        BinaryGpRegister destination,
                                        BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x63) ||
      !binary_code_buffer_append_u8(
          buffer,
          (unsigned char)(0xC0 | ((destination & 7) << 3) | (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_mov_eax_eax(BinaryCodeBuffer *buffer) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x89) ||
      !binary_code_buffer_append_u8(buffer, 0xC0)) {
    return 0;
  }

  return 1;
}

int binary_emit_setcc_reg8(BinaryCodeBuffer *buffer,
                                  unsigned char condition_opcode,
                                  BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }

  if ((int)reg >= 4) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, condition_opcode) ||
      !binary_code_buffer_append_u8(buffer, (unsigned char)(0xC0 | (reg & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_alu_reg8_reg8(BinaryCodeBuffer *buffer,
                                     unsigned char opcode,
                                     BinaryGpRegister destination,
                                     BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if ((int)destination >= 4 || (int)source >= 4) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, opcode) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((source & 7) << 3) |
                                  (destination & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_sse_reg_reg(BinaryCodeBuffer *buffer,
                                   unsigned char mandatory_prefix,
                                   int rex_w, unsigned char opcode1,
                                   unsigned char opcode2,
                                   BinaryXmmRegister destination,
                                   BinaryXmmRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, mandatory_prefix) ||
      !binary_emit_rex(buffer, rex_w, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, opcode1) ||
      !binary_code_buffer_append_u8(buffer, opcode2) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_movq_xmm_reg(BinaryCodeBuffer *buffer,
                                    BinaryXmmRegister destination,
                                    BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x66) ||
      !binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x6E) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_movq_reg_xmm(BinaryCodeBuffer *buffer,
                                    BinaryGpRegister destination,
                                    BinaryXmmRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x66) ||
      !binary_emit_rex(buffer, 1, source >> 3, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x7E) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((source & 7) << 3) |
                                  (destination & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_pxor_xmm_xmm(BinaryCodeBuffer *buffer,
                                    BinaryXmmRegister destination,
                                    BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0x66, 0, 0x0F, 0xEF, destination,
                                 source);
}

/* Bitwise XOR over the whole register, in the float domain. Negation is a
 * sign-bit flip rather than `0 - x`, because IEEE 754 says -(+0.0) is -0.0
 * while 0.0 - 0.0 is +0.0, and because a subtract cannot flip the sign of a
 * NaN. Used at both widths: the mask for a float32 has zeros above bit 31, so
 * the wider operation touches nothing that matters. */
int binary_emit_xorpd_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0x66, 0, 0x0F, 0x57, destination,
                                 source);
}

int binary_emit_addsd_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF2, 0, 0x0F, 0x58, destination,
                                 source);
}

int binary_emit_subsd_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF2, 0, 0x0F, 0x5C, destination,
                                 source);
}

int binary_emit_mulsd_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF2, 0, 0x0F, 0x59, destination,
                                 source);
}

int binary_emit_divsd_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF2, 0, 0x0F, 0x5E, destination,
                                 source);
}

int binary_emit_ucomisd_xmm_xmm(BinaryCodeBuffer *buffer,
                                       BinaryXmmRegister lhs,
                                       BinaryXmmRegister rhs) {
  return binary_emit_sse_reg_reg(buffer, 0x66, 0, 0x0F, 0x2E, lhs, rhs);
}

int binary_emit_cvttsd2si_reg_xmm(BinaryCodeBuffer *buffer,
                                         BinaryGpRegister destination,
                                         BinaryXmmRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xF2) ||
      !binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x2C) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_cvtsi2sd_xmm_reg(BinaryCodeBuffer *buffer,
                                        BinaryXmmRegister destination,
                                        BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xF2) ||
      !binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x2A) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

/* ---- Single-precision (float32) SSE encoders ----
 * These mirror the double-precision encoders above but use the F3 scalar-
 * single prefix / 32-bit operand forms. They exist so float32 values are
 * computed and converted at single precision instead of being silently
 * widened to double (which corrupts struct layout and ABI). */

/* movd xmm, r32 : 66 0F 6E /r  (no REX.W -> 32-bit GP source) */
int binary_emit_movd_xmm_reg(BinaryCodeBuffer *buffer,
                                    BinaryXmmRegister destination,
                                    BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x66) ||
      !binary_emit_rex(buffer, 0, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x6E) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

/* movd r32, xmm : 66 0F 7E /r  (no REX.W -> 32-bit GP destination) */
int binary_emit_movd_reg_xmm(BinaryCodeBuffer *buffer,
                                    BinaryGpRegister destination,
                                    BinaryXmmRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x66) ||
      !binary_emit_rex(buffer, 0, source >> 3, 0, destination >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x7E) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((source & 7) << 3) |
                                  (destination & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_addss_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF3, 0, 0x0F, 0x58, destination,
                                 source);
}

int binary_emit_subss_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF3, 0, 0x0F, 0x5C, destination,
                                 source);
}

int binary_emit_mulss_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF3, 0, 0x0F, 0x59, destination,
                                 source);
}

int binary_emit_divss_xmm_xmm(BinaryCodeBuffer *buffer,
                                     BinaryXmmRegister destination,
                                     BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF3, 0, 0x0F, 0x5E, destination,
                                 source);
}

/* ucomiss xmm, xmm : NP 0F 2E /r  (no mandatory prefix, so cannot use
 * binary_emit_sse_reg_reg which always emits one). */
int binary_emit_ucomiss_xmm_xmm(BinaryCodeBuffer *buffer,
                                       BinaryXmmRegister lhs,
                                       BinaryXmmRegister rhs) {
  if (!buffer) {
    return 0;
  }

  if (!binary_emit_rex(buffer, 0, lhs >> 3, 0, rhs >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x2E) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((lhs & 7) << 3) | (rhs & 7)))) {
    return 0;
  }

  return 1;
}

/* cvttss2si r64, xmm : F3 REX.W 0F 2C /r  (truncating float32 -> int64) */
int binary_emit_cvttss2si_reg_xmm(BinaryCodeBuffer *buffer,
                                         BinaryGpRegister destination,
                                         BinaryXmmRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xF3) ||
      !binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x2C) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

/* cvtsi2ss xmm, r64 : F3 REX.W 0F 2A /r  (int64 -> float32) */
int binary_emit_cvtsi2ss_xmm_reg(BinaryCodeBuffer *buffer,
                                        BinaryXmmRegister destination,
                                        BinaryGpRegister source) {
  if (!buffer) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xF3) ||
      !binary_emit_rex(buffer, 1, destination >> 3, 0, source >> 3) ||
      !binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, 0x2A) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xC0 | ((destination & 7) << 3) |
                                  (source & 7)))) {
    return 0;
  }

  return 1;
}

/* cvtss2sd xmm, xmm : F3 0F 5A /r  (widen float32 -> float64) */
int binary_emit_cvtss2sd_xmm_xmm(BinaryCodeBuffer *buffer,
                                        BinaryXmmRegister destination,
                                        BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF3, 0, 0x0F, 0x5A, destination,
                                 source);
}

/* cvtsd2ss xmm, xmm : F2 0F 5A /r  (narrow float64 -> float32) */
int binary_emit_cvtsd2ss_xmm_xmm(BinaryCodeBuffer *buffer,
                                        BinaryXmmRegister destination,
                                        BinaryXmmRegister source) {
  return binary_emit_sse_reg_reg(buffer, 0xF2, 0, 0x0F, 0x5A, destination,
                                 source);
}

int binary_emit_call_placeholder(BinaryCodeBuffer *buffer,
                                        size_t *displacement_offset_out) {
  if (!buffer || !displacement_offset_out) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xE8)) {
    return 0;
  }

  *displacement_offset_out = buffer->size;
  return binary_code_buffer_append_u32(buffer, 0);
}

int binary_emit_jmp_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }
  if ((int)reg >= 8 && !binary_emit_rex(buffer, 0, 0, 0, 1)) {
    return 0;
  }
  return binary_code_buffer_append_u8(buffer, 0xFF) &&
         binary_code_buffer_append_u8(buffer,
                                      (unsigned char)(0xE0 | (reg & 7)));
}

int binary_emit_call_reg(BinaryCodeBuffer *buffer, BinaryGpRegister reg) {
  if (!buffer) {
    return 0;
  }

  if ((int)reg >= 8 && !binary_emit_rex(buffer, 0, 0, 0, 1)) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xFF) ||
      !binary_code_buffer_append_u8(
          buffer, (unsigned char)(0xD0 | (reg & 7)))) {
    return 0;
  }

  return 1;
}

int binary_emit_jmp_placeholder(BinaryCodeBuffer *buffer,
                                       size_t *displacement_offset_out) {
  if (!buffer || !displacement_offset_out) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0xE9)) {
    return 0;
  }

  *displacement_offset_out = buffer->size;
  return binary_code_buffer_append_u32(buffer, 0);
}

int binary_emit_jcc_placeholder(BinaryCodeBuffer *buffer,
                                       unsigned char condition_opcode,
                                       size_t *displacement_offset_out) {
  if (!buffer || !displacement_offset_out) {
    return 0;
  }

  if (!binary_code_buffer_append_u8(buffer, 0x0F) ||
      !binary_code_buffer_append_u8(buffer, condition_opcode)) {
    return 0;
  }

  *displacement_offset_out = buffer->size;
  return binary_code_buffer_append_u32(buffer, 0);
}

int binary_emit_je_placeholder(BinaryCodeBuffer *buffer,
                                      size_t *displacement_offset_out) {
  return binary_emit_jcc_placeholder(buffer, 0x84, displacement_offset_out);
}

int binary_emit_ret(BinaryCodeBuffer *buffer) {
  return buffer ? binary_code_buffer_append_u8(buffer, 0xC3) : 0;
}

int binary_function_context_patch_rel32(BinaryFunctionContext *context,
                                               size_t displacement_offset,
                                               size_t target_offset) {
  if (!context || !context->code.data ||
      displacement_offset + sizeof(int32_t) > context->code.size) {
    return 0;
  }

  long long delta =
      (long long)target_offset - (long long)(displacement_offset + sizeof(int32_t));
  if (delta < INT32_MIN || delta > INT32_MAX) {
    return 0;
  }

  int32_t displacement = (int32_t)delta;
  memcpy(context->code.data + displacement_offset, &displacement,
         sizeof(displacement));
  return 1;
}
