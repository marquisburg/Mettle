#include "codegen/binary/internal.h"
#include "codegen/binary/simd_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Integer-array SIMD kernels (AVX2-widened reductions, element-wise maps, sort/search). Low-level encoders live in simd_encoders.c; see simd_internal.h. */

/* Lower IR_OP_SIMD_SUM_I32: add sum of base[0..len-1] int32s into dest. */
int code_generator_binary_emit_simd_sum_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  /* rax=sum, rcx=base, edx=i, r8d=len */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  /* ymm2 = four int64 partial sums; rax = prior value + scalar-tail sums. */
  if (!wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8)) return 0;
  if (!wcs_jcc(b, 0x83 /* jae */, &j_done)) return 0;

  /* len - i >= 8 ? */
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &j_scalar)) return 0;

  /* AVX2: two vpmovsxdq loads sign-extend 8 int32 to int64, summed into ymm2.
   * Accumulating in 64-bit lanes preserves the int64 result semantics without
   * the per-lane extract the SSE path used. */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vpmovsxdq_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 0) ||
      !wcs_avx_vpmovsxdq_ymm_mem(b, 1, BINARY_GP_RCX, 16) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar)) return 0;
  if (!binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Reduce the four int64 lanes of ymm2 into rax (drop AVX state first). */
  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vextracti128(b, 0, 2, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_paddq(b, 2, 0) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_pshufd(b, 0, 2, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Lower IR_OP_SIMD_LCG_U32: vectorize a uint32 linear-congruential recurrence
 * reduction `state = state*A + C; sum += (int64)(state & MASK)` over `iters`
 * steps. dest = sum accumulator; lhs = iters; rhs = state (its value here is the
 * seed); arguments[0..2] = A, C, MASK. 8 lanes advance in lockstep via the
 * closed form state_{k+8} = A^8*state_k + Q8 (Q8 = (A^7+..+1)*C), all mod 2^32
 * (exact under vpmulld); each lane is masked, widened to int64, and summed. A
 * scalar remainder finishes iters % 8. Bit-identical to the scalar loop. */
int code_generator_binary_emit_simd_lcg_u32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  uint32_t A = 0, C = 0, MASK = 0;
  uint32_t P[9], Q[9];
  size_t loop_top = 0, j_done = 0, jb = 0, tail_top = 0, t_done = 0, tb = 0;

  if (!generator || !context || !instruction || instruction->argument_count < 3 ||
      !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_lcg_u32");
    return 0;
  }
  b = &context->code;
  A = (uint32_t)instruction->arguments[0].int_value;
  C = (uint32_t)instruction->arguments[1].int_value;
  MASK = (uint32_t)instruction->arguments[2].int_value;
  P[0] = 1u;
  Q[0] = 0u;
  for (int j = 1; j <= 8; j++) {
    P[j] = P[j - 1] * A;
    Q[j] = Q[j - 1] * A + C;
  }

  /* RAX = sum (current value, accumulated into), RCX = seed, R8 = iters. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R8)) {
    return 0;
  }

  /* Stack pool (rsp stable, no calls): [rsp+0..31]=P[1..8], [rsp+32..63]=Q[1..8].
   * [rsp+64..127] holds xmm6..xmm9, which the lanes and their accumulators
   * below overwrite and which MS-x64 makes this function's to give back. */
  if (!binary_emit_sub_rsp_imm32(b, 128)) {
    return 0;
  }
  for (int x = 6; x <= 9; x++) {
    if (!simd_movdqu_mem_xmm_disp(b, BINARY_GP_RSP, 64 + 16 * (x - 6), x)) {
      return 0;
    }
  }
  for (int j = 0; j < 8; j++) {
    if (!binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, P[j + 1]) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RSP, 4 * j, BINARY_GP_R11) ||
        !binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, Q[j + 1]) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RSP, 32 + 4 * j,
                                   BINARY_GP_R11)) {
      return 0;
    }
  }

  /* R9 = nvec = iters >> 3; R10 = tail = iters & 7. */
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 5 /* shr */, BINARY_GP_R9, 3) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_R10, 7)) {
    return 0;
  }

  /* ymm0 = lanes = seed*P[1..8] + Q[1..8] = state_1..state_8. */
  if (!wcs_avx_vmovd_xmm_reg(b, 1, BINARY_GP_RCX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 1, 1) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RSP, 0) ||
      !wcs_avx_vpmulld_ymm(b, 0, 1, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 2, BINARY_GP_RSP, 32) ||
      !wcs_avx_vpaddd_ymm(b, 0, 0, 2)) {
    return 0;
  }

  /* Broadcasts: ymm3 = MASK, ymm4 = A^8, ymm5 = Q8. */
  if (!binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, MASK) ||
      !wcs_avx_vmovd_xmm_reg(b, 3, BINARY_GP_R11) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, P[8]) ||
      !wcs_avx_vmovd_xmm_reg(b, 4, BINARY_GP_R11) ||
      !wcs_avx_vpbroadcastd_ymm(b, 4, 4) ||
      !binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_R11, Q[8]) ||
      !wcs_avx_vmovd_xmm_reg(b, 5, BINARY_GP_R11) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5)) {
    return 0;
  }

  /* int64 lane accumulators ymm6, ymm7 = 0. */
  if (!wcs_avx_vpxor_ymm(b, 6, 6, 6) || !wcs_avx_vpxor_ymm(b, 7, 7, 7)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R9, 0) ||
      !wcs_jcc(b, 0x84 /* je */, &j_done)) {
    return 0;
  }
  /* accumulate the 8 current lanes (masked, widened to int64) */
  if (!wcs_avx_vpand_ymm(b, 8, 0, 3) || !wcs_avx_vpmovsxdq_ymm_xmm(b, 9, 8) ||
      !wcs_avx_vpaddq_ymm(b, 6, 6, 9) || !wcs_avx_vextracti128(b, 8, 8, 1) ||
      !wcs_avx_vpmovsxdq_ymm_xmm(b, 9, 8) || !wcs_avx_vpaddq_ymm(b, 7, 7, 9)) {
    return 0;
  }
  /* advance: lanes = lanes*A^8 + Q8 */
  if (!wcs_avx_vpmulld_ymm(b, 0, 0, 4) || !wcs_avx_vpaddd_ymm(b, 0, 0, 5)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 /* sub */, 1)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, loop_top)) {
    return 0;
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }

  /* Stash lanes[0] = state_{8*nvec+1} for the scalar tail before reduce. */
  if (!wcs_avx_vmovd_mem_xmm(b, BINARY_GP_RSP, 0, 0)) {
    return 0;
  }

  /* Horizontal-sum the 8 int64 lanes (ymm6 + ymm7) into RAX. */
  if (!wcs_avx_vpaddq_ymm(b, 6, 6, 7) || !wcs_avx_vextracti128(b, 1, 6, 1) ||
      !wcs_avx_vzeroupper(b) || !wcs_paddq(b, 6, 1) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R11, BINARY_XMM6) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !wcs_pshufd(b, 1, 6, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R11, BINARY_XMM1) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11)) {
    return 0;
  }

  /* Scalar remainder: cur = lanes[0]; repeat tail times. */
  if (!binary_emit_mov_reg_mem32(b, BINARY_GP_RCX, BINARY_GP_RSP, 0)) {
    return 0;
  }
  tail_top = b->size;
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R10, 0) ||
      !wcs_jcc(b, 0x84 /* je */, &t_done)) {
    return 0;
  }
  if (!wcs_mov_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_RCX) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RDX, MASK) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_RDX) ||
      !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_RCX, BINARY_GP_RCX, A) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RCX, 0xFFFFFFFFu) ||
      !binary_emit_add_reg_imm32(b, BINARY_GP_RCX, C) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RCX, 0xFFFFFFFFu)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1 /* sub */, 1)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &tb) || !wcs_patch_to(b, tb, tail_top)) {
    return 0;
  }
  if (!wcs_patch_here(b, t_done)) {
    return 0;
  }

  for (int x = 6; x <= 9; x++) {
    if (!simd_movdqu_xmm_mem_disp(b, x, BINARY_GP_RSP, 64 + 16 * (x - 6))) {
      return 0;
    }
  }
  if (!binary_emit_add_rsp_imm32(b, 128)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Lower IR_OP_SIMD_SUM_U8: add the unsigned sum of base[0..len-1] uint8s into
 * dest (an int64). Uses vpsadbw against zero to fold 32 bytes per iteration
 * into four int64 lane accumulators, then a scalar movzx tail. */
int code_generator_binary_emit_simd_sum_u8(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  /* rax=sum, rcx=base, edx=i, r8d=len */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  /* ymm2 = four int64 partial sums; ymm3 = zero operand for vpsadbw. */
  if (!wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 3, 3, 3)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8)) return 0;
  if (!wcs_jcc(b, 0x83 /* jae */, &j_done)) return 0;

  /* len - i >= 32 ? */
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec)) {
    return 0;
  }
  if (!wcs_jcc(b, 0, &j_scalar)) return 0;

  /* AVX2: vpsadbw of 32 bytes against zero yields four 64-bit lane sums (each
   * <= 8*255), accumulated into ymm2 with vpaddq. */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpsadbw_ymm(b, 0, 0, 3) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Scalar tail: zero-extend one byte and add (unsigned). */
  if (!wcs_patch_here(b, j_scalar)) return 0;
  if (!binary_emit_movzx_reg_mem8(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Reduce the four int64 lanes of ymm2 into rax (drop AVX state first). */
  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vextracti128(b, 0, 2, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_paddq(b, 2, 0) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_pshufd(b, 0, 2, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Emit one byte-map chain step applied to the 16 packed bytes in xmm0. The
 * step's broadcast constant lives at [rsp+bcast_off]; the 0x00FF word mask (for
 * the multiply path's pack) at [rsp+mask_off]. Scratch: xmm1-xmm5. */
static int byte_map_emit_step_vec(BinaryCodeBuffer *b, int op, int bcast_off,
                                  int mask_off) {
  switch (op) {
  case IR_BYTE_MAP_ADD:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpaddb_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_SUB:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpsubb_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_XOR:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpxor_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_AND:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpand_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_OR:
    return wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpor_xmm(b, 0, 0, 1);
  case IR_BYTE_MAP_MUL:
    /* Widen bytes to words, multiply by the broadcast word, keep the low byte
     * of each product (mask so packuswb does not saturate), repack. */
    return wcs_avx_vpxor_xmm(b, 3, 3, 3) &&
           wcs_avx_vpunpcklbw_xmm(b, 1, 0, 3) &&
           wcs_avx_vpunpckhbw_xmm(b, 2, 0, 3) &&
           wcs_avx_vmovdqu_xmm_mem(b, 4, BINARY_GP_RSP, bcast_off) &&
           wcs_avx_vpmullw_xmm(b, 1, 1, 4) &&
           wcs_avx_vpmullw_xmm(b, 2, 2, 4) &&
           wcs_avx_vmovdqu_xmm_mem(b, 5, BINARY_GP_RSP, mask_off) &&
           wcs_avx_vpand_xmm(b, 1, 1, 5) &&
           wcs_avx_vpand_xmm(b, 2, 2, 5) &&
           wcs_avx_vpackuswb_xmm(b, 0, 1, 2);
  default:
    return 0;
  }
}

/* Scalar-tail equivalent: apply one step to the byte value in r10 (32-bit ALU;
 * the byte store truncates, and these ops agree mod 256 with the vector path). */
static int byte_map_emit_step_scalar(BinaryCodeBuffer *b, int op, int k) {
  switch (op) {
  case IR_BYTE_MAP_ADD:
    return binary_emit_add_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_SUB:
    return binary_emit_sub_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_XOR:
    return binary_emit_xor_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_AND:
    return binary_emit_and_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_OR:
    return binary_emit_or_reg_imm32(b, BINARY_GP_R10, (uint32_t)k);
  case IR_BYTE_MAP_MUL:
    return binary_emit_imul_reg_reg_imm32(b, BINARY_GP_R10, BINARY_GP_R10,
                                          (uint32_t)k);
  default:
    return 0;
  }
}

/* Store the element in RAX's low bytes to [RCX], sized. */
static int fill_emit_element_store(BinaryCodeBuffer *b, long long size) {
  switch (size) {
  case 1: return binary_emit_mov_mem_reg8(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  case 2: return binary_emit_mov_mem_reg16(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  case 4: return binary_emit_mov_mem_reg32(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  case 8: return binary_emit_mov_mem_reg(b, BINARY_GP_RCX, 0, BINARY_GP_RAX);
  default: return 0;
  }
}

/* Build the 16-byte fill block in XMM0 from the raw element value in RAX, for
 * element size 1/2/4/8 (the value is byte/word/dword-replicated to fill a full
 * 16-byte vector lane). RAX keeps the low element so a scalar tail can store it
 * directly. Shared by the fallback fill lowering and the MIR inline-fill
 * passthrough (mir_encode MIR_SIMD_FILL). */
int code_generator_binary_emit_simd_fill_splat(BinaryCodeBuffer *b,
                                               long long size) {
  if (size == 1) {
    if (!binary_emit_and_reg_imm32(b, BINARY_GP_RAX, 0xFF) ||
        !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_RAX, BINARY_GP_RAX,
                                        0x01010101u)) {
      return 0;
    }
  } else if (size == 2) {
    if (!binary_emit_and_reg_imm32(b, BINARY_GP_RAX, 0xFFFF) ||
        !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_RAX, BINARY_GP_RAX,
                                        0x00010001u)) {
      return 0;
    }
  } else if (size == 4) {
    if (!wcs_mov_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX)) {
      return 0; /* zero-extend to a clean low half */
    }
  }
  if (size != 8) {
    /* high half = low half: r9 = rax; rax <<= 32; rax += r9. */
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RAX) ||
        !binary_emit_shift_reg_imm8(b, 4 /* shl */, BINARY_GP_RAX, 32) ||
        !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R9)) {
      return 0;
    }
  }
  if (!binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 0)) {
    return 0;
  }
  return 1;
}

/* Element-counted fill loop (mode 0): store the XMM0 16-byte block over R8
 * elements (int32-ranged signed count) starting at RCX, `size` bytes each, with
 * a scalar element tail. Clobbers RCX/RDX/R8/R9 and XMM0. No live-iv write-back
 * -- the caller restores the final induction value if it is live. Shared by the
 * fallback fill lowering and the MIR inline-fill passthrough. */
int code_generator_binary_emit_simd_fill_loop_mode0(BinaryCodeBuffer *b,
                                                    long long size) {
  long long per_vec = 16 / size;
  size_t j_done_neg = 0, j_done = 0, j_vec = 0, j_scalar = 0;
  size_t loop_top = 0;
  /* 32-bit signed guard: `while (i < n)` with n <= 0 runs zero iterations. */
  if (!wcs_test_reg_reg32(b, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8E /* jle */, &j_done_neg) ||
      !wcs_xor_self32(b, BINARY_GP_RDX)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, (uint32_t)per_vec) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RCX, 0, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, (unsigned char)per_vec)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_scalar) ||
      !fill_emit_element_store(b, size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done) || !wcs_patch_here(b, j_done_neg)) {
    return 0;
  }
  return 1;
}

/* Byte-walked fill loop (modes 1/2): store the XMM0 16-byte block from RCX
 * while at least 16 bytes remain, then an element tail while any bytes remain
 * (the final element may overshoot by up to size-1 bytes, exactly as the scalar
 * `*p <- v; p += size` loop does). Mode 1 computes the byte length as R8 - RCX
 * (R8 = end pointer); mode 2 arrives with R8 = byte length precomputed. Assumes
 * the splat is in XMM0. No mode-2 live-iv write-back (the caller handles it).
 * Shared by the fallback fill lowering and the MIR inline-fill passthrough. */
int code_generator_binary_emit_simd_fill_loop_bytewalk(BinaryCodeBuffer *b,
                                                       long long size, int mode) {
  size_t j_done = 0, j_tail = 0;
  size_t loop16_top = 0, tail_top = 0;
  if (mode == 1 && !wcs_sub_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX)) {
    return 0;
  }
  loop16_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R8, 16) ||
      !wcs_jcc(b, 0x8C /* jl */, &j_tail)) {
    return 0;
  }
  if (!wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RCX, 0, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R8, 1, 16)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop16_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_tail)) {
    return 0;
  }
  tail_top = b->size;
  if (!binary_emit_test_reg_reg(b, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8E /* jle */, &j_done)) {
    return 0;
  }
  if (!fill_emit_element_store(b, size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)size) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R8, 1, (unsigned char)size)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }
  return 1;
}

/* Lower IR_OP_SIMD_BYTE_MAP: in-place apply the constant byte-op chain to
 * base[0..len-1], 16 bytes per VEX.128 iteration plus a scalar tail. */
int code_generator_binary_emit_simd_byte_map(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t nsteps = 0;
  int mask_off = 0;
  uint32_t cbytes = 0;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count == 0 ||
      instruction->argument_count % 2 != 0) {
    return 0;
  }
  b = &context->code;
  nsteps = instruction->argument_count / 2;
  mask_off = 16 * (int)nsteps;
  cbytes = (uint32_t)(16 * ((int)nsteps + 1)); /* per-step broadcasts + mask */

  if (!binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }

  /* Build each step's 16-byte broadcast (and the word mask) into the scratch
   * frame. This setup runs once; the hot loop below is pure VEX. */
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[2 * s].int_value;
    int k = (int)instruction->arguments[2 * s + 1].int_value;
    uint64_t splat;
    if (op == IR_BYTE_MAP_MUL) {
      uint64_t w = (uint64_t)(k & 0xFFFF);
      splat = w | (w << 16) | (w << 32) | (w << 48);
    } else {
      uint64_t byte = (uint64_t)(k & 0xFF);
      splat = byte * 0x0101010101010101ULL;
    }
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, splat) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 0) ||
        !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RSP, 16 * (int)s, 0)) {
      return 0;
    }
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x00FF00FF00FF00FFULL) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 0) ||
      !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RSP, mask_off, 0)) {
    return 0;
  }

  /* rcx=base, r8=len, rdx=i. Loaded after the broadcasts (which use rax/xmm0). */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_xor_self32(b, BINARY_GP_RDX)) {
    return 0;
  }

  loop_top = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }
  /* len - i >= 16 ? */
  if (!wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_sub_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 16) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Vector path: 16 bytes through the chain, stored back in place. */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_xmm_mem(b, 0, BINARY_GP_RCX, 0)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[2 * s].int_value;
    if (!byte_map_emit_step_vec(b, op, 16 * (int)s, mask_off)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RCX, 0, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 16)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Scalar tail: one byte through the same chain. */
  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_movzx_reg_mem8(b, BINARY_GP_R10, BINARY_GP_RCX, 0)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[2 * s].int_value;
    int k = (int)instruction->arguments[2 * s + 1].int_value;
    if (!byte_map_emit_step_scalar(b, op, k)) {
      return 0;
    }
  }
  if (!binary_emit_mov_mem_reg8(b, BINARY_GP_RCX, 0, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return 1;
}

/* In-place int32 insertion sort with SSE2 chunk scan and 16-byte shifts. */
int code_generator_binary_emit_simd_insertion_sort_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t outer_loop = 0;
  size_t inner_loop = 0;
  size_t j_insert_from_bound = 0;
  size_t j_insert_from_le = 0;
  size_t j_done = 0;

  if (!generator || !context || !instruction ||
      instruction->dest.kind == IR_OPERAND_NONE ||
      instruction->rhs.kind == IR_OPERAND_NONE) {
    code_generator_set_error(generator, "Malformed simd_insertion_sort_i32");
    return 0;
  }
  b = &context->code;

  /* rcx=base, r8d=len, edx=i, r9d=key, r10=scan_ptr, r11d=current */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_xor_self32(b, BINARY_GP_RDX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }

  outer_loop = b->size;
  if (!wcs_cmp_reg_reg32(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_done)) {
    return 0;
  }

  if (!binary_emit_lea_reg_base_index_scale_disp(
          b, BINARY_GP_RAX, BINARY_GP_RCX, BINARY_GP_RDX, 4, 0) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RAX, 0) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_RAX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 4)) {
    return 0;
  }

  inner_loop = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_insert_from_bound) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R11, BINARY_GP_R10, 0) ||
      !wcs_cmp_reg_reg32(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8E /* jle */, &j_insert_from_le) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_R10, 4, BINARY_GP_R11) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, inner_loop)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_insert_from_bound) ||
      !wcs_patch_here(b, j_insert_from_le) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_R10, 4, BINARY_GP_R9) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, outer_loop)) {
      return 0;
    }
  }

  return wcs_patch_here(b, j_done);
}

int code_generator_binary_emit_lower_bound_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_back = 0;

  if (!generator || !context || !instruction || instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  /* r10=lo, r11=hi, rsi=arr, r9d=key, rax=mid, rcx=arr[mid]. All Win64 volatile
   * GPs (rax/rcx/rdx/r8..r11) are already in use, so the array base must live in
   * a callee-saved register; RSI is nonvolatile on Win64, so save/restore it
   * here (the function prologue does not know this special codegen uses it).
   * On SysV RSI is caller-saved, so the push/pop is merely harmless there. */
  if (!binary_emit_push_reg(b, BINARY_GP_RSI)) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R9) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RSI)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_R10, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_shift_reg_imm8(b, 7, BINARY_GP_RAX, 1) ||
      !binary_emit_alu_reg_reg(b, 0x01, BINARY_GP_RAX, BINARY_GP_R10) ||
      !binary_emit_lea_reg_base_index_scale_disp(
          b, BINARY_GP_RCX, BINARY_GP_RSI, BINARY_GP_RAX, 4, 0) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_RCX, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RDX, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R8, BINARY_GP_RAX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R8, 0, 1) ||
      !wcs_cmp_reg_reg32(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4C /* cmovl */, BINARY_GP_R10,
                                  BINARY_GP_R8) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4D /* cmovge */, BINARY_GP_R11,
                                  BINARY_GP_RDX) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
    return 0;
  }
  j_back = 0;
  if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top) ||
      !wcs_patch_here(b, j_done) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R10)) {
    return 0;
  }
  /* Restore the caller's RSI (saved above). The destination store used R10, not
   * RSI, so it is safe to pop here on the single success path. */
  if (!binary_emit_pop_reg(b, BINARY_GP_RSI)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_scale_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;
  int32_t mul_imm = 0;
  int32_t add_imm = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 3) {
    return 0;
  }
  b = &context->code;
  mul_imm = (int32_t)instruction->arguments[1].int_value;
  add_imm = (int32_t)instruction->arguments[2].int_value;

  /* rax=sum, rcx=src, rdx=dst, r8=src_end, xmm4/xmm5=mul/add */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)mul_imm) ||
      !wcs_broadcast_i32_to_ymm(b, 4, BINARY_GP_R9) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)add_imm) ||
      !wcs_broadcast_i32_to_ymm(b, 5, BINARY_GP_R9) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* AVX2: two 32-byte lanes per 64-byte block. Each is multiplied by the
   * broadcast mul (vpmulld), offset by add (vpaddd), stored, and folded into
   * the ymm2 int32 sum accumulator. */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpmulld_ymm(b, 0, 0, 4) ||
      !wcs_avx_vpaddd_ymm(b, 0, 0, 5) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 0, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vpmulld_ymm(b, 0, 0, 4) ||
      !wcs_avx_vpaddd_ymm(b, 0, 0, 5) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 32, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_imul_reg_reg_imm32(b, BINARY_GP_R10, BINARY_GP_R10,
                                      (uint32_t)mul_imm) ||
      !binary_emit_add_reg_imm32(b, BINARY_GP_R10, (uint32_t)add_imm) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R10) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_reduce_ymm_i32_sum_to_rax(b, 2)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_reverse_copy_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  /* rax=sum, rcx=src (last elem), rdx=dst walk, r8=dst_end, r10=src_base */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(&context->code, BINARY_GP_R10, BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1, 1) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RDX)) {
    return 0;
  }
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RDX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_scalar) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_R10) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 60) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_scalar) ||
      !wcs_jcc(b, 0, &j_vec)) {
    return 0;
  }

  /* AVX2: reverse eight int32 per 32-byte lane. The 32 bytes ending at rcx
   * (disp -28) load as ascending memory [src[k-7]..src[k]]; vpshufd 0x1B
   * reverses dwords within each 128-bit half, then vperm2i128 swaps the halves,
   * giving the full 8-lane reverse [src[k]..src[k-7]] to store ascending. Two
   * such lanes cover the 64-byte block (second ends at rcx-32 -> disp -60). */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, -28) ||
      !wcs_avx_vpshufd_ymm(b, 0, 0, 0x1B) ||
      !wcs_avx_vperm2i128(b, 0, 0, 0, 0x01) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 0, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, -60) ||
      !wcs_avx_vpshufd_ymm(b, 0, 0, 0x1B) ||
      !wcs_avx_vperm2i128(b, 0, 0, 0, 0x01) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 32, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 1, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R9) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R9) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 1, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_reduce_ymm_i32_sum_to_rax(b, 2)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_clamp_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;
  int32_t lo = 0;
  int32_t hi = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 3) {
    return 0;
  }
  b = &context->code;
  lo = (int32_t)instruction->arguments[1].int_value;
  hi = (int32_t)instruction->arguments[2].int_value;
  if (lo > hi) {
    return 0;
  }

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)lo) ||
      !wcs_broadcast_i32_to_ymm(b, 4, BINARY_GP_R9) ||
      !wcs_mov_reg_imm32(b, BINARY_GP_R9, (uint32_t)hi) ||
      !wcs_broadcast_i32_to_ymm(b, 5, BINARY_GP_R9) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* AVX2: clamp eight int32 per iteration, vpmaxsd against the lo broadcast,
   * vpminsd against the hi broadcast, store, and fold into the ymm2 sum. */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpmaxsd_ymm(b, 0, 0, 4) ||
      !wcs_avx_vpminsd_ymm(b, 0, 0, 5) ||
      !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_RDX, 0, 0) ||
      !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0)) {
    return 0;
  }
  {
    size_t j_not_lo = 0;
    size_t j_not_hi = 0;
    size_t j_clamp_done = 0;
    if (!wcs_cmp_reg_imm32(b, BINARY_GP_R10, lo) ||
        !wcs_jcc(b, 0x8D /* jge */, &j_not_lo) ||
        !wcs_mov_reg_imm32(b, BINARY_GP_R10, (uint32_t)lo) ||
        !wcs_jcc(b, 0, &j_clamp_done)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_not_lo) ||
        !wcs_cmp_reg_imm32(b, BINARY_GP_R10, hi) ||
        !wcs_jcc(b, 0x8E /* jle */, &j_not_hi) ||
        !wcs_mov_reg_imm32(b, BINARY_GP_R10, (uint32_t)hi)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_not_hi) ||
        !wcs_patch_here(b, j_clamp_done) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R10) ||
        !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
        !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
      return 0;
    }
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_reduce_ymm_i32_sum_to_rax(b, 2)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_dot_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_i32");
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 5, 5, 5) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R11, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 128) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 64) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 64) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 96) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 96) ||
      !wcs_avx_vpmuldq_ymm(b, 3, 0, 1) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 0, 0, 32) ||
      !wcs_avx_vpsrlq_ymm_imm(b, 1, 1, 32) ||
      !wcs_avx_vpmuldq_ymm(b, 4, 0, 1) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R9, BINARY_GP_RDX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R9) ||
      !binary_emit_imul_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vpaddq_ymm(b, 2, 2, 5) ||
      !wcs_avx_vextracti128(b, 3, 2, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_paddq(b, 2, 3) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_pshufd(b, 3, 2, 0xEE) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }

  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* IR_OP_SIMD_DOT_I8: byte x byte -> int32 dot product (the inner loop of
 * quantized GEMM/GEMV). dest = int32 sum symbol, lhs/rhs = the two byte array
 * bases, arguments[0] = element count n, is_unsigned = whether the source's
 * loads widen zero-extended. Two int32 accumulators (ymm2/ymm3) consume 32
 * bytes per iteration: vpmovsxbw or vpmovzxbw widens 16 bytes to 16 int16,
 * vpmaddwd folds adjacent pairs into 8 int32 partial dots, vpaddd accumulates.
 * The lanes are reduced to one signed int32 in-vector (wrapping at 32 bits like
 * the scalar `int32` accumulator does), then a scalar byte tail handles n % 32.
 * The whole reduction is summed in RAX and stored truncated to the int32 dest,
 * which preserves the source's int32 wraparound (sum mod 2^32).
 *
 * The widening used to be zero-extending whatever the element type said, so
 * every negative int8 in a dot product came back as its unsigned reading: a
 * kernel over -1 and 3 answered 765 per element instead of -3. */
typedef int (*BinaryByteWiden)(BinaryCodeBuffer *, int, int, int);
typedef int (*BinaryByteLoad)(BinaryCodeBuffer *, BinaryGpRegister,
                              BinaryGpRegister, int);

/* Main loop: while (end - cur) >= 32, two 16-wide MAC chains. */
static int binary_emit_dot_i8_body(BinaryCodeBuffer *b, BinaryByteWiden widen) {
  size_t loop_top = b->size;
  size_t after_main = 0;
  size_t j_back = 0;
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 32) ||
      !wcs_jcc(b, 0x82 /* jb */, &after_main) ||
      !widen(b, 0, BINARY_GP_RCX, 0) || !widen(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vpmaddwd_ymm(b, 0, 0, 1) || !wcs_avx_vpaddd_ymm(b, 2, 2, 0) ||
      !widen(b, 0, BINARY_GP_RCX, 16) || !widen(b, 1, BINARY_GP_RDX, 16) ||
      !wcs_avx_vpmaddwd_ymm(b, 0, 0, 1) || !wcs_avx_vpaddd_ymm(b, 3, 3, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
    return 0;
  }
  /* Reduce ymm2+ymm3 to a single signed int32 in RAX. */
  return wcs_patch_here(b, after_main) && wcs_avx_vpaddd_ymm(b, 2, 2, 3) &&
         wcs_avx_vextracti128(b, 3, 2, 1) && wcs_avx_vzeroupper(b) &&
         wcs_paddd(b, 2, 3) && wcs_pshufd(b, 3, 2, 0xEE) &&
         wcs_paddd(b, 2, 3) && wcs_pshufd(b, 3, 2, 0x55) &&
         wcs_paddd(b, 2, 3) &&
         binary_emit_movd_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM2) &&
         binary_emit_movsxd_rax_eax(b);
}

/* Scalar byte tail: while (cur < end), widen a[i]*b[i] into RAX the same way
 * the vector body did. */
static int binary_emit_dot_i8_tail(BinaryCodeBuffer *b,
                                   BinaryByteLoad tail_load) {
  size_t tail_top = b->size;
  size_t j_done = 0;
  size_t j_back = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !tail_load(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !tail_load(b, BINARY_GP_R9, BINARY_GP_RDX, 0) ||
      !binary_emit_imul_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1) ||
      !wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
    return 0;
  }
  return wcs_patch_here(b, j_done);
}

int code_generator_binary_emit_simd_dot_i8(CodeGenerator *generator,
                                           BinaryFunctionContext *context,
                                           const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  BinaryByteWiden widen = NULL;
  BinaryByteLoad tail_load = NULL;

  if (!generator || !context || !instruction ||
      instruction->argument_count != 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_i8");
    return 0;
  }
  widen = instruction->is_unsigned ? wcs_avx_vpmovzxbw_ymm_mem
                                   : wcs_avx_vpmovsxbw_ymm_mem;
  tail_load = instruction->is_unsigned ? binary_emit_movzx_reg_mem8
                                       : binary_emit_movsx_reg_mem8;
  b = &context->code;

  /* a->RCX, b->RDX, n->R8; end pointer R11 = RCX + n (int8 = 1 byte). */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RCX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R8)) {
    return 0;
  }

  if (!binary_emit_dot_i8_body(b, widen) ||
      !binary_emit_dot_i8_tail(b, tail_load)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Pure inner loop for the SLP MAC kernel, emitted INLINE inside a MIR
 * register-allocated function (see MIR_SIMD_SLP_MAC). The caller (the MIR
 * encoder) has already marshalled the operands into fixed registers:
 *   RCX = a element pointer (already offset to a[a_off])
 *   RDX = b element pointer (already offset to b[b_off])
 *   R8  = out element pointer (already offset to out[out_off])
 *   R9  = k iteration count
 *   RAX = b row stride in BYTES (b advances by this each k)
 * K is 4 or 8 (lanes). Clobbers RCX/RDX/R8/R9/RAX and ymm0..3; leaves the result
 * stored at [R8]. Unlike the fallback entry point above it performs NO operand
 * loads, so it is safe to run with no coherent fallback stack homes. */
int code_generator_binary_emit_vzeroupper(BinaryCodeBuffer *b) {
  return wcs_avx_vzeroupper(b);
}

/* One unrolled multiply-accumulate step: acc_ymm += broadcast(a[rcx+a_disp]) *
 * b[rdx], then advance rdx by the row stride in RAX. ymm0 is the broadcast temp,
 * ymm1 the load/product temp (both Win64-volatile). `wide` selects ymm (K=8) vs
 * xmm (K=4) b loads; an xmm load zero-extends the upper lanes so the ymm
 * multiply/add leave the accumulator's upper half untouched. */
static int slp_mac_step(BinaryCodeBuffer *b, int wide, int a_disp, int acc_ymm) {
  return wcs_avx_vpbroadcastd_ymm_mem(b, 0, BINARY_GP_RCX, a_disp) &&
         (wide ? wcs_avx_vmovdqu_ymm_mem(b, 1, BINARY_GP_RDX, 0)
               : wcs_avx_vmovdqu_xmm_mem(b, 1, BINARY_GP_RDX, 0)) &&
         wcs_avx_vpmulld_ymm(b, 1, 0, 1) &&
         wcs_avx_vpaddd_ymm(b, acc_ymm, acc_ymm, 1) &&
         wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_RAX);
}

int code_generator_binary_emit_simd_slp_mac_i32_loop(BinaryCodeBuffer *b,
                                                      long long K) {
  size_t main_top = 0, after_main = 0, tail_top = 0, j_done = 0;
  int wide = (K == 8);
  if ((K != 4 && K != 8) || !b) {
    return 0;
  }
  /* Four independent accumulators (ymm2..5, all Win64-volatile) so the serial
   * vpmulld->vpaddd dependency is hidden by 4-way ILP: a single-accumulator loop
   * is latency-bound (~10-cycle multiply) and loses to the scalar code's four
   * independent MAC chains. The main loop consumes 4 k-iterations at a time
   * (advancing rdx by the stride after each step, rcx by 16 at the end); a tail
   * loop handles a k-count that is not a multiple of 4. */
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4) || !wcs_avx_vpxor_ymm(b, 5, 5, 5)) {
    return 0;
  }
  main_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 4) ||
      !wcs_jcc(b, 0x8C /* jl (signed) */, &after_main) ||
      !slp_mac_step(b, wide, 0, 2) || !slp_mac_step(b, wide, 4, 3) ||
      !slp_mac_step(b, wide, 8, 4) || !slp_mac_step(b, wide, 12, 5) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
      !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, main_top)) {
      return 0;
    }
  }
  /* Reduce the four accumulators into ymm2. */
  if (!wcs_patch_here(b, after_main) || !wcs_avx_vpaddd_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddd_ymm(b, 4, 4, 5) || !wcs_avx_vpaddd_ymm(b, 2, 2, 4)) {
    return 0;
  }
  /* Tail: remaining (k mod 4) iterations, one at a time, into ymm2. */
  tail_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 0) ||
      !wcs_jcc(b, 0x84 /* je */, &j_done) || !slp_mac_step(b, wide, 0, 2) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }
  if (wide ? !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_R8, 0, 2)
           : !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_R8, 0, 2)) {
    return 0;
  }
  /* No vzeroupper here: this kernel runs inline, often many times per function
   * (once per output tile), so a per-invocation vzeroupper is pure overhead. The
   * MIR encoder emits a SINGLE vzeroupper in the epilogue of any function that
   * used an inline vector kernel, guarding the AVX->legacy-SSE transition once at
   * the function boundary. */
  return 1;
}

/* One int8 SLP-MAC step: acc += broadcast((int32)a[rcx+a_disp]) * widen(b[rdx]),
 * then advance b by the row stride in RAX. Mirrors slp_mac_step but a/b are int8
 * arrays: a[k] is one byte (zero-extended to an int32 broadcast via R10->xmm0),
 * and b's K contiguous bytes are zero-extended to K int32 lanes by vpmovzxbd.
 * R10 is a free scratch GP inside the kernel (the encoder's SCRATCH_A; no live
 * vreg occupies it across this call-like op). */
static int slp_mac_i8_step(BinaryCodeBuffer *b, int wide, int a_disp,
                           int acc_ymm) {
  return binary_emit_movzx_reg_mem8(b, BINARY_GP_R10, BINARY_GP_RCX, a_disp) &&
         wcs_avx_vmovd_xmm_reg(b, 0, BINARY_GP_R10) &&
         wcs_avx_vpbroadcastd_ymm(b, 0, 0) &&
         (wide ? wcs_avx_vpmovzxbd_ymm_mem(b, 1, BINARY_GP_RDX, 0)
               : wcs_avx_vpmovzxbd_xmm_mem(b, 1, BINARY_GP_RDX, 0)) &&
         wcs_avx_vpmulld_ymm(b, 1, 0, 1) &&
         wcs_avx_vpaddd_ymm(b, acc_ymm, acc_ymm, 1) &&
         wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_RAX);
}

/* int8 x int8 -> int32 inline SLP-MAC kernel: the quantized-GEMM tile. Same
 * structure as ..._slp_mac_i32_loop (4 int32 accumulators ymm2..5, 4 k-iters per
 * group + scalar tail) but a/b are byte arrays. RCX=a (int8), RDX=b (int8),
 * R8=out (int32), R9=count, RAX=b row stride in BYTES. a advances 1 byte per k
 * (4 per group); b widens through vpmovzxbd; the int32 accumulate/store/reduce
 * are identical to the int32 kernel (out is int32). */
int code_generator_binary_emit_simd_slp_mac_i8_loop(BinaryCodeBuffer *b,
                                                     long long K) {
  size_t main_top = 0, after_main = 0, tail_top = 0, j_done = 0;
  int wide = (K == 8);
  if ((K != 4 && K != 8) || !b) {
    return 0;
  }
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4) || !wcs_avx_vpxor_ymm(b, 5, 5, 5)) {
    return 0;
  }
  main_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 4) ||
      !wcs_jcc(b, 0x8C /* jl (signed) */, &after_main)) {
    return 0;
  }
  if (!wide) {
    /* K=4 fast path: load all four a[k..k+3] once (vpmovzxbd -> 4 int32 in
     * xmm6) and splat lane j with a VEX vpshufd, avoiding the per-k
     * movzx+vmovd+vpbroadcastd GP<->XMM round-trip that otherwise bottlenecks
     * the loop. The 4 accumulators (ymm2..5) still hide the multiply latency;
     * b's upper ymm halves stay zero (xmm vpmovzxbd loads), so the int32
     * accumulators' upper halves are untouched and the xmm store is exact. */
    static const unsigned char splat[4] = {0x00, 0x55, 0xAA, 0xFF};
    const int acc[4] = {2, 3, 4, 5};
    if (!wcs_avx_vpmovzxbd_xmm_mem(b, 6, BINARY_GP_RCX, 0)) {
      return 0;
    }
    for (int j = 0; j < 4; j++) {
      if (!wcs_avx_vpshufd_ymm(b, 0, 6, splat[j]) ||
          !wcs_avx_vpmovzxbd_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
          !wcs_avx_vpmulld_ymm(b, 1, 0, 1) ||
          !wcs_avx_vpaddd_ymm(b, acc[j], acc[j], 1) ||
          !wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_RAX)) {
        return 0;
      }
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 4)) {
      return 0;
    }
  } else if (!slp_mac_i8_step(b, wide, 0, 2) ||
             !slp_mac_i8_step(b, wide, 1, 3) ||
             !slp_mac_i8_step(b, wide, 2, 4) ||
             !slp_mac_i8_step(b, wide, 3, 5) ||
             !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
             !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, main_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, after_main) || !wcs_avx_vpaddd_ymm(b, 2, 2, 3) ||
      !wcs_avx_vpaddd_ymm(b, 4, 4, 5) || !wcs_avx_vpaddd_ymm(b, 2, 2, 4)) {
    return 0;
  }
  tail_top = b->size;
  if (!binary_emit_cmp_reg_imm32(b, BINARY_GP_R9, 0) ||
      !wcs_jcc(b, 0x84 /* je */, &j_done) || !slp_mac_i8_step(b, wide, 0, 2) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !binary_emit_sub_reg_imm32(b, BINARY_GP_R9, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, tail_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, j_done)) {
    return 0;
  }
  if (wide ? !wcs_avx_vmovdqu_mem_ymm(b, BINARY_GP_R8, 0, 2)
           : !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_R8, 0, 2)) {
    return 0;
  }
  return 1; /* vzeroupper emitted once in the MIR epilogue */
}

int code_generator_binary_emit_prefix_sum_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  /* r8=sum, rcx=src, rdx=dst, r9=end */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R11) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R11) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  /* Vector body, 4 lanes per step. The stored prefixes are exact mod 2^32
   * (identical to the scalar (int32) truncation), computed by the classic
   * in-register scan network: x += x<<32bits, x += x<<64bits, then a lane
   * broadcast of the running carry. The RETURNED sum must be exact int64, so
   * each chunk's elements are also sign-extended and accumulated in 64-bit
   * lanes (one widening load + one vpaddq), and the horizontal fold lands in
   * r8 before the scalar tail continues with it. The tail's stores stay
   * consistent because r8 mod 2^32 equals the carry. */
  {
    size_t j_novec = 0;
    size_t j_dist_ok = 0;
    size_t vec_top = 0;
    /* A chunk reads its 16 source bytes before writing any of its 16
     * destination bytes, where the scalar order interleaves them; the results
     * differ only when the destination sits 1..15 bytes ahead of the source
     * (a store then lands on a source byte the scalar loop had yet to read).
     * That sliver of overlap keeps the scalar loop. */
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_RDX) ||
        !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
        !binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 /* jz */, &j_dist_ok) ||
        !binary_emit_cmp_reg_imm32(b, BINARY_GP_R10, 16) ||
        !wcs_jcc(b, 0x82 /* jb, unsigned */, &j_novec)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_dist_ok) ||
        !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R10, 1 /* shr */, 4) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R10, 0 /* shl */, 4) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R10, BINARY_GP_RCX)) {
      return 0;
    }
    if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x83 /* jae */, &j_novec)) {
      return 0;
    }
    if (!wcs_avx_vpxor_ymm(b, 5, 5, 5) ||
        !wcs_broadcast_i32_to_ymm(b, 2, BINARY_GP_R8)) {
      return 0;
    }
    vec_top = b->size;
    if (!wcs_avx_vmovdqu_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
        !wcs_avx_vpmovsxdq_ymm_mem(b, 4, BINARY_GP_RCX, 0) ||
        !wcs_avx_vpaddq_ymm(b, 5, 5, 4) ||
        !wcs_avx_vpslldq_xmm(b, 1, 0, 4) ||
        !wcs_avx_vpaddd_xmm(b, 0, 0, 1) ||
        !wcs_avx_vpslldq_xmm(b, 1, 0, 8) ||
        !wcs_avx_vpaddd_xmm(b, 0, 0, 1) ||
        !wcs_avx_vpaddd_xmm(b, 0, 0, 2) ||
        !wcs_avx_vmovdqu_mem_xmm(b, BINARY_GP_RDX, 0, 0) ||
        !wcs_avx_vpshufd_xmm(b, 2, 0, 0xFF) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 16) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 16) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R10)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 /* jb */, &j_back) ||
          !wcs_patch_to(b, j_back, vec_top)) {
        return 0;
      }
    }
    if (!wcs_avx_vextracti128(b, 4, 5, 1) ||
        !wcs_avx_vpaddq_xmm(b, 5, 5, 4) ||
        !binary_emit_movq_reg_xmm(b, BINARY_GP_R10, BINARY_XMM5) ||
        !wcs_avx_vpshufd_xmm(b, 5, 5, 0x4E) ||
        !binary_emit_movq_reg_xmm(b, BINARY_GP_R11, BINARY_XMM5) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_R10) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_R11) ||
        !wcs_avx_vzeroupper(b)) {
      return 0;
    }
    if (!wcs_patch_here(b, j_novec)) {
      return 0;
    }
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_mem32(b, BINARY_GP_R10, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_R10) ||
      !binary_emit_mov_mem_reg32(b, BINARY_GP_RDX, 0, BINARY_GP_R8) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4)) {
    return 0;
  }

  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R8)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_minmax_i32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1) {
    return 0;
  }
  b = &context->code;

  /* r10=min, r11=max, rcx=walk, r8=end, xmm4/xmm5 extrema */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(
          generator, context, &instruction->arguments[0], BINARY_GP_R11) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R8, BINARY_GP_RDX) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R8, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R8, BINARY_GP_RCX) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_broadcast_i32_to_ymm(b, 4, BINARY_GP_R10) ||
      !wcs_broadcast_i32_to_ymm(b, 5, BINARY_GP_R11) ||
      /* Keep the GP min/max accumulators sign-extended so the 64-bit signed
       * compares in the scalar tail and final reduce stay correct for
       * negative extrema (broadcasts above already used the low 32 bits). */
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R10, BINARY_GP_R10) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R11, BINARY_GP_R11)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }

  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R9, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* AVX2: two 32-byte loads cover the same 64-byte block the SSE path used,
   * folding 8 int32 per vpminsd/vpmaxsd into the ymm4/ymm5 running extrema. */
  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vpminsd_ymm(b, 4, 4, 0) ||
      !wcs_avx_vpmaxsd_ymm(b, 5, 5, 0) ||
      !wcs_avx_vmovdqu_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vpminsd_ymm(b, 4, 4, 0) ||
      !wcs_avx_vpmaxsd_ymm(b, 5, 5, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !binary_emit_mov_reg_mem32(b, BINARY_GP_R14, BINARY_GP_RCX, 0) ||
      !binary_emit_movsxd_reg_reg32(b, BINARY_GP_R14, BINARY_GP_R14) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R14, BINARY_GP_R10) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4C /* cmovl */, BINARY_GP_R10,
                                  BINARY_GP_R14) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_R14, BINARY_GP_R11) ||
      !binary_emit_cmovcc_reg_reg(b, 0x4F /* cmovg */, BINARY_GP_R11,
                                  BINARY_GP_R14) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Fold the upper 128-bit lanes of ymm4/ymm5 down onto the lower lanes, drop
   * the AVX upper state (vzeroupper) before the SSE horizontal reduce, then
   * collapse the 4 surviving lanes to the scalar min/max. */
  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vextracti128(b, 0, 4, 1) ||
      !wcs_avx_vextracti128(b, 1, 5, 1) ||
      !wcs_avx_vzeroupper(b) ||
      !wcs_pminsd(b, 4, 0) ||
      !wcs_pmaxsd(b, 5, 1) ||
      !wcs_horizontal_pminsd_to_reg(b, 4, BINARY_GP_R10) ||
      !wcs_horizontal_pmaxsd_to_reg(b, 5, BINARY_GP_R11) ||
      !code_generator_binary_emit_destination_store(generator, context,
                                                    &instruction->dest,
                                                    BINARY_GP_R10) ||
      !code_generator_binary_emit_destination_store(
          generator, context, &instruction->arguments[0], BINARY_GP_R11)) {
    return 0;
  }
  return 1;
}

/* -------------------------------------------------------------------------- */
/* Vectorized search skip-ahead (IR_OP_SIMD_FIND).                             */
/*                                                                             */
/* Computes dest(iv) = the exact first index in [0, n) where the loop's exit   */
/* predicate `a[i] PRED rhs` holds, else n. The original scalar loop survives  */
/* and re-runs from that index (see the opcode doc in ir.h), so this kernel    */
/* never reconstructs control flow -- it only fast-forwards the counter.      */
/*                                                                             */
/* Structure: a scalar HEAD walks elements until `a` is 32-byte aligned, then  */
/* the vector loop tests whole ALIGNED 32-byte blocks (8 int32 / 32 bytes per  */
/* step) with vpcmpeq/vpcmpgt + movemask, stopping at the first hit block and  */
/* resolving the exact lane with bsf. Alignment is the soundness trick for     */
/* sentinel searches (n overstates the valid buffer, e.g. strlen with a huge   */
/* bound): an aligned block never crosses a page boundary, so every block      */
/* loaded lies in a page the scalar loop itself would touch (all lanes before  */
/* the hit, plus the hit's own page), and no block after the hit's is read.    */
/* The remaining sub-block tail is left to the surviving scalar loop.          */
/*                                                                             */
/* Registers: RCX walks a, RDX walks b (two-array form), R8 = key (GP),        */
/* R9 = head element scratch, R10 = n, R11 = i (the result), RAX = scratch /   */
/* masks. ymm1 = broadcast key, ymm0/ymm2 = blocks. All volatile.              */
#define VFIND_EQ 0
#define VFIND_NE 1
#define VFIND_LT 2
#define VFIND_GT 3
#define VFIND_LE 4
#define VFIND_GE 5
#define VFIND_ASCII_IDENT_END 6

/* jcc condition for `cmp elem32, key32` taking the HIT branch (signed forms
 * for the ordered predicates; the recognizer gates signedness). */
static unsigned char vfind_hit_cc(int pred) {
  switch (pred) {
  case VFIND_EQ: return 0x84; /* je */
  case VFIND_NE: return 0x85; /* jne */
  case VFIND_LT: return 0x8C; /* jl */
  case VFIND_GT: return 0x8F; /* jg */
  case VFIND_LE: return 0x8E; /* jle */
  default: return 0x8D;       /* jge */
  }
}

/* Find the first byte outside [0-9A-Z_a-z], starting at an existing absolute
 * index. PCMPISTRI tests 16 bytes at once and returns the first rejected byte
 * in ECX. A block is read only when it stays within the current page. If fewer
 * than 16 bytes remain, or the block would cross a page, the surviving scalar
 * loop handles the tail. */
static int code_generator_binary_emit_simd_find_ascii_ident(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = &context->code;
  if (instruction->argument_count != 5 ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R10) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[4],
                                               BINARY_GP_R11) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RDX, BINARY_GP_R11) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                 UINT64_C(0x7A615F5F5A413930)) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX)) {
    return 0;
  }

  size_t top = b->size;
  size_t to_done[4];
  size_t n_done = 0;
  size_t to_hit = 0;
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R10) ||
      !wcs_jcc(b, 0x8D /* jge: start >= n */, &to_done[n_done++]) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RDX) ||
      !binary_emit_and_reg_imm32(b, BINARY_GP_RAX, 4095) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_RAX, 4080) ||
      !wcs_jcc(b, 0x87 /* ja: a 16-byte read would cross the page */,
               &to_done[n_done++]) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_sub_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !wcs_cmp_reg_imm8(b, BINARY_GP_RAX, 16) ||
      !wcs_jcc(b, 0x8C /* jl: leave the short tail scalar */,
               &to_done[n_done++]) ||
      !wcs_sse42_pcmpistri_ranges_mem(b, BINARY_XMM0, BINARY_GP_RDX,
                                      0x14) ||
      !wcs_cmp_reg_imm8(b, BINARY_GP_RCX, 16) ||
      !wcs_jcc(b, 0x85 /* jne: ECX is the first rejected byte */,
               &to_hit) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 16) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 16)) {
    return 0;
  }
  {
    size_t back = 0;
    if (!wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, top) ||
        !wcs_patch_here(b, to_hit) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
      return 0;
    }
  }
  for (size_t i = 0; i < n_done; i++) {
    if (!wcs_patch_here(b, to_done[i])) return 0;
  }
  if (!code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_R11)) {
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_find(CodeGenerator *generator,
                                         BinaryFunctionContext *context,
                                         const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  if (!generator || !context || !instruction ||
      (instruction->argument_count != 4 && instruction->argument_count != 5) ||
      !instruction->arguments ||
      instruction->dest.kind != IR_OPERAND_SYMBOL ||
      (instruction->rhs.kind != IR_OPERAND_SYMBOL &&
       instruction->rhs.kind != IR_OPERAND_TEMP)) {
    code_generator_set_error(generator, "Malformed simd_find");
    return 0;
  }
  b = &context->code;
  const IROperand *args = instruction->arguments;
  int pred = (int)args[0].int_value;
  int u8 = (int)args[1].int_value == 1;
  int rhs_kind = (int)args[2].int_value;
  const IROperand *rhs = &args[3];
  if (pred == VFIND_ASCII_IDENT_END) {
    if (!u8 || rhs_kind != 0 || instruction->argument_count != 5 ||
        args[3].kind != IR_OPERAND_INT) {
      code_generator_set_error(generator, "Bad ASCII class simd_find encoding");
      return 0;
    }
    return code_generator_binary_emit_simd_find_ascii_ident(
        generator, context, instruction);
  }
  if (instruction->argument_count != 4 ||
      instruction->rhs.kind != IR_OPERAND_SYMBOL || pred < VFIND_EQ ||
      pred > VFIND_GE) {
    code_generator_set_error(generator, "Bad simd_find encoding");
    return 0;
  }
  const int lanes = u8 ? 32 : 8;
  const int esz = u8 ? 1 : 4;
  const int two_arrays = (rhs_kind == 2);
  const int invert_mask = (pred == VFIND_NE || pred == VFIND_LE ||
                           pred == VFIND_GE);
  const uint32_t full_mask = u8 ? 0xFFFFFFFFu : 0xFFu;
  if (pred < VFIND_EQ || pred > VFIND_GE || rhs_kind < 0 || rhs_kind > 2 ||
      (u8 && pred != VFIND_EQ && pred != VFIND_NE)) {
    code_generator_set_error(generator, "Bad simd_find encoding");
    return 0;
  }

  /* ---- setup ---- */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R10) || /* n */
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RCX)) { /* a */
    return 0;
  }
  if (two_arrays) {
    if (!code_generator_binary_emit_operand_load(generator, context, rhs,
                                                 BINARY_GP_RDX)) { /* b */
      return 0;
    }
  } else if (rhs_kind == 0) { /* literal key (range-gated by the recognizer) */
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_R8,
                                   (uint64_t)rhs->int_value)) {
      return 0;
    }
  } else { /* invariant scalar key (type-gated: extension matches the width) */
    if (!code_generator_binary_emit_operand_load(generator, context, rhs,
                                                 BINARY_GP_R8)) {
      return 0;
    }
  }
  if (!two_arrays) { /* broadcast the key once: ymm1 */
    if (u8) {
      if (!wcs_avx_vmovd_xmm_reg(b, 1, BINARY_GP_R8) ||
          !wcs_avx_vpbroadcastb_ymm(b, 1, 1)) {
        return 0;
      }
    } else if (!wcs_broadcast_i32_to_ymm(b, 1, BINARY_GP_R8)) {
      return 0;
    }
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_R11, 0)) { /* i = 0 */
    return 0;
  }

  size_t to_done[3];
  size_t n_done = 0;

  /* ---- scalar head: until `a` is 32-byte aligned (or i == n / a hit) ---- */
  size_t head_top = b->size;
  size_t j_vec = 0;
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 31) ||
      !wcs_and_reg_reg(b, BINARY_GP_RAX, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x84 /* jz -> vector loop */, &j_vec)) {
    return 0;
  }
  if (!wcs_cmp_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R10) ||
      !wcs_jcc(b, 0x8D /* jge -> done (i >= n) */, &to_done[n_done])) {
    return 0;
  }
  n_done++;
  if (u8) {
    if (!wcs_movzx_reg_byte_mem(b, BINARY_GP_R9, BINARY_GP_RCX)) {
      return 0;
    }
  } else if (!code_generator_binary_emit_load_from_address(
                 generator, context, BINARY_GP_RCX, 4, BINARY_GP_R9)) {
    return 0;
  }
  if (two_arrays) {
    int ok = u8 ? wcs_movzx_reg_byte_mem(b, BINARY_GP_RAX, BINARY_GP_RDX)
                : code_generator_binary_emit_load_from_address(
                      generator, context, BINARY_GP_RDX, 4, BINARY_GP_RAX);
    if (!ok || !wcs_cmp_reg_reg32(b, BINARY_GP_R9, BINARY_GP_RAX)) {
      return 0;
    }
  } else if (!wcs_cmp_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8)) {
    return 0;
  }
  if (!wcs_jcc(b, vfind_hit_cc(pred), &to_done[n_done])) { /* hit at i */
    return 0;
  }
  n_done++;
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)esz) ||
      (two_arrays &&
       !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, (unsigned char)esz)) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 1)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, head_top)) {
      return 0;
    }
  }

  /* ---- vector loop over aligned 32-byte blocks ---- */
  if (!wcs_patch_here(b, j_vec)) {
    return 0;
  }
  size_t vec_top = b->size;
  size_t j_hit = 0;
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_RAX, BINARY_GP_R10) ||
      !wcs_sub_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R11) ||
      !wcs_cmp_reg_imm8(b, BINARY_GP_RAX, (unsigned char)lanes) ||
      !wcs_jcc(b, 0x8C /* jl -> done (remaining < lanes; n may be < 0) */,
               &to_done[n_done])) {
    return 0;
  }
  n_done++;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0)) {
    return 0;
  }
  if (two_arrays && !wcs_avx_vmovups_ymm_mem(b, 2, BINARY_GP_RDX, 0)) {
    return 0;
  }
  {
    int src2 = two_arrays ? 2 : 1;
    int ok = 0;
    switch (pred) {
    case VFIND_EQ:
    case VFIND_NE:
      ok = u8 ? wcs_avx_vpcmpeqb_ymm(b, 0, 0, src2)
              : wcs_avx_vpcmpeqd_ymm(b, 0, 0, src2);
      break;
    case VFIND_GT:
    case VFIND_LE:
      ok = wcs_avx_vpcmpgtd_ymm(b, 0, 0, src2);
      break;
    default: /* LT / GE: src2 > a */
      ok = wcs_avx_vpcmpgtd_ymm(b, 0, src2, 0);
      break;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!(u8 ? wcs_avx_vpmovmskb_reg_ymm(b, BINARY_GP_RAX, 0)
           : wcs_avx_vmovmskps_reg_ymm(b, BINARY_GP_RAX, 0))) {
    return 0;
  }
  if (invert_mask && !wcs_xor_reg_imm32(b, BINARY_GP_RAX, full_mask)) {
    return 0;
  }
  if (!wcs_test_reg_reg32(b, BINARY_GP_RAX) ||
      !wcs_jcc(b, 0x85 /* jnz -> hit block */, &j_hit)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      (two_arrays && !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)lanes)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, vec_top)) {
      return 0;
    }
  }

  /* hit block: resolve the exact first lane. */
  if (!wcs_patch_here(b, j_hit) ||
      !wcs_bsf_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RAX)) {
    return 0;
  }

  /* done: i is the result (high bits are zero; <= n fits every int home). */
  for (size_t k = 0; k < n_done; k++) {
    if (!wcs_patch_here(b, to_done[k])) {
      return 0;
    }
  }
  if (!wcs_avx_vzeroupper(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_R11);
}
