#include "codegen/binary/internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "codegen/binary/simd_internal.h"
/* ---- SSE2 / scalar byte encoders local to the word-count vectorizer ----
 * Kept self-contained so the (verified) instruction encodings live next to
 * the algorithm that depends on them. xmm regs used are 0..6 and GPRs are
 * rax/rcx/rdx + r8..r11, so REX.R/B handling is explicit where r8..r15 or the
 * SSE high regs would need it (here they do not, but the helpers stay
 * general). All return 1 on success, 0 on OOM. */

/* 66 0F <op> /r: SSE2 packed op, xmm dst, xmm src (regs 0..7). */
int wcs_sse_66(BinaryCodeBuffer *b, unsigned char op,
                      int dst, int src) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* F3 0F 6F /r: movdqu xmm, [rcx]  (mod=00, rm=001=rcx, no disp). */
int wcs_movdqu_xmm_rcx(BinaryCodeBuffer *b, int xmm) {
  return binary_code_buffer_append_u8(b, 0xF3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0x00 | ((xmm & 7) << 3) | 0x01));
}

/* 66 0F 6E /r: movd xmm, r32 (here src is always a low GPR 0..2,8,9). */
int wcs_movd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, xmm >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x6E) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}

/* 66 0F 70 /r ib: pshufd xmm, xmm, imm8. */
int wcs_pshufd(BinaryCodeBuffer *b, int dst, int src,
                      unsigned char imm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x70) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* 66 0F D7 /r: pmovmskb r32, xmm. dst is a GPR (0..15), src xmm 0..7. */
int wcs_pmovmskb(BinaryCodeBuffer *b, int gpr, int xmm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, gpr >> 3, 0, xmm >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xD7) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (xmm & 7)));
}

/* F3 0F B8 /r: popcnt r32, r32. */
int wcs_popcnt(BinaryCodeBuffer *b, int dst, int src) {
  return binary_code_buffer_append_u8(b, 0xF3) &&
         binary_emit_rex(b, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xB8) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* 0F B6 /r: movzx r32, byte [rcx]. */
int wcs_movzx_reg_byte_rcx(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, 0) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xB6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0x00 | ((gpr & 7) << 3) | 0x01));
}

/* C1 /4 ib (shl) or /5 ib (shr): r32, imm8. */
int wcs_shift_reg_imm(BinaryCodeBuffer *b, int gpr, int is_shr,
                             unsigned char imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0xC1) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((is_shr ? 5 : 4) << 3) |
                                (gpr & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* 09 /r: or r32, r32  (dst |= src). */
int wcs_or_reg_reg(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x09) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* 01 /r: add r32, r32 (dst += src). */
int wcs_add_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x01) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* REX.W 29 /r: sub r64, r64 (dst -= src; pointer differences need 64-bit). */
int wcs_sub_reg_reg64(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 1, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x29) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* F7 /2: not r32. */
int wcs_not_reg(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0xF7) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (gpr & 7)));
}

/* 23 /r: and r32, r32 (dst &= src). */
int wcs_and_reg_reg(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x23) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* 89 /r: mov r32, r32 (dst = src). */
int wcs_mov_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x89) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* B8+r id: mov r32, imm32. */
int wcs_mov_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xB8 + (gpr & 7))) &&
         binary_code_buffer_append_u32(b, imm);
}

/* 48 01 /r: add r64, r64 (dst += src). */
int wcs_add_reg_reg64(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 1, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x01) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* 48 83 /0 ib: add r64, imm8 ; or /5 for sub. */
int wcs_addsub_reg_imm8(BinaryCodeBuffer *b, int gpr, int is_sub,
                               unsigned char imm) {
  return binary_emit_rex(b, 1, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x83) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((is_sub ? 5 : 0) << 3) |
                                (gpr & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* 48 83 /7 ib: cmp r64, imm8 (sign-extended). */
int wcs_cmp_reg_imm8(BinaryCodeBuffer *b, int gpr, unsigned char imm) {
  return binary_emit_rex(b, 1, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x83) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (7 << 3) | (gpr & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* 81 /7 id: cmp r32, imm32 (used for the tail byte compares). */
int wcs_cmp_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x81) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (7 << 3) | (gpr & 7))) &&
         binary_code_buffer_append_u32(b, imm);
}

/* 85 /r: test r32, r32. */
int wcs_test_reg_reg32(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x85) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (gpr & 7)));
}

/* 31 /r: xor r32, r32 (zero a reg via self-xor). */
int wcs_xor_self32(BinaryCodeBuffer *b, int gpr) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x31) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (gpr & 7)));
}

/* Emit a near jcc/jmp with a 32-bit rel placeholder; record the offset of the
 * displacement field so it can be patched once the target is known. cc==0
 * means an unconditional jmp (E9), otherwise 0F 8x. */
int wcs_jcc(BinaryCodeBuffer *b, unsigned char cc, size_t *disp_off) {
  if (cc == 0) {
    if (!binary_code_buffer_append_u8(b, 0xE9)) return 0;
  } else {
    if (!binary_code_buffer_append_u8(b, 0x0F) ||
        !binary_code_buffer_append_u8(b, cc))
      return 0;
  }
  *disp_off = b->size;
  return binary_code_buffer_append_u32(b, 0);
}

/* Patch a rel32 placeholder so it jumps to the current end of the buffer. */
int wcs_patch_here(BinaryCodeBuffer *b, size_t disp_off) {
  long long delta =
      (long long)b->size - (long long)(disp_off + 4);
  if (delta < INT32_MIN || delta > INT32_MAX) return 0;
  int32_t d = (int32_t)delta;
  memcpy(b->data + disp_off, &d, 4);
  return 1;
}

/* Patch a rel32 placeholder to jump backward to a recorded target offset. */
int wcs_patch_to(BinaryCodeBuffer *b, size_t disp_off,
                        size_t target) {
  long long delta = (long long)target - (long long)(disp_off + 4);
  if (delta < INT32_MIN || delta > INT32_MAX) return 0;
  int32_t d = (int32_t)delta;
  memcpy(b->data + disp_off, &d, 4);
  return 1;
}

/* 39 /r: cmp r32, r32 */
int wcs_cmp_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x39) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* movdqu xmm, [gpr+0] */
int wcs_movdqu_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr) {
  return binary_code_buffer_append_u8(b, 0xF3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0x00 | ((xmm & 7) << 3) | (gpr & 7)));
}

int simd_emit_prefixed_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char prefix,
                                    unsigned char opcode, int xmm, int gpr,
                                    int displacement) {
  if (!b) {
    return 0;
  }

  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char rm = (unsigned char)(gpr & 7);
  int needs_sib = rm == (BINARY_GP_RSP & 7);
  int needs_base_disp = rm == (BINARY_GP_RBP & 7);
  unsigned char mod =
      (displacement == 0 && !needs_base_disp) ? 0 : (use_disp8 ? 1 : 2);
  unsigned char modrm =
      (unsigned char)((mod << 6) | ((xmm & 7) << 3) |
                      (needs_sib ? 4 : rm));

  if (!binary_code_buffer_append_u8(b, prefix) ||
      !binary_emit_rex(b, 0, xmm >> 3, 0, gpr >> 3) ||
      !binary_code_buffer_append_u8(b, 0x0F) ||
      !binary_code_buffer_append_u8(b, opcode) ||
      !binary_code_buffer_append_u8(b, modrm)) {
    return 0;
  }
  if (needs_sib) {
    unsigned char sib = (unsigned char)((0 << 6) | (4 << 3) | (gpr & 7));
    if (!binary_code_buffer_append_u8(b, sib)) {
      return 0;
    }
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(b, (unsigned char)(int8_t)displacement);
  }
  if (mod == 2) {
    return binary_code_buffer_append_u32(b, (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int simd_emit_xmm_mem_disp(BinaryCodeBuffer *b, unsigned char opcode,
                                  int xmm, int gpr, int displacement) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF3, opcode, xmm, gpr,
                                         displacement);
}

int simd_movdqu_xmm_mem_disp(BinaryCodeBuffer *b, int xmm, int gpr,
                                    int displacement) {
  return simd_emit_xmm_mem_disp(b, 0x6F, xmm, gpr, displacement);
}

int simd_movdqu_mem_xmm_disp(BinaryCodeBuffer *b, int gpr,
                                    int displacement, int xmm) {
  return simd_emit_xmm_mem_disp(b, 0x7F, xmm, gpr, displacement);
}

int simd_movd_xmm_mem32_disp(BinaryCodeBuffer *b, int xmm, int gpr,
                             int displacement) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0x66, 0x6E, xmm, gpr,
                                         displacement);
}

/* 66 0F 7E /r - movd r32, xmm */
int wcs_movd_reg_xmm(BinaryCodeBuffer *b, int gpr, int xmm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, xmm >> 3, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x7E) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}

/* 29 /r: sub r32, r32 (dst -= src) */
int wcs_sub_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x29) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* 66 0F F4 /r: pmuludq xmm, xmm */
int wcs_pmuludq(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66(b, 0xF4, dst, src);
}

/* 66 0F FE /r: paddd xmm, xmm */
int wcs_paddd(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66(b, 0xFE, dst, src);
}

/* 66 0F 38 xx /r: SSE4.1 packed xmm ops. */
int wcs_sse_66_38(BinaryCodeBuffer *b, unsigned char op, int dst,
                         int src) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x38) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_pmulld(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66_38(b, 0x40, dst, src);
}

/* 66 0F 38 39 /r: pminsd xmm, xmm (SSE4.1) */
int wcs_pminsd(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66_38(b, 0x39, dst, src);
}

/* 66 0F 38 3D /r: pmaxsd xmm, xmm (SSE4.1) */
int wcs_pmaxsd(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66_38(b, 0x3D, dst, src);
}

int wcs_broadcast_i32_to_xmm(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_movd_xmm_reg(b, xmm, gpr) && wcs_pshufd(b, xmm, xmm, 0x00);
}

int wcs_accumulate_xmm0_i32_to_rax(BinaryCodeBuffer *b) {
  if (!wcs_sse_66(b, 0x6F, 1, 0) || !wcs_pshufd(b, 1, 0, 0xEE) ||
      !wcs_paddd(b, 0, 1) || !wcs_pshufd(b, 1, 0, 0x01) ||
      !wcs_paddd(b, 0, 1) || !wcs_movd_reg_xmm(b, BINARY_GP_R10, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }
  return 1;
}

int wcs_fold_xmm6_i32_sum_to_rax(BinaryCodeBuffer *b) {
  if (!wcs_sse_66(b, 0x6F, 0, 6) || !wcs_accumulate_xmm0_i32_to_rax(b)) {
    return 0;
  }
  return 1;
}

/* 66 0F 73 /2 ib: psrlq xmm, imm8 */
int wcs_psrlq_imm(BinaryCodeBuffer *b, int xmm, unsigned char imm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x73) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (xmm & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* Fixed 32x32 int32 matrix multiply, SSE2 4-column kernel, N=32. */
/* 66 0F 73 /3 ib - psrldq xmm, imm8 */
int wcs_psrldq_imm(BinaryCodeBuffer *b, int xmm, unsigned char imm) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x73) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (3 << 3) | (xmm & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* 66 0F D4 /r - paddq xmm, xmm */
int wcs_paddq(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_sse_66(b, 0xD4, dst, src);
}

/* 66 0F 38 28 /r - pmuldq xmm, xmm (signed dword -> qword). */
int wcs_pmuldq(BinaryCodeBuffer *b, int dst, int src) {
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x38) &&
         binary_code_buffer_append_u8(b, 0x28) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_vex3(BinaryCodeBuffer *b, int map, int pp, int len256, int w,
                    int reg, int rm, int vvvv) {
  unsigned char b2 = (unsigned char)((((~(reg >> 3)) & 1) << 7) |
                                     (1 << 6) |
                                     (((~(rm >> 3)) & 1) << 5) |
                                     (map & 0x1F));
  unsigned char b3 = (unsigned char)(((w & 1) << 7) |
                                     (((~vvvv) & 0x0F) << 3) |
                                     ((len256 & 1) << 2) | (pp & 3));
  return binary_code_buffer_append_u8(b, 0xC4) &&
         binary_code_buffer_append_u8(b, b2) &&
         binary_code_buffer_append_u8(b, b3);
}

int wcs_avx_modrm_mem_disp(BinaryCodeBuffer *b, int reg, int base,
                                  int displacement) {
  int use_disp8 = displacement >= -128 && displacement <= 127;
  unsigned char base_low = (unsigned char)(base & 7);
  unsigned char mod = 0;
  unsigned char rm = base_low;
  if (displacement != 0 || base_low == 5) {
    mod = use_disp8 ? 1 : 2;
  }
  if (base_low == 4) {
    rm = 4;
  }
  if (!binary_code_buffer_append_u8(
          b, (unsigned char)((mod << 6) | ((reg & 7) << 3) | rm))) {
    return 0;
  }
  if (base_low == 4 &&
      !binary_code_buffer_append_u8(
          b, (unsigned char)((0 << 6) | (4 << 3) | base_low))) {
    return 0;
  }
  if (mod == 1) {
    return binary_code_buffer_append_u8(b,
                                        (unsigned char)(int8_t)displacement);
  }
  if (mod == 2 || (mod == 0 && base_low == 5)) {
    return binary_code_buffer_append_u32(b, (uint32_t)(int32_t)displacement);
  }
  return 1;
}

int wcs_avx_vpmovsxdq_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                     int disp) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x25) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}

/* vmovups [base+disp], ymm (store) - VEX.256.0F.WIG 11 /r. */
int wcs_avx_vmovups_mem_ymm(BinaryCodeBuffer *b, int base, int disp,
                                   int src) {
  return wcs_vex3(b, 1, 0, 1, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x11) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}

/* vbroadcastsd ymm, xmm - VEX.256.66.0F38.W0 19 /r (AVX2). */
int wcs_avx_vbroadcastsd_ymm_xmm(BinaryCodeBuffer *b, int dst,
                                        int src_xmm) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x19) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src_xmm & 7)));
}

int wcs_avx_vmovdqu_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                   int displacement) {
  return wcs_vex3(b, 1, 2, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

int wcs_avx_vpaddq_ymm(BinaryCodeBuffer *b, int dst, int src1,
                              int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xD4) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpxor_ymm(BinaryCodeBuffer *b, int dst, int src1,
                             int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xEF) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

/* vpsadbw ymm_dst, ymm_src1, ymm_src2, VEX.256.66.0F.WIG F6 /r. Sum of
 * absolute byte differences; against a zeroed src2 it yields, per 64-bit lane,
 * the unsigned sum of that lane's 8 bytes (0..2040) in the low word. */
int wcs_avx_vpsadbw_ymm(BinaryCodeBuffer *b, int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xF6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

/* ---- VEX.128 packed-byte/word ops for the uint8 element-wise map kernel ---- */
/* All are VEX.128.66.0F.WIG <op> /r, reg-reg form: dst = src1 <op> src2, with
 * vvvv=src1 and ModRM.rm=src2. */
static int wcs_avx128_66_0f_rr(BinaryCodeBuffer *b, unsigned char opcode,
                               int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, opcode) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpaddb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xFC, d, s1, s2);
}
int wcs_avx_vpsubb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xF8, d, s1, s2);
}
int wcs_avx_vpand_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xDB, d, s1, s2);
}
int wcs_avx_vpor_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xEB, d, s1, s2);
}
int wcs_avx_vpxor_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xEF, d, s1, s2);
}
int wcs_avx_vpmullw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xD5, d, s1, s2);
}
int wcs_avx_vpunpcklbw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0x60, d, s1, s2);
}
int wcs_avx_vpunpckhbw_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0x68, d, s1, s2);
}
int wcs_avx_vpackuswb_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0x67, d, s1, s2);
}
int wcs_avx_vpaddd_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xFE, d, s1, s2);
}
int wcs_avx_vpaddq_xmm(BinaryCodeBuffer *b, int d, int s1, int s2) {
  return wcs_avx128_66_0f_rr(b, 0xD4, d, s1, s2);
}
/* vpshufd xmm, xmm, imm8, VEX.128.66.0F.WIG 70 /r ib. */
int wcs_avx_vpshufd_xmm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x70) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}
/* vpslldq xmm_dst, xmm_src, imm8 (byte shift left), VEX.128.66.0F.WIG 73 /7
 * ib. The destination rides in vvvv; ModRM.reg carries the /7 subopcode. */
int wcs_avx_vpslldq_xmm(BinaryCodeBuffer *b, int dst, int src,
                        unsigned char imm) {
  return wcs_vex3(b, 1, 1, 0, 0, 7, src, dst) &&
         binary_code_buffer_append_u8(b, 0x73) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (7 << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* vmovdqu xmm, [base+disp], VEX.128.F3.0F.WIG 6F /r. */
int wcs_avx_vmovdqu_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 2, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x6F) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}
/* vmovdqu [base+disp], xmm, VEX.128.F3.0F.WIG 7F /r. */
int wcs_avx_vmovdqu_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 2, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x7F) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}

int wcs_avx_vpmuldq_ymm(BinaryCodeBuffer *b, int dst, int src1,
                               int src2) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0x28) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vpsrlq_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                                  unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 2, dst, src) &&
         binary_code_buffer_append_u8(b, 0x73) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vextracti128(BinaryCodeBuffer *b, int dst_xmm, int src_ymm,
                                unsigned char lane) {
  return wcs_vex3(b, 3, 1, 1, 0, src_ymm, dst_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x39) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src_ymm & 7) << 3) |
                                (dst_xmm & 7))) &&
         binary_code_buffer_append_u8(b, lane);
}

int wcs_avx_vzeroupper(BinaryCodeBuffer *b) {
  return binary_code_buffer_append_u8(b, 0xC5) &&
         binary_code_buffer_append_u8(b, 0xF8) &&
         binary_code_buffer_append_u8(b, 0x77);
}

/* ---- packed / scalar floating-point encoders for the float vectorizers ----
 * These mirror the integer helpers above but operate on IEEE-754 lanes. The
 * 256-bit AVX forms reuse wcs_vex3 + wcs_avx_modrm_mem_disp; the 128-bit SSE
 * forms reuse the generic prefixed encoders. xmm regs used by the float
 * kernels are restricted to 0..5 (all volatile under Win64) so the kernels can
 * be inlined without saving callee-saved xmm6..xmm15. */

/* 0F <op> /r: two-byte-map SSE op with no mandatory prefix (e.g. addps). */
int wcs_sse_0f(BinaryCodeBuffer *b, unsigned char op, int dst, int src) {
  return binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* F2 0F <op> /r: SSE op with the F2 mandatory prefix (e.g. haddps). */
int wcs_sse_f2(BinaryCodeBuffer *b, unsigned char op, int dst, int src) {
  return binary_code_buffer_append_u8(b, 0xF2) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* VEX.256.66.0F.WIG <op> /r, packed-double 3-operand ymm op (dst=src1 OP
 * src2). pp=1 selects the 66 prefix that distinguishes pd from ps. */
int wcs_avx_vpd_ymm(BinaryCodeBuffer *b, unsigned char op, int dst,
                           int src1, int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

/* VEX.256.0F.WIG <op> /r, packed-single 3-operand ymm op (pp=0). */
int wcs_avx_vps_ymm(BinaryCodeBuffer *b, unsigned char op, int dst,
                           int src1, int src2) {
  return wcs_vex3(b, 1, 0, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

int wcs_avx_vaddpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x58, dst, s1, s2);
}
int wcs_avx_vmulpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x59, dst, s1, s2);
}
int wcs_avx_vsubpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5C, dst, s1, s2);
}
int wcs_avx_vdivpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5E, dst, s1, s2);
}
/* MAXPD/MINPD return src2 when either operand is NaN. The min/max reduction
 * kernel relies on that: passing the loaded data as src1 and the running
 * accumulator as src2 reproduces `if (v > m) { m = v; }` exactly, NaN
 * included -- a NaN element loses the comparison and leaves the accumulator
 * alone, in both the scalar loop and the vector one. */
int wcs_avx_vminpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5D, dst, s1, s2);
}
int wcs_avx_vmaxpd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x5F, dst, s1, s2);
}

/* vcvtdq2pd ymm, xmm/m128, VEX.256.F3.0F.WIG E6 /r: 4 int32 -> 4 f64. */
int wcs_avx_vcvtdq2pd_ymm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 2, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0xE6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* vcvttpd2dq xmm, ymm, VEX.256.66.0F.WIG E6 /r: 4 f64 -> 4 int32 (truncate). */
int wcs_avx_vcvttpd2dq_xmm_ymm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0xE6) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* vpmovsxdq ymm, xmm, VEX.256.66.0F38.W0 25 /r: sign-extend 4 int32 -> 4 int64. */
int wcs_avx_vpmovsxdq_ymm_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x25) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* vpunpcklqdq xmm, xmm, xmm/m128, VEX.128.66.0F.WIG 6C /r. Interleaves the low
 * quadwords: dst = { src1.q0, src2.q0 }. Used to assemble the int32 lane vector
 * [0,1,2,3] from two movq halves for the counter-reduction iota. */
int wcs_avx_vpunpcklqdq_xmm(BinaryCodeBuffer *b, int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0x6C) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

/* VEX.LIG.F2.0F scalar-double 3-operand ops (dst = s1 OP s2, low lane). Using
 * these instead of legacy-SSE addsd/etc. inside an AVX loop avoids the AVX<->SSE
 * transition penalty. */
static int wcs_avx_vsd(BinaryCodeBuffer *b, unsigned char op, int dst, int s1,
                       int s2) {
  return wcs_vex3(b, 1, 3, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_avx_vaddsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x58, dst, s1, s2);
}
int wcs_avx_vsubsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x5C, dst, s1, s2);
}
int wcs_avx_vmulsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x59, dst, s1, s2);
}
int wcs_avx_vdivsd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vsd(b, 0x5E, dst, s1, s2);
}
/* vcvtsi2sd xmm_dst, xmm_s1, r64, VEX.LIG.F2.0F.W1 2A /r (W1 = 64-bit source);
 * s1 supplies the merged upper lane (pass dst to self-merge). */
int wcs_avx_vcvtsi2sd(BinaryCodeBuffer *b, int dst, int s1, int gpr) {
  return wcs_vex3(b, 1, 3, 0, 1, dst, gpr, s1) &&
         binary_code_buffer_append_u8(b, 0x2A) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (gpr & 7)));
}
/* vmovsd xmm, [base+disp], VEX.LIG.F2.0F.WIG 10 /r (scalar-double load). */
int wcs_avx_vmovsd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 3, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}
/* vmovsd [base+disp], xmm, VEX.LIG.F2.0F.WIG 11 /r (scalar-double store). */
int wcs_avx_vmovsd_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 3, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x11) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}
/* vunpckhpd xmm, s1, s2, VEX.128.66.0F.WIG 15 /r: dst = { s1.hi, s2.hi }.
 * vunpckhpd dst, a, a moves a's high double into dst's low lane (lane extract). */
int wcs_avx_vunpckhpd_xmm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x15) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

int wcs_avx_vaddps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x58, dst, s1, s2);
}
int wcs_avx_vmulps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x59, dst, s1, s2);
}
int wcs_avx_vsubps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5C, dst, s1, s2);
}
int wcs_avx_vdivps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5E, dst, s1, s2);
}
int wcs_avx_vminps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5D, dst, s1, s2);
}
int wcs_avx_vmaxps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vps_ymm(b, 0x5F, dst, s1, s2);
}

/* vroundps ymm,ymm,imm8, VEX.256.66.0F3A.WIG 08 /r ib. imm bit1:0 select the
 * rounding mode; 1 = round toward -inf (floor), what the exp range-reduction
 * needs. */
int wcs_avx_vroundps_ymm(BinaryCodeBuffer *b, int dst, int src,
                         unsigned char imm) {
  return wcs_vex3(b, 3, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x08) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* vcvttps2dq ymm,ymm, VEX.256.F3.0F.WIG 5B /r. Truncating float32 -> int32. */
int wcs_avx_vcvttps2dq_ymm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 2, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x5B) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* vcvtdq2ps ymm,ymm, VEX.256.0F.WIG 5B /r. int32 -> float32. */
int wcs_avx_vcvtdq2ps_ymm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 1, 0, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x5B) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vcvtph2ps_xmm(BinaryCodeBuffer *b, int dst, int src) {
  return wcs_vex3(b, 2, 1, 0, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x13) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

int wcs_avx_vcvtps2ph_xmm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 3, 1, 0, 0, src, dst, 0) &&
         binary_code_buffer_append_u8(b, 0x1D) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* vpslld ymm,ymm,imm8, VEX.256.66.0F 72 /6 ib. Shift int32 lanes left. Used
 * in-place (dst == src) to build 2^n exponents. */
int wcs_avx_vpslld_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 6, dst, src) &&
         binary_code_buffer_append_u8(b, 0x72) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (6 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* The two right shifts share opcode 0x72 with the left one and differ only in
 * the /digit: /2 shifts zeros in (unsigned), /4 replicates the sign. Which one
 * a `>>` becomes is the shifted expression's own signedness. */
int wcs_avx_vpsrld_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 2, dst, src) &&
         binary_code_buffer_append_u8(b, 0x72) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (2 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

int wcs_avx_vpsrad_ymm_imm(BinaryCodeBuffer *b, int dst, int src,
                           unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, 4, dst, src) &&
         binary_code_buffer_append_u8(b, 0x72) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (4 << 3) | (dst & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* vmovups ymm, [base+disp] (load), VEX.256.0F.WIG 10. A 256-bit load is
 * data-agnostic, so this serves both pd and ps. */
int wcs_avx_vmovups_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                    int disp) {
  return wcs_vex3(b, 1, 0, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x10) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}

/* vextractf128 xmm/m128, ymm, imm8, VEX.256.66.0F3A.W0 19 /r ib. The ymm is
 * the ModRM.reg operand and the xmm destination is ModRM.rm. */
int wcs_avx_vextractf128(BinaryCodeBuffer *b, int dst_xmm, int src_ymm,
                                unsigned char lane) {
  return wcs_vex3(b, 3, 1, 1, 0, src_ymm, dst_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x19) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src_ymm & 7) << 3) | (dst_xmm & 7))) &&
         binary_code_buffer_append_u8(b, lane);
}

/* movsd xmm, [gpr+disp] (F2 0F 10), scalar-double load for the dot/sum tail. */
int wcs_movsd_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF2, 0x10, xmm, gpr, disp);
}
/* movsd [gpr+disp], xmm (F2 0F 11) scalar-double store. */
int wcs_movsd_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF2, 0x11, xmm, gpr, disp);
}
/* movss xmm, [gpr+disp] (F3 0F 10), scalar-single load for the dot/sum tail. */
int wcs_movss_xmm_mem(BinaryCodeBuffer *b, int xmm, int gpr, int disp) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF3, 0x10, xmm, gpr, disp);
}
/* movss [gpr+disp], xmm (F3 0F 11) scalar-single store. */
int wcs_movss_mem_xmm(BinaryCodeBuffer *b, int gpr, int disp, int xmm) {
  return simd_emit_prefixed_xmm_mem_disp(b, 0xF3, 0x11, xmm, gpr, disp);
}

/* Fold the four packed-double partials in ymm2 plus the scalar running total in
 * xmm3 down to a single double, returning its bit pattern in RAX. Consumes the
 * upper ymm lanes (vextractf128 + vzeroupper) before the SSE tail so no AVX
 * state leaks past the kernel. */
int wcs_reduce_pd_acc_to_rax(BinaryCodeBuffer *b) {
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_66(b, 0x58, 2, 0) &&   /* addpd  xmm2, xmm0 */
         wcs_sse_66(b, 0x7C, 2, 2) &&   /* haddpd xmm2, xmm2 -> lane0 = sum */
         binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM2) &&
         binary_emit_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM3);
}

/* Single-precision counterpart: fold eight packed-single partials in ymm2 and
 * the scalar total in xmm3 to one float, bit pattern (zero-extended) in RAX. */
int wcs_reduce_ps_acc_to_rax(BinaryCodeBuffer *b) {
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_0f(b, 0x58, 2, 0) &&   /* addps  xmm2, xmm0 (4 partials) */
         wcs_sse_f2(b, 0x7C, 2, 2) &&   /* haddps xmm2, xmm2 -> 2 partials */
         wcs_sse_f2(b, 0x7C, 2, 2) &&   /* haddps xmm2, xmm2 -> lane0 = sum */
         binary_emit_addss_xmm_xmm(b, BINARY_XMM3, BINARY_XMM2) &&
         wcs_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM3);
}

/* Fold a min/max reduction's packed accumulator in ymm2 to one value in RAX.
 *
 * No scalar partner to merge in: the accumulator was SEEDED with the incoming
 * value broadcast across every lane, so each lane already carries it and the
 * tree fold below reaches the same answer as the scalar loop for any trip
 * count, zero included. The packed form (maxpd, not maxsd) is used all the way
 * down; only lane 0 is read at the end, and the wider op needs no extra
 * encoders. */
int wcs_reduce_pd_minmax_to_rax(BinaryCodeBuffer *b, int is_max) {
  unsigned char op = is_max ? 0x5F : 0x5D;
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_66(b, op, 2, 0) &&      /* xmm2 = 2 lanes */
         wcs_pshufd(b, 0, 2, 0xEE) &&    /* xmm0 = high qword */
         wcs_sse_66(b, op, 2, 0) &&      /* xmm2 lane0 = result */
         binary_emit_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2);
}

int wcs_reduce_ps_minmax_to_rax(BinaryCodeBuffer *b, int is_max) {
  unsigned char op = is_max ? 0x5F : 0x5D;
  return wcs_avx_vextractf128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_sse_0f(b, op, 2, 0) &&      /* xmm2 = 4 lanes */
         wcs_pshufd(b, 0, 2, 0xEE) &&
         wcs_sse_0f(b, op, 2, 0) &&      /* xmm2 = 2 lanes */
         wcs_pshufd(b, 0, 2, 0x01) &&
         wcs_sse_0f(b, op, 2, 0) &&      /* xmm2 lane0 = result */
         wcs_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2);
}

/* int32 twin. The result is sign-extended: the accumulator's 8-byte home holds
 * a canonically extended 32-bit value, and movd zero-extends. */
int wcs_reduce_ymm_i32_minmax_to_rax(BinaryCodeBuffer *b, int is_max) {
  int (*fold)(BinaryCodeBuffer *, int, int) = is_max ? wcs_pmaxsd : wcs_pminsd;
  return wcs_avx_vextracti128(b, 0, 2, 1) && wcs_avx_vzeroupper(b) &&
         fold(b, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0xEE) && fold(b, 2, 0) &&
         wcs_pshufd(b, 0, 2, 0x01) && fold(b, 2, 0) &&
         wcs_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX);
}

/* The reduced lane lands in R9 via movd, which zero-extends; sign-extend it to
 * 64 bits so the REX.W cmp/cmov below compare signed int32 extrema correctly
 * (a negative minimum is otherwise seen as a large positive value). The gpr
 * accumulator is kept sign-extended by its callers. */
int wcs_horizontal_pminsd_to_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_pshufd(b, 1, xmm, 0xEE) &&
         wcs_pminsd(b, xmm, 1) &&
         wcs_pshufd(b, 1, xmm, 0x01) &&
         wcs_pminsd(b, xmm, 1) &&
         wcs_movd_reg_xmm(b, BINARY_GP_R9, xmm) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_R9, gpr) &&
         binary_emit_cmovcc_reg_reg(b, 0x4C /* cmovl */, gpr, BINARY_GP_R9);
}

int wcs_horizontal_pmaxsd_to_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_pshufd(b, 1, xmm, 0xEE) &&
         wcs_pmaxsd(b, xmm, 1) &&
         wcs_pshufd(b, 1, xmm, 0x01) &&
         wcs_pmaxsd(b, xmm, 1) &&
         wcs_movd_reg_xmm(b, BINARY_GP_R9, xmm) &&
         binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_R9, gpr) &&
         binary_emit_cmovcc_reg_reg(b, 0x4F /* cmovg */, gpr, BINARY_GP_R9);
}

/* VEX.NDS.256.66.0F38.W0 <op> /r, packed-int 3-operand ymm op via the 0F38
 * map (dst = src1 OP src2). Used by the AVX2-widened integer kernels. */
int wcs_avx_0f38_ymm(BinaryCodeBuffer *b, unsigned char op, int dst,
                            int src1, int src2) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, op) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}
int wcs_avx_vpminsd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x39, dst, s1, s2);
}
int wcs_avx_vpmaxsd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x3D, dst, s1, s2);
}
int wcs_avx_vpmulld_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x40, dst, s1, s2);
}
/* vpshufd ymm, ymm, imm8, VEX.256.66.0F.WIG 70 /r ib. Shuffles dwords within
 * each 128-bit lane independently (the imm selects per-lane). */
int wcs_avx_vpshufd_ymm(BinaryCodeBuffer *b, int dst, int src,
                               unsigned char imm) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x70) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}
/* vperm2i128 ymm, ymm, ymm, imm8, VEX.256.66.0F3A.W0 46 /r ib. Selects a
 * 128-bit lane from the source pair into each half of the destination. */
int wcs_avx_vperm2i128(BinaryCodeBuffer *b, int dst, int s1, int s2,
                              unsigned char imm) {
  return wcs_vex3(b, 3, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x46) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}
int wcs_avx_vpaddd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  /* paddd lives in the legacy 0F map, not 0F38: VEX.256.66.0F.WIG FE /r. */
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xFE) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
/* vmovdqu [base+disp], ymm (store), VEX.256.F3.0F.WIG 7F /r. */
int wcs_avx_vmovdqu_mem_ymm(BinaryCodeBuffer *b, int base,
                                   int displacement, int src) {
  return wcs_vex3(b, 1, 2, 1, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x7F) &&
         wcs_avx_modrm_mem_disp(b, src, base, displacement);
}
/* vpbroadcastd ymm, xmm, VEX.256.66.0F38.W0 58 /r (AVX2). Splats the low
 * dword of the xmm source across all eight ymm lanes. */
int wcs_avx_vpbroadcastd_ymm(BinaryCodeBuffer *b, int dst, int src_xmm) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x58) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src_xmm & 7)));
}

/* vpbroadcastd ymm, m32, VEX.256.66.0F38.W0 58 /r with a memory operand. Loads
 * 32 bits and broadcasts to all 8 lanes in ONE VEX op (no legacy movd, so no
 * AVX<->SSE transition penalty inside a hot loop). */
int wcs_avx_vpbroadcastd_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                 int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x58) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vbroadcastss ymm, m32, VEX.256.66.0F38.W0 18 /r. Broadcast a 32-bit float
 * (or any 32-bit pattern) from memory to all 8 lanes. The exp kernel keeps its
 * constants in a small stack pool and broadcasts each on use, avoiding a
 * per-iteration GP->XMM round-trip. */
int wcs_avx_vbroadcastss_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                                 int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x18) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vpmaddwd ymm_dst, ymm_src1, ymm_src2, VEX.256.66.0F.WIG F5 /r. Multiplies
 * signed int16 lanes and horizontally adds adjacent pairs into signed int32
 * lanes: dst[j] = src1[2j]*src2[2j] + src1[2j+1]*src2[2j+1]. The int8 dot kernel
 * feeds it sign-extended bytes, so each int32 lane holds two int8 products and
 * cannot overflow (2*127*127 < 2^31). */
int wcs_avx_vpmaddwd_ymm(BinaryCodeBuffer *b, int dst, int src1, int src2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, src2, src1) &&
         binary_code_buffer_append_u8(b, 0xF5) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

/* vpmovsxbw ymm_dst, [mem], VEX.256.66.0F38.WIG 20 /r. Loads 16 bytes and
 * sign-extends each to a 16-bit lane (16 int8 -> 16 int16 in a ymm), fusing the
 * load and widening the int8 dot kernel needs before vpmaddwd. */
int wcs_avx_vpmovsxbw_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x20) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vpmovzxbw ymm_dst, [mem], VEX.256.66.0F38.WIG 30 /r. ZERO-extends each of 16
 * bytes to a 16-bit lane. Mettle's `int8`->`int32` cast zero-extends (the type
 * is byte-unsigned), so the int8 dot kernel uses this form to match the scalar
 * semantics exactly. */
int wcs_avx_vpmovzxbw_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x30) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vpmovzxbd xmm_dst, [mem], VEX.128.66.0F38.WIG 31 /r. Zero-extends 4 bytes to
 * 4 int32 lanes. Used by the int8 SLP-MAC kernel (K=4) to widen b's contiguous
 * bytes to int32 before the int32 multiply/accumulate. */
int wcs_avx_vpmovzxbd_xmm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x31) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vpmovzxbd ymm_dst, [mem], VEX.256.66.0F38.WIG 31 /r. Zero-extends 8 bytes to
 * 8 int32 lanes (the K=8 int8 SLP-MAC form). */
int wcs_avx_vpmovzxbd_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x31) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vpmovsxbd ymm, m64, VEX.256.66.0F38.WIG 21 /r: eight int8 sign-extended
 * into eight int32 lanes. The signed twin of the zero-extending form above;
 * which one a byte kernel uses is the array's declared signedness. */
int wcs_avx_vpmovsxbd_ymm_mem(BinaryCodeBuffer *b, int dst, int base,
                              int displacement) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x21) &&
         wcs_avx_modrm_mem_disp(b, dst, base, displacement);
}

/* vpackusdw ymm, VEX.256.66.0F38.WIG 2B /r: dwords to words, unsigned
 * saturating, within each 128-bit lane. */
int wcs_avx_vpackusdw_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_0f38_ymm(b, 0x2B, dst, s1, s2);
}

/* vpackuswb ymm, VEX.256.66.0F.WIG 67 /r: words to bytes, unsigned
 * saturating, within each 128-bit lane. */
int wcs_avx_vpackuswb_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_avx_vpd_ymm(b, 0x67, dst, s1, s2);
}

/* vpextrb [base+disp], xmm, 0, VEX.128.66.0F3A.W0 14 /r ib. Stores lane 0's
 * low byte and nothing else, which is what a byte kernel's scalar tail needs:
 * a 4-byte store would take three neighbours with it. */
int wcs_avx_vpextrb_mem_xmm(BinaryCodeBuffer *b, int base, int displacement,
                            int xmm) {
  return wcs_vex3(b, 3, 1, 0, 0, xmm, base, 0) &&
         binary_code_buffer_append_u8(b, 0x14) &&
         wcs_avx_modrm_mem_disp(b, xmm, base, displacement) &&
         binary_code_buffer_append_u8(b, 0x00);
}

/* vpermq ymm, ymm, imm8, VEX.256.66.0F3A.W1 00 /r ib. The one op here that
 * crosses the 128-bit lane boundary, which is what a byte kernel needs: the
 * in-lane packs leave the two halves of a result eight lanes apart. */
int wcs_avx_vpermq_ymm(BinaryCodeBuffer *b, int dst, int src,
                       unsigned char imm) {
  return wcs_vex3(b, 3, 1, 1, 1, dst, src, 0) &&
         binary_code_buffer_append_u8(b, 0x00) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7))) &&
         binary_code_buffer_append_u8(b, imm);
}

/* vmovd xmm, r/m32, VEX.128.66.0F.W0 6E /r. The VEX form is mandatory inside
 * AVX hot loops: a legacy (66 0F 6E) movd next to VEX-256 ops makes Golden Cove
 * P-cores take the AVX<->SSE transition penalty (upper-YMM save/restore) every
 * iteration, a ~30x slowdown that Gracemont E-cores don't exhibit, so it hides
 * behind hybrid-core scheduling. Keep every in-loop xmm<-gpr move VEX-encoded. */
int wcs_avx_vmovd_xmm_reg(BinaryCodeBuffer *b, int xmm, int gpr) {
  return wcs_vex3(b, 1, 1, 0, 0, xmm, gpr, 0) &&
         binary_code_buffer_append_u8(b, 0x6E) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
}

/* vmovd xmm, m32, VEX.128.66.0F.W0 6E /r (memory form). Loads exactly 4 bytes
 * (no over-read past an array end) and zeroes bits 32..255, which the int
 * vloop kernel's scalar tail relies on: every tail value carries zeros in
 * lanes 1..7 so full-width integer ops stay exact in lane 0. */
int wcs_avx_vmovd_xmm_mem(BinaryCodeBuffer *b, int dst, int base, int disp) {
  return wcs_vex3(b, 1, 1, 0, 0, dst, base, 0) &&
         binary_code_buffer_append_u8(b, 0x6E) &&
         wcs_avx_modrm_mem_disp(b, dst, base, disp);
}

/* vmovd m32, xmm, VEX.128.66.0F.W0 7E /r. Stores the low 4 bytes. */
int wcs_avx_vmovd_mem_xmm(BinaryCodeBuffer *b, int base, int disp, int src) {
  return wcs_vex3(b, 1, 1, 0, 0, src, base, 0) &&
         binary_code_buffer_append_u8(b, 0x7E) &&
         wcs_avx_modrm_mem_disp(b, src, base, disp);
}

/* vpsubd ymm_dst, ymm_src1, ymm_src2, VEX.256.66.0F.WIG FA /r. */
int wcs_avx_vpsubd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xFA) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

/* vpand ymm_dst, ymm_src1, ymm_src2, VEX.256.66.0F.WIG DB /r. */
int wcs_avx_vpand_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xDB) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

/* vpor ymm_dst, ymm_src1, ymm_src2, VEX.256.66.0F.WIG EB /r. */
int wcs_avx_vpor_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xEB) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

/* REX.W 39 /r: cmp r64, r64 (flags from dst - src). */
int wcs_cmp_reg_reg64(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 1, src >> 3, 0, dst >> 3) &&
         binary_code_buffer_append_u8(b, 0x39) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* 81 /6 id: xor r32, imm32 (32-bit op: also zeroes the high dword). */
int wcs_xor_reg_imm32(BinaryCodeBuffer *b, int gpr, uint32_t imm) {
  return binary_emit_rex(b, 0, 0, 0, gpr >> 3) &&
         binary_code_buffer_append_u8(b, 0x81) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | (6 << 3) | (gpr & 7))) &&
         binary_code_buffer_append_u32(b, imm);
}

/* 0F BC /r: bsf r32, r32 (index of lowest set bit; src must be nonzero). */
int wcs_bsf_reg_reg32(BinaryCodeBuffer *b, int dst, int src) {
  return binary_emit_rex(b, 0, dst >> 3, 0, src >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xBC) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* 0F B6 /r: movzx r32, byte [base] (any base; SIB/disp handled). */
int wcs_movzx_reg_byte_mem(BinaryCodeBuffer *b, int gpr, int base) {
  return binary_emit_rex(b, 0, gpr >> 3, 0, base >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0xB6) &&
         wcs_avx_modrm_mem_disp(b, gpr, base, 0);
}

/* vpcmpeqd ymm_dst, ymm_s1, ymm_s2, VEX.256.66.0F.WIG 76 /r. Lane = all-ones
 * when the int32 lanes are equal. */
int wcs_avx_vpcmpeqd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x76) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

/* vpcmpgtd ymm_dst, ymm_s1, ymm_s2, VEX.256.66.0F.WIG 66 /r. SIGNED s1 > s2. */
int wcs_avx_vpcmpgtd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x66) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

/* vpcmpeqb ymm_dst, ymm_s1, ymm_s2, VEX.256.66.0F.WIG 74 /r. */
int wcs_avx_vpcmpeqb_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 1, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0x74) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}

/* vpbroadcastb ymm, xmm, VEX.256.66.0F38.W0 78 /r. */
int wcs_avx_vpbroadcastb_ymm(BinaryCodeBuffer *b, int dst, int src_xmm) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, src_xmm, 0) &&
         binary_code_buffer_append_u8(b, 0x78) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (src_xmm & 7)));
}

/* vmovmskps r32, ymm, VEX.256.0F.WIG 50 /r. One bit per dword lane (8). */
int wcs_avx_vmovmskps_reg_ymm(BinaryCodeBuffer *b, int gpr, int ymm) {
  return wcs_vex3(b, 1, 0, 1, 0, gpr, ymm, 0) &&
         binary_code_buffer_append_u8(b, 0x50) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (ymm & 7)));
}

/* vpmovmskb r32, ymm, VEX.256.66.0F.WIG D7 /r. One bit per byte lane (32). */
int wcs_avx_vpmovmskb_reg_ymm(BinaryCodeBuffer *b, int gpr, int ymm) {
  return wcs_vex3(b, 1, 1, 1, 0, gpr, ymm, 0) &&
         binary_code_buffer_append_u8(b, 0xD7) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((gpr & 7) << 3) | (ymm & 7)));
}

/* pcmpistri xmm_ranges, m128, imm8, 66 0F 3A 63 /r ib. The instruction
 * returns the selected byte index in ECX. */
int wcs_sse42_pcmpistri_ranges_mem(BinaryCodeBuffer *b, int ranges_xmm,
                                   int base, unsigned char control) {
  if ((base & 7) == 4) return 0; /* no SIB form is needed by the caller */
  return binary_code_buffer_append_u8(b, 0x66) &&
         binary_emit_rex(b, 0, ranges_xmm >> 3, 0, base >> 3) &&
         binary_code_buffer_append_u8(b, 0x0F) &&
         binary_code_buffer_append_u8(b, 0x3A) &&
         binary_code_buffer_append_u8(b, 0x63) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(((ranges_xmm & 7) << 3) | (base & 7))) &&
         binary_code_buffer_append_u8(b, control);
}

/* Broadcast a 32-bit GPR value across all eight lanes of a ymm register. */
int wcs_broadcast_i32_to_ymm(BinaryCodeBuffer *b, int ymm, int gpr) {
  return wcs_avx_vmovd_xmm_reg(b, ymm, gpr) &&
         wcs_avx_vpbroadcastd_ymm(b, ymm, ymm);
}

int wcs_reduce_ymm_i32_sum_to_rax(BinaryCodeBuffer *b, int src) {
  /* Fold high 128 onto low, drop AVX state, then reuse the SSE 4-lane fold
   * (which operates on xmm0 and sign-extends into RAX). */
  return wcs_avx_vextracti128(b, 0, src, 1) && wcs_avx_vzeroupper(b) &&
         wcs_paddd(b, src, 0) && wcs_sse_66(b, 0x6F, 0, src) &&
         wcs_accumulate_xmm0_i32_to_rax(b);
}

/* ---- FMA3 (VEX.DDS, 0F38 map), fused multiply-add helpers ----
 * vfmadd231 computes ModRM.reg = (vvvv * ModRM.rm) + ModRM.reg, i.e. the
 * destination accumulates src1*src2 in one rounding step. W1 selects double
 * lanes, W0 single; len256=1 picks the ymm form, len256=0 the scalar (xmm low
 * lane) form used by the dot/affine tails. All operands here are xmm/ymm 0..7
 * so REX.R/B (encoded in the VEX prefix) stay zero. */
int wcs_avx_vfmadd231pd_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 1, 1, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB8) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_avx_vfmadd231ps_ymm(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 1, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB8) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_fmadd231sd(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 0, 1, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB9) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
int wcs_fmadd231ss(BinaryCodeBuffer *b, int dst, int s1, int s2) {
  return wcs_vex3(b, 2, 1, 0, 0, dst, s2, s1) &&
         binary_code_buffer_append_u8(b, 0xB9) &&
         binary_code_buffer_append_u8(
             b, (unsigned char)(0xC0 | ((dst & 7) << 3) | (s2 & 7)));
}
