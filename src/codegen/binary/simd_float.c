#include "codegen/binary/internal.h"
#include "codegen/binary/simd_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Floating-point SIMD kernels (AVX2 + FMA3 horizontal sums, dot products, affine maps). Encoders live in simd_encoders.c; see simd_internal.h. */

/* Horizontal sum of base[0..len-1] doubles, ADDED to dest's prior value.
 * dest = float64 sum symbol, lhs = base pointer, rhs = element count. */
int code_generator_binary_emit_simd_sum_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  /* rax=prior bits, rcx=walk, r9=end, r10=bytes-remaining scratch.
   * xmm3=scalar running total, ymm2/ymm4=packed accumulators, ymm0/1=scratch. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator unroll: 8 doubles/iter summed into the independent ymm2 and
   * ymm4 chains so the ~4-cycle vaddpd latency overlaps instead of serializing
   * a single accumulator. The 32-byte and scalar tiers below mop up the < 64B
   * remainder once the loop re-dispatches. */
  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RCX, 32) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 0) ||
      !wcs_avx_vaddpd_ymm(b, 4, 4, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !wcs_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Fold the second accumulator in before the horizontal reduce. */
  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 4) ||
      !wcs_reduce_pd_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Horizontal sum of base[0..len-1] floats, ADDED to dest's prior value.
 * dest = float32 sum symbol, lhs = base pointer, rhs = element count. */
int code_generator_binary_emit_simd_sum_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction) {
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator unroll: 16 floats/iter into the independent ymm2 and ymm4
   * chains; the 32-byte and scalar tiers handle the < 64B remainder. */
  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RCX, 32) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vaddps_ymm(b, 4, 4, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_scalar) ||
      !wcs_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !binary_emit_addss_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4) ||
      !wcs_reduce_ps_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Float64 dot product of a[0..n-1]*b[0..n-1], ADDED to dest's prior value.
 * dest = float64 sum, lhs = a, rhs = b, arguments[0] = element count. */
int code_generator_binary_emit_simd_dot_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_f64");
    return 0;
  }
  b = &context->code;

  /* rcx=a walk, rdx=b walk, r9=a_end, r10=scratch, rax=prior/result.
   * xmm3=scalar total, ymm2/ymm4=packed FMA accumulators, ymm0/ymm1=scratch. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator FMA unroll: 8 elements/iter. Each vfmadd231pd folds a*b
   * into its accumulator in a single rounding step, and the two chains (ymm2,
   * ymm4) run independently so the FMA latency is hidden. */
  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231pd_ymm(b, 2, 0, 1) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 32) ||
      !wcs_avx_vfmadd231pd_ymm(b, 4, 0, 1) ||
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

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231pd_ymm(b, 2, 0, 1) ||
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
      !wcs_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movsd_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_fmadd231sd(b, 3, 0, 1) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_done) ||
      !wcs_avx_vaddpd_ymm(b, 2, 2, 4) ||
      !wcs_reduce_pd_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* One scalar float64 element of the affine map, advancing both pointers.
 * Shared by the alignment peel and the remainder tail. */
static int wcs_affine_f64_scalar_step(BinaryCodeBuffer *b, int b_is_one,
                                      int b_is_zero, int c_is_zero) {
  if (!wcs_movsd_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movsd_xmm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_fmadd231sd(b, 1, 0, 4) ||
        !wcs_movsd_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM1)) {
      return 0;
    }
  } else {
    if (!binary_emit_mulsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM4)) {
      return 0;
    }
    if (!b_is_zero && !wcs_fmadd231sd(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !binary_emit_addsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM3)) {
      return 0;
    }
    if (!wcs_movsd_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM0)) {
      return 0;
    }
  }
  return wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 8) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 8);
}

/* The vectorized + scalar-tail loop of the float64 affine map, factored so the
 * fallback lowering and the MIR inline passthrough (MIR_SIMD_AFFINE_MAP_F64)
 * share one kernel. Assumes RCX = src (iterated), RDX = dst (output), R9 = src
 * end pointer (src + count*8), and the broadcast coefficients already in ymm4
 * (a), ymm5 (b), ymm3 (c). Emits the closing vzeroupper. */
static int wcs_affine_alias_guard(BinaryCodeBuffer *b, size_t *j_alias) {
  size_t j_same = 0, j_after = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_RCX) ||
      !wcs_jcc(b, 0x84 /* je */, &j_same) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_after)) {
    return 0;
  }
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x82 /* jb */, j_alias)) {
    return 0;
  }
  return wcs_patch_here(b, j_same) && wcs_patch_here(b, j_after);
}

int code_generator_binary_emit_simd_affine_map_f64_loop(BinaryCodeBuffer *b,
                                                        int b_is_one,
                                                        int b_is_zero,
                                                        int c_is_zero) {
  size_t loop_top = 0, j_done = 0, j_scalar = 0;
  size_t j_alias = 0;
  if (!wcs_affine_alias_guard(b, &j_alias)) {
    return 0;
  }

  /* The dst += a*src form first runs a 64-byte (8 doubles) unrolled loop: its
   * body is only load/load/fma/store twice, so at one chunk per iteration the
   * loop bookkeeping (two pointer adds, compare, branch) was a third of the
   * instruction stream. ymm2/ymm3 are free here (b and c are folded away in
   * this form), so the second chunk overlaps the first with no spill. Any
   * remaining 32-byte chunk falls through to the single-chunk loop below,
   * which then runs at most once, and the scalar tail is shared. */
  if (b_is_one && c_is_zero) {
    size_t j_skip64 = 0, top64 = 0;
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R11, 1 /* shr */, 6) ||
        !wcs_shift_reg_imm(b, BINARY_GP_R11, 0 /* shl */, 6) ||
        !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
      return 0;
    }
    if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
        !wcs_jcc(b, 0x83 /* jae */, &j_skip64)) {
      return 0;
    }
    top64 = b->size;
    if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
        !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
        !wcs_avx_vfmadd231pd_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1) ||
        !wcs_avx_vmovups_ymm_mem(b, 2, BINARY_GP_RCX, 32) ||
        !wcs_avx_vmovups_ymm_mem(b, 3, BINARY_GP_RDX, 32) ||
        !wcs_avx_vfmadd231pd_ymm(b, 3, 2, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 32, 3)) {
      return 0;
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 64) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 64) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 /* jb */, &j_back) ||
          !wcs_patch_to(b, j_back, top64)) {
        return 0;
      }
    }
    if (!wcs_patch_here(b, j_skip64)) {
      return 0;
    }
  }

  /* vec_end (r11) = src_start + (src_end - src_start) rounded down to a 32-byte
   * multiple (4 doubles/chunk). Hoisting the strip-mine bound out of the loop
   * keeps the hot vector body free of the per-iteration remainder recompute. */
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 1 /* shr */, 5) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 0 /* shl */, 5) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
    return 0;
  }

  /* Vector loop: entry guard + bottom-tested body (one taken branch/iter). */
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_scalar)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    /* dst += a*src (one fma into the dst vector). */
    if (!wcs_avx_vfmadd231pd_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1)) {
      return 0;
    }
  } else {
    if (!wcs_avx_vmulpd_ymm(b, 0, 0, 4)) {
      return 0;
    }
    if (!b_is_zero && !wcs_avx_vfmadd231pd_ymm(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !wcs_avx_vaddpd_ymm(b, 0, 0, 3)) {
      return 0;
    }
    if (!wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 0)) {
      return 0;
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0x82 /* jb */, &j_back) ||
        !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Scalar tail: 1 double/iter up to src_end (r9), entry guard + bottom test. */
  if (!wcs_patch_here(b, j_scalar) || !wcs_patch_here(b, j_alias) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }
  {
    size_t scalar_top = b->size;
    if (!wcs_affine_f64_scalar_step(b, b_is_one, b_is_zero, c_is_zero) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 /* jb */, &j_back) ||
          !wcs_patch_to(b, j_back, scalar_top)) {
        return 0;
      }
    }
  }

  return wcs_patch_here(b, j_done) && wcs_avx_vzeroupper(b);
}

/* MIR inline passthrough entry: materialize the coefficient broadcasts (a->ymm4,
 * b->ymm5, c->ymm3 from their raw 64-bit IEEE bits), compute the src end pointer
 * R9 = RCX + count*8, then run the shared loop. Assumes RCX = src, RDX = dst,
 * R8 = count (marshalled by the MIR lowering). */
int code_generator_binary_emit_simd_affine_map_f64_inline(
    BinaryCodeBuffer *b, unsigned long long a_bits, unsigned long long b_bits,
    unsigned long long c_bits, int b_is_one, int b_is_zero, int c_is_zero,
    int a_runtime) {
  /* a -> ymm4. When a is a runtime scale the lowering has already placed its
   * scalar in XMM4, so just broadcast it; otherwise materialize the immediate. */
  if (a_runtime) {
    if (!wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4)) {
      return 0;
    }
  } else if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (long long)a_bits) ||
             !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
             !wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4)) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (long long)b_bits) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM5, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 5, 5) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (long long)c_bits) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 3) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  return code_generator_binary_emit_simd_affine_map_f64_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

/* Float64 affine map: dst[i] = a * src[i] + b * dst[i] + c.
 * lhs=src, rhs=dst, arguments[0]=count, [1]=a, [2]=b, [3]=c. */
int code_generator_binary_emit_simd_affine_map_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 4 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_affine_map_f64");
    return 0;
  }
  b = &context->code;

  /* Identity-fold the affine coefficients when they are compile-time constants:
   * b == 1 means the dst term is just +dst[i] (no scale), and c == 0 means no
   * bias add. The common saxpy form `dst = a*src + dst` (b==1, c==0) then
   * collapses to a single fused multiply-add per vector instead of mul+fma+add. */
  int b_is_one = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                 instruction->arguments[2].float_value == 1.0;
  /* b==0 (a constant dst coefficient of zero) means the `b*dst[i]` term must
   * be DROPPED, not computed: dst is the output array and may hold
   * uninitialized NaN/Inf where 0*x is NaN, not 0. A pure copy out[i]=src[i]
   * lowers to a=1,b=0,c=0 and must never read its own garbage output. */
  int b_is_zero = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[2].float_value == 0.0;
  int c_is_zero = instruction->arguments[3].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[3].float_value == 0.0;

  /* rcx=src walk, rdx=dst walk, r9=src_end; ymm4=a, ymm5=b, ymm3=c. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 4, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM5, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 5, 5) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[3],
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 3) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 3) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  return code_generator_binary_emit_simd_affine_map_f64_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

/* The vectorized + scalar-tail loop of the float32 affine map, factored so the
 * fallback lowering and the MIR inline passthrough (MIR_SIMD_AFFINE_MAP_F32)
 * share one kernel. Assumes RCX = src (iterated), RDX = dst (output), R9 = src
 * end pointer (src + count*4), and the broadcast coefficients already in ymm4
 * (a), ymm5 (b), ymm3 (c). Emits the closing vzeroupper. */
int code_generator_binary_emit_simd_affine_map_f32_loop(BinaryCodeBuffer *b,
                                                        int b_is_one,
                                                        int b_is_zero,
                                                        int c_is_zero) {
  size_t loop_top = 0, j_done = 0, j_scalar = 0;
  size_t j_alias = 0;
  if (!wcs_affine_alias_guard(b, &j_alias)) {
    return 0;
  }

  /* vec_end (r11) = src_start + (src_end - src_start) rounded down to a 32-byte
   * multiple (8 floats/chunk). Hoisting the strip-mine bound out of the loop
   * keeps the hot vector body free of the per-iteration remainder recompute. */
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R11, BINARY_GP_RCX) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 1 /* shr */, 5) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R11, 0 /* shl */, 5) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R11, BINARY_GP_RCX)) {
    return 0;
  }

  /* Vector loop: entry guard + bottom-tested body (one taken branch/iter). */
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_scalar)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (b_is_one && c_is_zero) {
    if (!wcs_avx_vfmadd231ps_ymm(b, 1, 0, 4) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 1)) {
      return 0;
    }
  } else {
    if (!wcs_avx_vmulps_ymm(b, 0, 0, 4)) {
      return 0;
    }
    if (!b_is_zero && !wcs_avx_vfmadd231ps_ymm(b, 0, 5, 1)) {
      return 0;
    }
    if (!c_is_zero && !wcs_avx_vaddps_ymm(b, 0, 0, 3)) {
      return 0;
    }
    if (!wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RDX, 0, 0)) {
      return 0;
    }
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R11)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0x82 /* jb */, &j_back) ||
        !wcs_patch_to(b, j_back, loop_top)) {
      return 0;
    }
  }

  /* Scalar tail: 1 float/iter up to src_end (r9), entry guard + bottom test. */
  if (!wcs_patch_here(b, j_scalar) || !wcs_patch_here(b, j_alias) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done)) {
    return 0;
  }
  {
    size_t scalar_top = b->size;
    if (!wcs_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
        !wcs_movss_xmm_mem(b, 1, BINARY_GP_RDX, 0)) {
      return 0;
    }
    if (b_is_one && c_is_zero) {
      if (!wcs_fmadd231ss(b, 1, 0, 4) ||
          !wcs_movss_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM1)) {
        return 0;
      }
    } else if (!binary_emit_mulss_xmm_xmm(b, BINARY_XMM0, BINARY_XMM4) ||
               (!b_is_zero && !wcs_fmadd231ss(b, 0, 5, 1)) ||
               (!c_is_zero &&
                !binary_emit_addss_xmm_xmm(b, BINARY_XMM0, BINARY_XMM3)) ||
               !wcs_movss_mem_xmm(b, BINARY_GP_RDX, 0, BINARY_XMM0)) {
      return 0;
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4) ||
        !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9)) {
      return 0;
    }
    {
      size_t j_back = 0;
      if (!wcs_jcc(b, 0x82 /* jb */, &j_back) ||
          !wcs_patch_to(b, j_back, scalar_top)) {
        return 0;
      }
    }
  }
  return wcs_patch_here(b, j_done) && wcs_avx_vzeroupper(b);
}

/* MIR inline passthrough entry: materialize the coefficient broadcasts (a->ymm4,
 * b->ymm5, c->ymm3 from their raw 32-bit IEEE bits), compute the src end pointer
 * R9 = RCX + count*4, then run the shared loop. Assumes RCX = src, RDX = dst,
 * R8 = count (marshalled by the MIR lowering). The fallback loads the
 * coefficients from their operands instead and shares only the loop. */
int code_generator_binary_emit_simd_affine_map_f32_inline(
    BinaryCodeBuffer *b, unsigned a_bits, unsigned b_bits, unsigned c_bits,
    int b_is_one, int b_is_zero, int c_is_zero, int a_runtime) {
  /* a -> ymm4. When a is a runtime scale the lowering has already placed its
   * scalar in XMM4, so just broadcast it; otherwise materialize the immediate. */
  if (a_runtime) {
    if (!wcs_avx_vpbroadcastd_ymm(b, 4, 4)) {
      return 0;
    }
  } else if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, a_bits) ||
             !wcs_movd_xmm_reg(b, 4, BINARY_GP_RAX) ||
             !wcs_avx_vpbroadcastd_ymm(b, 4, 4)) {
    return 0;
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, b_bits) ||
      !wcs_movd_xmm_reg(b, 5, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, c_bits) ||
      !wcs_movd_xmm_reg(b, 3, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  return code_generator_binary_emit_simd_affine_map_f32_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

/* Float32 affine map: dst[i] = a * src[i] + b * dst[i] + c.
 * lhs=src, rhs=dst, arguments[0]=count, [1]=a, [2]=b, [3]=c. */
int code_generator_binary_emit_simd_affine_map_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 4 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_affine_map_f32");
    return 0;
  }
  b = &context->code;

  /* See the f64 variant: fold b==1 (no dst scale) and c==0 (no bias); the saxpy
   * form b==1,c==0 collapses mul+fma+add into a single fused multiply-add. */
  int b_is_one = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                 instruction->arguments[2].float_value == 1.0;
  /* b==0: drop the `b*dst` term (0*NaN==NaN; dst may be uninitialized). */
  int b_is_zero = instruction->arguments[2].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[2].float_value == 0.0;
  int c_is_zero = instruction->arguments[3].kind == IR_OPERAND_FLOAT &&
                  instruction->arguments[3].float_value == 0.0;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[1],
                                               BINARY_GP_RAX) ||
      !wcs_movd_xmm_reg(b, 4, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 4, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[2],
                                               BINARY_GP_RAX) ||
      !wcs_movd_xmm_reg(b, 5, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 5, 5) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[3],
                                               BINARY_GP_RAX) ||
      !wcs_movd_xmm_reg(b, 3, BINARY_GP_RAX) ||
      !wcs_avx_vpbroadcastd_ymm(b, 3, 3) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }

  return code_generator_binary_emit_simd_affine_map_f32_loop(b, b_is_one,
                                                             b_is_zero,
                                                             c_is_zero);
}

/* General auto-vectorized loop kernel (IR_OP_SIMD_VLOOP_F64 and its integer
 * twin IR_OP_SIMD_VLOOP_I32). Decodes the serialized body DAG (see the opcode
 * doc in ir.h) and emits a packed AVX2 loop + scalar remainder for element-wise
 * maps out[i] = DAG(a_k[i], i, consts, scalars) and '+' reductions. The element
 * kind comes from the opcode + instruction->float_bits: f64x4 lanes (pd ops,
 * 8B elements), f32x8 lanes (ps ops, 4B), or i32x8 lanes (vpaddd/vpsubd/
 * vpmulld/vpand/vpor/vpxor/vpslld, 4B), all walk array bases by 32B per
 * vector iteration. The DAG is replayed via a stack-machine of up to
 * VLOOP_KERNEL_REGS ymm registers; constants AND runtime invariant scalars are
 * broadcast once to a stack array and re-read. Float maps are bit-identical to
 * the scalar loop (each lane is an independent IEEE op); float '+' reductions
 * reassociate like the sum/dot kernels. Integer maps and reductions are
 * bit-exact (every op congruent mod 2^32, '+' associative).
 *
 * The integer scalar tail reuses the full-width ymm ALU ops on a zero-upper
 * invariant: every tail leaf loads via VEX vmovd (zeroes bits 32..255), and
 * all supported int ops preserve zero lanes, so lane 0 is exact and lanes 1..7
 * stay zero with no transition penalty and no extra 128-bit encoders. */
#define VLOOP_K_LOAD 0
#define VLOOP_K_IOTA 1
#define VLOOP_K_CONST 2
#define VLOOP_K_ADD 3
#define VLOOP_K_SUB 4
#define VLOOP_K_MUL 5
#define VLOOP_K_DIV 6
#define VLOOP_K_SCALAR 7
#define VLOOP_K_AND 8
#define VLOOP_K_OR 9
#define VLOOP_K_XOR 10
#define VLOOP_K_SHL 11
#define VLOOP_K_SAR 12
#define VLOOP_K_SHR 13
#define VLOOP_K_MIN 14
#define VLOOP_K_MAX 15
#define VLOOP_K_CMPGT 16
#define VLOOP_K_PAIR 17
#define VLOOP_K_SELECT 18
#define VLOOP_K_CMPEQ 19
#define VLOOP_KERNEL_REGS 4
#define VLOOP_KERNEL_MAX_NODES 48
#define VLOOP_KERNEL_MAX_BASES 4
#define VLOOP_KERNEL_POOL_MAX 6

/* ymm registers a map's node stack may use. ymm0/1/2/4 always; ymm3 unless a
 * byte map needs it for the store mask; ymm5 unless an iota occupies it. The
 * recognizer and the kernel both size the pool here so neither can outrun the
 * other. */
int code_generator_vloop_pool_size(int elem8, int has_iota) {
  return 4 + (elem8 ? 0 : 1) + (has_iota ? 0 : 1);
}

static int vloop_kernel_tag_is_leaf(int tag) {
  return tag == VLOOP_K_LOAD || tag == VLOOP_K_IOTA || tag == VLOOP_K_CONST ||
         tag == VLOOP_K_SCALAR;
}

/* The distinct walking-pointer base operands of a vloop, in kGp order: dest base
 * first (maps), then each loaded array not already seen. Shared by the kernel
 * and the MIR passthrough lowering so both agree on which base lands in which
 * register. Returns -1 if there are more than VLOOP_KERNEL_MAX_BASES. */
int code_generator_vloop_collect_dist(const IRInstruction *in, int is_reduce,
                                      const char *names[4],
                                      const IROperand *srcs[4], int *n_out) {
  int n_arrays = (int)in->arguments[1].int_value;
  int n = 0;
  if (!is_reduce) {
    names[0] = in->dest.name;
    srcs[0] = &in->dest;
    n = 1;
  }
  for (int k = 0; k < n_arrays; k++) {
    const IROperand *as = &in->arguments[7 + k];
    const char *nm = as->name;
    int found = 0;
    for (int j = 0; j < n; j++)
      if (names[j] && nm && strcmp(names[j], nm) == 0) { found = 1; break; }
    if (!found) {
      if (n >= VLOOP_KERNEL_MAX_BASES) return -1;
      names[n] = nm;
      srcs[n] = as;
      n++;
    }
  }
  *n_out = n;
  return 0;
}

/* Table-shaped entry for the MIR kernel bridge: reductions run against staged
 * frame slots (operands_marshaled=0), unlike the map passthrough. */
int code_generator_binary_emit_simd_vloop_unmarshaled(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  return code_generator_binary_emit_simd_vloop_f64(generator, context,
                                                   instruction, 0);
}

int code_generator_binary_emit_simd_vloop_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction, int operands_marshaled) {
  BinaryCodeBuffer *b = NULL;
  /* ymm node-eval pools (volatile). A map's is sized below, once it is known
   * whether ymm3 and ymm5 are spoken for. '+' reductions reserve ymm2 = packed
   * accumulator and xmm3 = scalar/prior accumulator, so their node pool is
   * ymm0,1,4 (3-deep). ymm5 = iota const. */
  static const int kPoolReduce[3] = {0, 1, 4};
  static const BinaryGpRegister kGp[VLOOP_KERNEL_MAX_BASES] = {
      BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};
  const int IOTA_CONST = 5;
  /* ymm3 is the reduction's scalar accumulator; a byte MAP has no accumulator,
   * so it borrows the register for the low-byte mask its store needs. */
  const int BYTE_MASK = 3;

  if (!generator || !context || !instruction || instruction->argument_count < 7 ||
      !instruction->arguments || instruction->dest.kind != IR_OPERAND_SYMBOL) {
    code_generator_set_error(generator, "Malformed simd_vloop");
    return 0;
  }
  const int i32 = (instruction->op == IR_OP_SIMD_VLOOP_I32);
  if (!i32 && instruction->float_bits != 32 && instruction->float_bits != 64) {
    code_generator_set_error(generator, "simd_vloop bad float width");
    return 0;
  }
  /* Byte elements with int32 lanes: the arithmetic is unchanged (every op
   * congruent mod 2^32, so the lanes reproduce the scalar loop exactly) and
   * only the traffic at either end narrows -- widen eight bytes into the lanes
   * on load, truncate back to eight bytes on store. instruction->is_unsigned
   * says which widening. */
  const int elem8 = i32 && instruction->float_bits == 8;
  const int elem8_unsigned = elem8 && instruction->is_unsigned;
  if (i32 && instruction->float_bits != 32 && instruction->float_bits != 8) {
    code_generator_set_error(generator, "simd_vloop bad int element width");
    return 0;
  }
  b = &context->code;
  /* f32 = single precision: f32x8 lanes (ps ops, 4-byte elements); f64x4 (pd
   * ops, 8-byte elements) otherwise. Int lanes are i32x8. All three stride 32
   * bytes per vector iter, and i32 shares the f32 4-byte layout everywhere a
   * raw 32-bit pattern is moved (loads, stores, broadcast slots). */
  const int f32 = i32 || (instruction->float_bits == 32);
  const int lanes = f32 ? 8 : 4;
  const int elem_bytes = elem8 ? 1 : (f32 ? 4 : 8);
  /* Bytes advance a base by one vector's worth of ELEMENTS, not of register. */
  const int vec_stride = elem_bytes * lanes;

  const IROperand *args = instruction->arguments;
  long long reduce_op = args[0].int_value;
  int n_arrays = (int)args[1].int_value;
  int n_nodes = (int)args[2].int_value;
  int root = (int)args[3].int_value;
  int n_consts = (int)args[4].int_value;
  int n_scalars = (int)args[5].int_value;
  int depth = (int)args[6].int_value;
  /* reduce_op: 0 = element-wise map, 1 = '+' reduction, 2 = max, 3 = min. */
  const int is_minmax = (reduce_op == 2 || reduce_op == 3);
  const int is_max = (reduce_op == 2);
  int is_reduce = (reduce_op == 1) || is_minmax;
  size_t expect =
      (size_t)(7 + n_arrays + n_scalars + 3 * n_nodes + n_consts);
  if ((reduce_op < 0 || reduce_op > 3) || n_arrays < 0 ||
      n_arrays > VLOOP_KERNEL_MAX_BASES || n_nodes <= 0 ||
      n_nodes > VLOOP_KERNEL_MAX_NODES || n_consts < 0 || n_scalars < 0 ||
      root < 0 || root >= n_nodes ||
      instruction->argument_count != expect) {
    code_generator_set_error(generator, "Bad simd_vloop encoding");
    return 0;
  }

  size_t scalars_off = (size_t)(7 + n_arrays);
  size_t nodes_off = scalars_off + (size_t)n_scalars;
  size_t consts_off = nodes_off + (size_t)(3 * n_nodes);
  int n_slots = n_consts + n_scalars; /* 32-byte broadcast slots on the stack */

  /* Assign a GP walking-pointer register to each distinct base symbol (the
   * destination plus each loaded array not equal to it). arr_reg[k] = the GP
   * register holding loaded array k's current element address. */
  const char *dist_name[VLOOP_KERNEL_MAX_BASES];
  const IROperand *dist_src[VLOOP_KERNEL_MAX_BASES];
  int n_dist = 0;
  /* For a map, dest is the stored array base (a walking pointer). For a '+'
   * reduction, dest is the scalar accumulator (handled via xmm3/RAX), not a
   * base pointer, so it is not seeded. (Shared with the MIR passthrough.) */
  if (code_generator_vloop_collect_dist(instruction, is_reduce, dist_name,
                                        dist_src, &n_dist) < 0) {
    code_generator_set_error(generator, "simd_vloop_f64 too many bases");
    return 0;
  }
  int has_iota = 0;
  for (int i = 0; i < n_nodes; i++) {
    if ((int)args[nodes_off + 3 * i].int_value == VLOOP_K_IOTA) {
      has_iota = 1;
    }
  }
  /* A map's node pool is every ymm this particular kernel does not otherwise
   * need: ymm3 only carries a byte map's low-byte mask, ymm5 only an iota, so a
   * map with neither has six to spend. ymm6 and up are callee-saved under Win64
   * and stay out of it. code_generator_vloop_pool_size counts the same
   * registers for the recognizer, which refuses a DAG deeper than this. */
  int map_pool[VLOOP_KERNEL_POOL_MAX] = {0, 1, 2, 4, 0, 0};
  int map_pool_n = 4;
  if (!elem8) {
    map_pool[map_pool_n++] = BYTE_MASK;
  }
  if (!has_iota) {
    map_pool[map_pool_n++] = IOTA_CONST;
  }
  const int *kPool = is_reduce ? kPoolReduce : map_pool;
  int pool_n = is_reduce ? 3 : map_pool_n;
  if (pool_n != (is_reduce ? 3 : code_generator_vloop_pool_size(elem8,
                                                                has_iota)) ||
      depth > pool_n) {
    code_generator_set_error(generator, "simd_vloop depth over pool");
    return 0;
  }
  int arr_reg[VLOOP_KERNEL_MAX_BASES];
  for (int k = 0; k < n_arrays; k++) {
    const char *nm = args[7 + k].name; /* array base k (always in dist) */
    int found = 0;
    for (int j = 0; j < n_dist; j++) {
      if (dist_name[j] && nm && strcmp(dist_name[j], nm) == 0) {
        found = j;
        break;
      }
    }
    arr_reg[k] = kGp[found];
  }
  int dst_reg = kGp[0];
  /* r10 = element count (decrements), r11 = i (only if IOTA). rax = scratch. */

  /* Stack scratch for broadcast constants and scalars: 32 bytes each. */
  uint32_t cbytes = (uint32_t)(32 * n_slots);
  if (cbytes && !binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }

  /* Load base pointers and the element count. In the MIR passthrough the bases
   * are already in kGp[0..n_dist-1] and the count in kGp[n_dist] (the lowering
   * marshalled them into ABI arg registers, which -- unlike R10/R11, the MIR
   * encoder scratch -- are safe to write before this kernel); just move the
   * count into R10. The fallback loads everything from the operands. */
  if (operands_marshaled) {
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R10, kGp[n_dist])) {
      return 0;
    }
  } else {
    for (int j = 0; j < n_dist; j++) {
      if (!code_generator_binary_emit_operand_load(generator, context,
                                                   dist_src[j], kGp[j])) {
        return 0;
      }
    }
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->lhs,
                                                 BINARY_GP_R10)) {
      return 0;
    }
  }
  if (has_iota && !binary_emit_mov_reg_imm64(b, BINARY_GP_R11, 0)) {
    return 0;
  }

  /* Broadcast each constant once into its stack slot. For f32/i32 the 32-bit
   * pattern is packed into both halves of a 64-bit word so the shared
   * vbroadcastsd path yields 8 identical lanes; the scalar tail reads the low
   * 4 bytes. */
  for (int c = 0; c < n_consts; c++) {
    uint64_t bits = 0;
    if (i32) {
      uint32_t iv = (uint32_t)(uint64_t)args[consts_off + c].int_value;
      bits = (uint64_t)iv | ((uint64_t)iv << 32);
    } else if (f32) {
      float fv = (float)args[consts_off + c].float_value;
      uint32_t fb = 0;
      memcpy(&fb, &fv, sizeof(fb));
      bits = (uint64_t)fb | ((uint64_t)fb << 32);
    } else {
      double dv = args[consts_off + c].float_value;
      memcpy(&bits, &dv, sizeof(bits));
    }
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, bits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * c, 0)) {
      return 0;
    }
  }
  /* Broadcast each runtime invariant scalar once into its slot (after the
   * literal consts). The recognizer proved the symbol loop-invariant, so one
   * read at entry is identical to the scalar loop's per-iteration reads. */
  for (int s = 0; s < n_scalars; s++) {
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &args[scalars_off + s],
                                                 BINARY_GP_RAX)) {
      return 0;
    }
    if (f32) { /* 32-bit lanes (f32 or i32): bits live in RAX's low dword */
      if (!wcs_broadcast_i32_to_ymm(b, 0, BINARY_GP_RAX)) {
        return 0;
      }
    } else if (!binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
               !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0)) {
      return 0;
    }
    if (!wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * (n_consts + s), 0)) {
      return 0;
    }
  }
  /* iota constant in ymm5 (int32 lanes), if needed: [0,1,2,3] for f64 (only the
   * low xmm feeds vcvtdq2pd), [0,1,2,3,4,5,6,7] for f32. */
  if (has_iota) {
    if (f32) {
      /* Build [0,1,2,3] in xmm0 and [4,5,6,7] in xmm1, then splice xmm1 into the
       * high 128-bit lane via vperm2i128 (imm 0x20: dst.lo<-src1.lo,
       * dst.hi<-src2.lo). The source ymms' high lanes are unread, so any prior
       * garbage there is harmless. */
      if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000100000000ULL) ||
          !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
          !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000300000002ULL) ||
          !binary_emit_movq_xmm_reg(b, BINARY_XMM2, BINARY_GP_RAX) ||
          !wcs_avx_vpunpcklqdq_xmm(b, 0, 0, 2) ||
          !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000500000004ULL) ||
          !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
          !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000700000006ULL) ||
          !binary_emit_movq_xmm_reg(b, BINARY_XMM2, BINARY_GP_RAX) ||
          !wcs_avx_vpunpcklqdq_xmm(b, 1, 1, 2) ||
          !wcs_avx_vperm2i128(b, IOTA_CONST, 0, 1, 0x20)) {
        return 0;
      }
    } else if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                          0x0000000100000000ULL) ||
               !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
               !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX,
                                          0x0000000300000002ULL) ||
               !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
               !wcs_avx_vpunpcklqdq_xmm(b, IOTA_CONST, 0, 1)) {
      return 0;
    }
  }

  /* Reduction: ymm2 = packed accumulator = 0. The floats also park the prior
   * accumulator value in xmm3 now (low dword for f32, low qword for f64); the
   * int form instead loads it into RAX at finalize, because
   * wcs_accumulate_xmm0_i32_to_rax accumulates the lane sum INTO RAX (the
   * sum_i32 convention). */
  if (elem8 && !is_reduce &&
      (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x000000FF000000FFULL) ||
       !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
       !wcs_avx_vbroadcastsd_ymm_xmm(b, BYTE_MASK, 0))) {
    return 0;
  }
  if (is_minmax) {
    /* Seed every lane with the incoming accumulator instead of an identity:
     * min/max has no additive zero, and a broadcast seed is also what makes a
     * short trip count (or none at all) come out exactly right -- the fold at
     * the end then needs no separate scalar partner. */
    if (!code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->dest,
                                                 BINARY_GP_RAX)) {
      return 0;
    }
    if (f32) { /* 32-bit lanes: f32 or i32, both a raw dword broadcast */
      if (!wcs_broadcast_i32_to_ymm(b, 2, BINARY_GP_RAX)) {
        return 0;
      }
    } else if (!binary_emit_movq_xmm_reg(b, BINARY_XMM2, BINARY_GP_RAX) ||
               !wcs_avx_vbroadcastsd_ymm_xmm(b, 2, 2)) {
      return 0;
    }
  } else if (is_reduce) {
    if (i32) {
      if (!wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
        return 0;
      }
    } else if (!code_generator_binary_emit_operand_load(generator, context,
                                                        &instruction->dest,
                                                        BINARY_GP_RAX) ||
               !(f32 ? binary_emit_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX)
                     : binary_emit_movq_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX)) ||
               !wcs_avx_vpxor_ymm(b, 2, 2, 2)) {
      return 0;
    }
  }

  size_t j_overlap[2 * VLOOP_KERNEL_MAX_BASES];
  int n_overlap = 0;
  if (!is_reduce) {
    int shift = 0;
    while ((1 << shift) < elem_bytes) {
      shift++;
    }
    for (int j = 1; j < n_dist; j++) {
      for (int dir = 0; dir < 2; dir++) {
        int lo = dir ? (int)kGp[j] : (int)dst_reg;
        int hi = dir ? (int)dst_reg : (int)kGp[j];
        if (!binary_emit_mov_reg_reg(b, BINARY_GP_RAX, (BinaryGpRegister)hi) ||
            !wcs_sub_reg_reg64(b, BINARY_GP_RAX, lo)) {
          return 0;
        }
        if (shift && !wcs_shift_reg_imm(b, BINARY_GP_RAX, 1,
                                        (unsigned char)shift)) {
          return 0;
        }
        if (!wcs_cmp_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10) ||
            !wcs_jcc(b, 0x82 /* jb */, &j_overlap[n_overlap++])) {
          return 0;
        }
      }
    }
  }

  /* ---- vector loop: while (count >= lanes) ---- */
  size_t vec_top = b->size;
  size_t j_tail = 0;
  if (!wcs_cmp_reg_imm8(b, BINARY_GP_R10, lanes) ||
      !wcs_jcc(b, 0x82 /* jb */, &j_tail)) {
    return 0;
  }
  {
    int pool[VLOOP_KERNEL_POOL_MAX];
    int nfree = pool_n;
    for (int i = 0; i < pool_n; i++) {
      pool[i] = kPool[i];
    }
    int vstk[VLOOP_KERNEL_MAX_NODES];
    int nv = 0;
    for (int i = 0; i < n_nodes; i++) {
      int tag = (int)args[nodes_off + 3 * i].int_value;
      int op0 = (int)args[nodes_off + 3 * i + 1].int_value;
      int op1 = (int)args[nodes_off + 3 * i + 2].int_value;
      if (vloop_kernel_tag_is_leaf(tag)) {
        if (nfree <= 0) {
          code_generator_set_error(generator, "vloop reg budget");
          return 0;
        }
        int R = pool[--nfree];
        int ok = 0;
        if (tag == VLOOP_K_LOAD) {
          ok = elem8 ? (elem8_unsigned
                            ? wcs_avx_vpmovzxbd_ymm_mem(b, R, arr_reg[op0], 0)
                            : wcs_avx_vpmovsxbd_ymm_mem(b, R, arr_reg[op0], 0))
                     : wcs_avx_vmovups_ymm_mem(b, R, arr_reg[op0], 0);
        } else if (tag == VLOOP_K_CONST) {
          ok = wcs_avx_vmovups_ymm_mem(b, R, BINARY_GP_RSP, 32 * op0);
        } else if (tag == VLOOP_K_SCALAR) {
          ok = wcs_avx_vmovups_ymm_mem(b, R, BINARY_GP_RSP,
                                       32 * (n_consts + op0));
        } else { /* IOTA: R = [i, i+1, ...] over `lanes` lanes (cvt if float) */
          ok = wcs_broadcast_i32_to_ymm(b, R, BINARY_GP_R11) &&
               wcs_avx_vpaddd_ymm(b, R, R, IOTA_CONST) &&
               (i32 ? 1
                    : (f32 ? wcs_avx_vcvtdq2ps_ymm(b, R, R)
                           : wcs_avx_vcvtdq2pd_ymm_xmm(b, R, R)));
        }
        if (!ok) {
          return 0;
        }
        vstk[nv++] = R;
      } else if (tag == VLOOP_K_SHL || tag == VLOOP_K_SAR ||
                 tag == VLOOP_K_SHR) {
        if (nv < 1 || !i32) {
          code_generator_set_error(generator, "vloop shift");
          return 0;
        }
        int ra = vstk[nv - 1]; /* unary: shift the stack top in place */
        if (!(tag == VLOOP_K_SHL
                  ? wcs_avx_vpslld_ymm_imm(b, ra, ra, (unsigned char)op1)
                  : tag == VLOOP_K_SAR
                        ? wcs_avx_vpsrad_ymm_imm(b, ra, ra, (unsigned char)op1)
                        : wcs_avx_vpsrld_ymm_imm(b, ra, ra,
                                                 (unsigned char)op1))) {
          return 0;
        }
      } else if (tag == VLOOP_K_PAIR) {
        /* Both values are already on the stack; the node exists only to give
         * SELECT a third operand within a two-operand encoding. */
        if (nv < 2) {
          code_generator_set_error(generator, "vloop pair");
          return 0;
        }
      } else if (tag == VLOOP_K_SELECT) {
        /* mask ? then : else, as else ^ ((then ^ else) & mask). The mask is
         * all-ones or all-zeros per lane, so the arithmetic picks one side
         * whole, and it needs no register beyond the three already live. */
        if (nv < 3 || !i32) {
          code_generator_set_error(generator, "vloop select");
          return 0;
        }
        int relse = vstk[--nv];
        int rthen = vstk[--nv];
        int rmask = vstk[--nv];
        if (!wcs_avx_vpxor_ymm(b, rthen, rthen, relse) ||
            !wcs_avx_vpand_ymm(b, rthen, rthen, rmask) ||
            !wcs_avx_vpxor_ymm(b, rthen, rthen, relse)) {
          return 0;
        }
        pool[nfree++] = relse;
        pool[nfree++] = rmask;
        vstk[nv++] = rthen;
      } else {
        if (nv < 2) {
          code_generator_set_error(generator, "vloop stack");
          return 0;
        }
        int rb = vstk[--nv];
        int ra = vstk[--nv];
        int ok = 0;
        switch (tag) {
        case VLOOP_K_ADD:
          ok = i32 ? wcs_avx_vpaddd_ymm(b, ra, ra, rb)
                   : (f32 ? wcs_avx_vaddps_ymm(b, ra, ra, rb)
                          : wcs_avx_vaddpd_ymm(b, ra, ra, rb));
          break;
        case VLOOP_K_SUB:
          ok = i32 ? wcs_avx_vpsubd_ymm(b, ra, ra, rb)
                   : (f32 ? wcs_avx_vsubps_ymm(b, ra, ra, rb)
                          : wcs_avx_vsubpd_ymm(b, ra, ra, rb));
          break;
        case VLOOP_K_MUL:
          ok = i32 ? wcs_avx_vpmulld_ymm(b, ra, ra, rb)
                   : (f32 ? wcs_avx_vmulps_ymm(b, ra, ra, rb)
                          : wcs_avx_vmulpd_ymm(b, ra, ra, rb));
          break;
        case VLOOP_K_DIV:
          if (i32) {
            code_generator_set_error(generator, "vloop int div");
            return 0;
          }
          ok = f32 ? wcs_avx_vdivps_ymm(b, ra, ra, rb)
                   : wcs_avx_vdivpd_ymm(b, ra, ra, rb);
          break;
        case VLOOP_K_AND:
        case VLOOP_K_OR:
        case VLOOP_K_XOR:
          if (!i32) {
            code_generator_set_error(generator, "vloop float bitop");
            return 0;
          }
          ok = tag == VLOOP_K_AND
                   ? wcs_avx_vpand_ymm(b, ra, ra, rb)
                   : (tag == VLOOP_K_OR ? wcs_avx_vpor_ymm(b, ra, ra, rb)
                                        : wcs_avx_vpxor_ymm(b, ra, ra, rb));
          break;
        case VLOOP_K_MIN:
        case VLOOP_K_MAX:
        case VLOOP_K_CMPGT:
        case VLOOP_K_CMPEQ:
          if (!i32) {
            code_generator_set_error(generator, "vloop float compare");
            return 0;
          }
          ok = tag == VLOOP_K_MIN
                   ? wcs_avx_vpminsd_ymm(b, ra, ra, rb)
                   : tag == VLOOP_K_MAX
                         ? wcs_avx_vpmaxsd_ymm(b, ra, ra, rb)
                         : tag == VLOOP_K_CMPGT
                               ? wcs_avx_vpcmpgtd_ymm(b, ra, ra, rb)
                               : wcs_avx_vpcmpeqd_ymm(b, ra, ra, rb);
          break;
        default: code_generator_set_error(generator, "vloop op"); return 0;
        }
        if (!ok) {
          return 0;
        }
        pool[nfree++] = rb;
        vstk[nv++] = ra;
        (void)op0;
      }
    }
    if (nv != 1) {
      code_generator_set_error(generator, "vloop root");
      return 0;
    }
    if (is_minmax) {
      /* Data as src1, accumulator as src2: MAXPS/MINPS return src2 on an
       * unordered compare, so a NaN element leaves the accumulator alone --
       * exactly what `if (v > m) { m = v; }` does. */
      if (!(i32 ? (is_max ? wcs_avx_vpmaxsd_ymm(b, 2, vstk[0], 2)
                          : wcs_avx_vpminsd_ymm(b, 2, vstk[0], 2))
                : (f32 ? (is_max ? wcs_avx_vmaxps_ymm(b, 2, vstk[0], 2)
                                 : wcs_avx_vminps_ymm(b, 2, vstk[0], 2))
                       : (is_max ? wcs_avx_vmaxpd_ymm(b, 2, vstk[0], 2)
                                 : wcs_avx_vminpd_ymm(b, 2, vstk[0], 2))))) {
        return 0;
      }
    } else if (is_reduce) {
      if (!(i32 ? wcs_avx_vpaddd_ymm(b, 2, 2, vstk[0]) /* acc += lanes */
                : (f32 ? wcs_avx_vaddps_ymm(b, 2, 2, vstk[0])
                       : wcs_avx_vaddpd_ymm(b, 2, 2, vstk[0])))) {
        return 0;
      }
    } else if (elem8) {
      /* Eight int32 lanes back down to eight contiguous bytes. Masking to the
       * low byte first is what makes the two saturating packs exact: every
       * value is then in 0..255, so neither one clamps and the result is the
       * plain truncation the scalar store performs.
       *
       * The packs work within each 128-bit lane, which leaves the halves eight
       * lanes apart; vpermq brings them together BEFORE the second pack, so
       * the finished bytes land contiguously in the low quadword. */
      int R = vstk[0];
      if (!wcs_avx_vpand_ymm(b, R, R, BYTE_MASK) ||
          !wcs_avx_vpackusdw_ymm(b, R, R, R) ||
          !wcs_avx_vpermq_ymm(b, R, R, 0x08) ||
          !wcs_avx_vpackuswb_ymm(b, R, R, R) ||
          !wcs_movsd_mem_xmm(b, dst_reg, 0, R)) {
        return 0;
      }
    } else if (!wcs_avx_vmovups_mem_ymm(b, dst_reg, 0, vstk[0])) {
      return 0;
    }
  }
  for (int j = 0; j < n_dist; j++) {
    if (!wcs_addsub_reg_imm8(b, kGp[j], 0, vec_stride)) {
      return 0;
    }
  }
  if (has_iota && !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, lanes)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, lanes)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, vec_top)) {
      return 0;
    }
  }

  /* ---- scalar tail: while (count != 0) ---- */
  if (!wcs_patch_here(b, j_tail)) {
    return 0;
  }
  for (int j = 0; j < n_overlap; j++) {
    if (!wcs_patch_here(b, j_overlap[j])) {
      return 0;
    }
  }
  size_t tail_top = b->size;
  size_t j_done = 0;
  if (!wcs_cmp_reg_imm8(b, BINARY_GP_R10, 0) || !wcs_jcc(b, 0x84 /* je */, &j_done)) {
    return 0;
  }
  {
    int pool[VLOOP_KERNEL_POOL_MAX];
    int nfree = pool_n;
    for (int i = 0; i < pool_n; i++) {
      pool[i] = kPool[i];
    }
    int vstk[VLOOP_KERNEL_MAX_NODES];
    int nv = 0;
    for (int i = 0; i < n_nodes; i++) {
      int tag = (int)args[nodes_off + 3 * i].int_value;
      int op0 = (int)args[nodes_off + 3 * i + 1].int_value;
      int op1 = (int)args[nodes_off + 3 * i + 2].int_value;
      if (vloop_kernel_tag_is_leaf(tag)) {
        int R = pool[--nfree];
        int ok = 0;
        if (tag == VLOOP_K_LOAD && elem8) {
          /* Widen through a GP register: there is no scalar vpmovzxbd, and
           * movzx/movsx name the two widenings directly. */
          ok = (elem8_unsigned
                    ? binary_emit_movzx_reg_mem8(b, BINARY_GP_RAX,
                                                 (BinaryGpRegister)arr_reg[op0], 0)
                    : binary_emit_movsx_reg_mem8(b, BINARY_GP_RAX,
                                                 (BinaryGpRegister)arr_reg[op0], 0)) &&
               wcs_avx_vmovd_xmm_reg(b, R, BINARY_GP_RAX);
        } else if (tag == VLOOP_K_LOAD) {
          ok = i32 ? wcs_avx_vmovd_xmm_mem(b, R, arr_reg[op0], 0)
                   : (f32 ? wcs_movss_xmm_mem(b, R, arr_reg[op0], 0)
                          : wcs_movsd_xmm_mem(b, R, arr_reg[op0], 0));
        } else if (tag == VLOOP_K_CONST || tag == VLOOP_K_SCALAR) {
          int disp = 32 * (tag == VLOOP_K_CONST ? op0 : n_consts + op0);
          ok = i32 ? wcs_avx_vmovd_xmm_mem(b, R, BINARY_GP_RSP, disp)
                   : (f32 ? wcs_movss_xmm_mem(b, R, BINARY_GP_RSP, disp)
                          : wcs_movsd_xmm_mem(b, R, BINARY_GP_RSP, disp));
        } else { /* IOTA scalar: i (cvt to float for the float kinds) */
          ok = i32 ? wcs_avx_vmovd_xmm_reg(b, R, BINARY_GP_R11)
                   : (f32 ? binary_emit_cvtsi2ss_xmm_reg(b, (BinaryXmmRegister)R,
                                                         BINARY_GP_R11)
                          : binary_emit_cvtsi2sd_xmm_reg(b, (BinaryXmmRegister)R,
                                                         BINARY_GP_R11));
        }
        if (!ok) {
          return 0;
        }
        vstk[nv++] = R;
      } else if (tag == VLOOP_K_SHL || tag == VLOOP_K_SAR ||
                 tag == VLOOP_K_SHR) {
        /* Unary, in place. Every right shift maps a zero lane to zero, so the
         * tail's zero-upper invariant survives them as it does the left one. */
        int ra = vstk[nv - 1];
        if (!i32 ||
            !(tag == VLOOP_K_SHL
                  ? wcs_avx_vpslld_ymm_imm(b, ra, ra, (unsigned char)op1)
                  : tag == VLOOP_K_SAR
                        ? wcs_avx_vpsrad_ymm_imm(b, ra, ra, (unsigned char)op1)
                        : wcs_avx_vpsrld_ymm_imm(b, ra, ra,
                                                 (unsigned char)op1))) {
          return 0;
        }
      } else if (tag == VLOOP_K_PAIR) {
        if (nv < 2) {
          return 0;
        }
      } else if (tag == VLOOP_K_SELECT) {
        /* Same bitwise select as the vector body. A zero lane compares equal,
         * not greater, so the mask's upper lanes are zero and the blend keeps
         * the else side there: zero stays zero and lane 0 is exact. */
        int relse, rthen, rmask;
        if (nv < 3 || !i32) {
          return 0;
        }
        relse = vstk[--nv];
        rthen = vstk[--nv];
        rmask = vstk[--nv];
        if (!wcs_avx_vpxor_ymm(b, rthen, rthen, relse) ||
            !wcs_avx_vpand_ymm(b, rthen, rthen, rmask) ||
            !wcs_avx_vpxor_ymm(b, rthen, rthen, relse)) {
          return 0;
        }
        pool[nfree++] = relse;
        pool[nfree++] = rmask;
        vstk[nv++] = rthen;
      } else if (i32) {
        /* Integer tail ALU: full-width VEX ops on zero-upper values (every
         * leaf above loaded via VEX vmovd, and + - * & | ^ << all map zero
         * lanes to zero), so lane 0 is the exact scalar result. */
        int rb = vstk[--nv];
        int ra = vstk[--nv];
        int ok = 0;
        switch (tag) {
        case VLOOP_K_ADD: ok = wcs_avx_vpaddd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_SUB: ok = wcs_avx_vpsubd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_MUL: ok = wcs_avx_vpmulld_ymm(b, ra, ra, rb); break;
        case VLOOP_K_AND: ok = wcs_avx_vpand_ymm(b, ra, ra, rb); break;
        case VLOOP_K_OR: ok = wcs_avx_vpor_ymm(b, ra, ra, rb); break;
        case VLOOP_K_XOR: ok = wcs_avx_vpxor_ymm(b, ra, ra, rb); break;
        case VLOOP_K_MIN: ok = wcs_avx_vpminsd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_MAX: ok = wcs_avx_vpmaxsd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_CMPGT: ok = wcs_avx_vpcmpgtd_ymm(b, ra, ra, rb); break;
        case VLOOP_K_CMPEQ: ok = wcs_avx_vpcmpeqd_ymm(b, ra, ra, rb); break;
        default: return 0;
        }
        if (!ok) {
          return 0;
        }
        pool[nfree++] = rb;
        vstk[nv++] = ra;
      } else {
        int rb = vstk[--nv];
        int ra = vstk[--nv];
        int ok = 0;
        BinaryXmmRegister A = (BinaryXmmRegister)ra;
        BinaryXmmRegister B = (BinaryXmmRegister)rb;
        switch (tag) {
        case VLOOP_K_ADD:
          ok = f32 ? binary_emit_addss_xmm_xmm(b, A, B)
                   : binary_emit_addsd_xmm_xmm(b, A, B);
          break;
        case VLOOP_K_SUB:
          ok = f32 ? binary_emit_subss_xmm_xmm(b, A, B)
                   : binary_emit_subsd_xmm_xmm(b, A, B);
          break;
        case VLOOP_K_MUL:
          ok = f32 ? binary_emit_mulss_xmm_xmm(b, A, B)
                   : binary_emit_mulsd_xmm_xmm(b, A, B);
          break;
        case VLOOP_K_DIV:
          ok = f32 ? binary_emit_divss_xmm_xmm(b, A, B)
                   : binary_emit_divsd_xmm_xmm(b, A, B);
          break;
        default: return 0;
        }
        if (!ok) {
          return 0;
        }
        pool[nfree++] = rb;
        vstk[nv++] = ra;
      }
    }
    if (is_minmax) {
      /* Only lane 0 of the tail value is meaningful, and min/max has no
       * identity to pad the rest with -- so splat lane 0 across the register
       * and fold it in whole. Every lane then sees the same element, which
       * leaves the packed accumulator exactly where one more scalar step
       * would have put it. */
      int R = vstk[0];
      if (!(f32 ? wcs_avx_vpbroadcastd_ymm(b, R, R)
                : wcs_avx_vbroadcastsd_ymm_xmm(b, R, R))) {
        return 0;
      }
      if (!(i32 ? (is_max ? wcs_avx_vpmaxsd_ymm(b, 2, R, 2)
                          : wcs_avx_vpminsd_ymm(b, 2, R, 2))
                : (f32 ? (is_max ? wcs_avx_vmaxps_ymm(b, 2, R, 2)
                                 : wcs_avx_vminps_ymm(b, 2, R, 2))
                       : (is_max ? wcs_avx_vmaxpd_ymm(b, 2, R, 2)
                                 : wcs_avx_vminpd_ymm(b, 2, R, 2))))) {
        return 0;
      }
    } else if (is_reduce) {
      if (i32) {
        /* lane 0 carries the addend, lanes 1..7 zeros: fold into ymm2. */
        if (!wcs_avx_vpaddd_ymm(b, 2, 2, vstk[0])) {
          return 0;
        }
      } else if (!(f32 ? binary_emit_addss_xmm_xmm(b, BINARY_XMM3,
                                                   (BinaryXmmRegister)vstk[0])
                       : binary_emit_addsd_xmm_xmm(b, BINARY_XMM3,
                                                   (BinaryXmmRegister)vstk[0]))) {
        return 0;
      }
    } else if (elem8) {
      /* The scalar store truncates to the low byte, which is exactly what the
       * masked packs do eight at a time above. */
      if (!wcs_avx_vpextrb_mem_xmm(b, dst_reg, 0, vstk[0])) {
        return 0;
      }
    } else if (!(i32 ? wcs_avx_vmovd_mem_xmm(b, dst_reg, 0, vstk[0])
                     : (f32 ? wcs_movss_mem_xmm(b, dst_reg, 0, vstk[0])
                            : wcs_movsd_mem_xmm(b, dst_reg, 0, vstk[0])))) {
      return 0;
    }
  }
  for (int j = 0; j < n_dist; j++) {
    if (!wcs_addsub_reg_imm8(b, kGp[j], 0, elem_bytes)) {
      return 0;
    }
  }
  if (has_iota && !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 1)) {
    return 0;
  }
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R10, 1, 1)) {
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
  if (is_reduce) {
    /* Fold the packed accumulator -> RAX and store to the accumulator symbol.
     * The reduce helpers do their own vextract + vzeroupper, so they MUST run
     * before any standalone vzeroupper (which would zero ymm2's upper lanes
     * and drop accumulator lanes). i32 loads the prior accumulator value into
     * RAX first: wcs_reduce_ymm_i32_sum_to_rax accumulates the lane sum INTO
     * RAX (and clobbers R10, which the count loop is done with). */
    if (!is_minmax && i32 &&
        !code_generator_binary_emit_operand_load(generator, context,
                                                 &instruction->dest,
                                                 BINARY_GP_RAX)) {
      return 0;
    }
    if (is_minmax) {
      if (!(i32 ? wcs_reduce_ymm_i32_minmax_to_rax(b, is_max)
                : (f32 ? wcs_reduce_ps_minmax_to_rax(b, is_max)
                       : wcs_reduce_pd_minmax_to_rax(b, is_max)))) {
        return 0;
      }
    } else if (!(i32 ? wcs_reduce_ymm_i32_sum_to_rax(b, 2)
                     : (f32 ? wcs_reduce_ps_acc_to_rax(b)
                            : wcs_reduce_pd_acc_to_rax(b)))) {
      return 0;
    }
    /* The 64-bit fold can carry bits past 32 (the helper sign-extends and
     * adds); the accumulator's 8-byte home must hold a canonically-extended
     * 32-bit value, matching its declared signedness. */
    if (i32 &&
        !(instruction->is_unsigned
              ? wcs_mov_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX)
              : binary_emit_movsxd_reg_reg32(b, BINARY_GP_RAX, BINARY_GP_RAX))) {
      return 0;
    }
    if (cbytes && !binary_emit_add_rsp_imm32(b, cbytes)) {
      return 0;
    }
    return code_generator_binary_emit_destination_store(generator, context,
                                                        &instruction->dest,
                                                        BINARY_GP_RAX);
  }
  if (!wcs_avx_vzeroupper(b)) {
    return 0;
  }
  if (cbytes && !binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return 1;
}

/* Outer-loop lane vectorizer (IR_OP_SIMD_OUTER_LANE_F64). Runs 4 outer-loop
 * iterations of an outer-invariant inner serial recurrence in lockstep f64x4
 * lanes (genuinely running all the inner work 4-wide to hide the recurrence
 * latency), then accumulates the lane-identical result into the total with
 * exact scalar adds (bit-identical to the scalar loop). See the recognizer
 * ir_outer_vectorize_pass for the arguments[] layout. */
/* uniform micro-ops (mirror of the recognizer's OL_U_*). */
#define OLK_U_AND 1
#define OLK_U_OR 2
#define OLK_U_XOR 3
#define OLK_U_ADD 4
#define OLK_U_SUB 5
#define OLK_U_MUL 6
#define OLK_U_SHL 7
#define OLK_U_SHR 8
#define OLK_U_CVT 9
#define OLK_U_FADD 10
#define OLK_U_FSUB 11
#define OLK_U_FMUL 12
#define OLK_U_FDIV 13
#define OLK_C_ADD 0
#define OLK_C_SUB 1
#define OLK_C_MUL 2
#define OLK_C_DIV 3

/* Emit a uniform-of-base program (micro-ops at args[prog_off..]) over the integer
 * value in val_gpr, leaving the float result in res_xmm. All ops VEX-encoded to
 * avoid AVX<->SSE transitions. Uses work_gpr + ctmp_xmm + RAX as scratch; float
 * consts are read from the stack array at [rsp + 32*idx]. */
static int ol_emit_uniform_prog(CodeGenerator *gen, BinaryCodeBuffer *b,
                                const IROperand *args, size_t prog_off,
                                int n_micro, int n_fconst,
                                BinaryGpRegister val_gpr, BinaryGpRegister work_gpr,
                                int res_xmm, int ctmp_xmm) {
  if (!binary_emit_mov_reg_reg(b, work_gpr, val_gpr)) {
    return 0;
  }
  int in_float = 0;
  for (int m = 0; m < n_micro; m++) {
    int mop = (int)args[prog_off + 2 * m].int_value;
    long long mimm = args[prog_off + 2 * m + 1].int_value;
    int ok = 1;
    switch (mop) {
    case OLK_U_AND:
    case OLK_U_OR:
    case OLK_U_XOR:
    case OLK_U_ADD:
    case OLK_U_SUB:
    case OLK_U_MUL: {
      unsigned char alu = 0;
      if (mop == OLK_U_ADD) alu = 0x01;
      else if (mop == OLK_U_SUB) alu = 0x29;
      else if (mop == OLK_U_AND) alu = 0x21;
      else if (mop == OLK_U_OR) alu = 0x09;
      else if (mop == OLK_U_XOR) alu = 0x31;
      ok = binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (uint64_t)mimm);
      if (ok && mop == OLK_U_MUL) {
        ok = binary_emit_imul_reg_reg(b, work_gpr, BINARY_GP_RAX);
      } else if (ok) {
        ok = binary_emit_alu_reg_reg(b, alu, work_gpr, BINARY_GP_RAX);
      }
      break;
    }
    case OLK_U_SHL: ok = wcs_shift_reg_imm(b, work_gpr, 0, (unsigned char)mimm); break;
    case OLK_U_SHR: ok = wcs_shift_reg_imm(b, work_gpr, 1, (unsigned char)mimm); break;
    case OLK_U_CVT:
      ok = wcs_avx_vcvtsi2sd(b, res_xmm, res_xmm, work_gpr);
      in_float = 1;
      break;
    case OLK_U_FADD:
    case OLK_U_FSUB:
    case OLK_U_FMUL:
    case OLK_U_FDIV: {
      if (mimm < 0 || mimm >= n_fconst) {
        code_generator_set_error(gen, "vloop outer: unif fconst idx");
        return 0;
      }
      ok = wcs_avx_vmovsd_xmm_mem(b, ctmp_xmm, BINARY_GP_RSP, (int)(32 * mimm));
      if (ok) {
        if (mop == OLK_U_FADD) ok = wcs_avx_vaddsd(b, res_xmm, res_xmm, ctmp_xmm);
        else if (mop == OLK_U_FSUB) ok = wcs_avx_vsubsd(b, res_xmm, res_xmm, ctmp_xmm);
        else if (mop == OLK_U_FMUL) ok = wcs_avx_vmulsd(b, res_xmm, res_xmm, ctmp_xmm);
        else ok = wcs_avx_vdivsd(b, res_xmm, res_xmm, ctmp_xmm);
      }
      break;
    }
    default:
      code_generator_set_error(gen, "vloop outer: bad micro op");
      return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!in_float) {
    code_generator_set_error(gen, "vloop outer: uniform never cast");
    return 0;
  }
  return 1;
}

int code_generator_binary_emit_simd_outer_lane_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  if (!generator || !context || !instruction || instruction->argument_count < 8 ||
      !instruction->arguments || instruction->dest.kind != IR_OPERAND_SYMBOL) {
    code_generator_set_error(generator, "Malformed simd_outer_lane_f64");
    return 0;
  }
  b = &context->code;
  const IROperand *args = instruction->arguments;
  int inner_cmp = (int)args[0].int_value;  /* 0:< 1:<= */
  long long istep = args[1].int_value;
  int n_chain = (int)args[2].int_value;
  int n_unif = (int)args[3].int_value;
  int n_fconst = (int)args[4].int_value;
  long long i0 = args[5].int_value;
  int init_mode = (int)args[6].int_value;     /* 0: const seed; 1: seed(p) */
  double iacc_init = args[7].float_value;
  if (n_chain <= 0 || n_chain > 8 || n_unif < 0 || n_unif > 8 || n_fconst < 0 ||
      n_fconst > 16 || istep <= 0 || (init_mode != 0 && init_mode != 1)) {
    code_generator_set_error(generator, "Bad simd_outer_lane_f64 encoding");
    return 0;
  }

  /* K independent lane-accumulator chains run in lockstep so the divide unit
   * stays saturated (a single serial divpd chain is latency-bound; K>=2 makes it
   * throughput-bound). Each chain covers 4 outer iterations, so a super-group
   * covers 4*K. init_mode 0 -> all lanes identical (lane0 fast path). init_mode 1
   * -> each lane seeded from its own outer index p (lanes diverge), summed by
   * per-lane extraction in p order (still bit-exact). K=3 fits volatile xmm0-5:
   * acc=ymm0/1/2, term=ymm3, uniform-scalar=xmm4, uniform-const=xmm5; total and
   * the per-lane seed array live in stack slots. */
  const int K = 3;
  const int kAcc[3] = {0, 1, 2};
  const int GROUP = 4 * K;

  /* Locate the sections of arguments[]. */
  size_t chain_off = 8;
  size_t off = chain_off + (size_t)(4 * n_chain);
  size_t unif_start[8];
  int unif_nmicro[8];
  for (int u = 0; u < n_unif; u++) {
    if (off >= instruction->argument_count) {
      code_generator_set_error(generator, "vloop outer: uniform overrun");
      return 0;
    }
    int nm = (int)args[off].int_value;
    if (nm < 0 || nm > 16) {
      code_generator_set_error(generator, "vloop outer: bad micro count");
      return 0;
    }
    unif_nmicro[u] = nm;
    unif_start[u] = off + 1;
    off += 1 + (size_t)(2 * nm);
  }
  size_t init_start = 0;
  int init_nmicro = 0;
  if (init_mode == 1) {
    if (off >= instruction->argument_count) {
      code_generator_set_error(generator, "vloop outer: seed overrun");
      return 0;
    }
    init_nmicro = (int)args[off].int_value;
    if (init_nmicro < 0 || init_nmicro > 16) {
      code_generator_set_error(generator, "vloop outer: bad seed micro count");
      return 0;
    }
    init_start = off + 1;
    off += 1 + (size_t)(2 * init_nmicro);
  }
  size_t fconst_off = off;
  if (fconst_off + (size_t)n_fconst != instruction->argument_count) {
    code_generator_set_error(generator, "vloop outer: arg length mismatch");
    return 0;
  }

  /* Stack frame: [0 .. 32*n_fconst) broadcast const array; then an 8-byte running
   * total slot; then (init_mode 1 only) a 32*K seed-vector scratch array. */
  uint64_t init_bits = 0;
  memcpy(&init_bits, &iacc_init, sizeof(init_bits));
  int total_off = 32 * n_fconst;
  int seedarr_off = total_off + 16;
  uint32_t cbytes =
      (uint32_t)(seedarr_off + (init_mode == 1 ? 32 * K : 0)); /* 16-aligned */
  if (!binary_emit_sub_rsp_imm32(b, cbytes)) {
    return 0;
  }
  /* total = prior accumulator value (dest) -> stack slot. */
  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX) ||
      !wcs_movsd_mem_xmm(b, BINARY_GP_RSP, total_off, 4) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_R9) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_R8)) {
    return 0;
  }
  /* Broadcast each fconst to its stack slot. */
  for (int c = 0; c < n_fconst; c++) {
    double dv = args[fconst_off + c].float_value;
    uint64_t bits = 0;
    memcpy(&bits, &dv, sizeof(bits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, bits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 0, 0) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 32 * c, 0)) {
      return 0;
    }
  }
  /* rdx = 0 (outer counter). */
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RDX, 0)) {
    return 0;
  }

  size_t outer_top = b->size;
  size_t j_outer_end = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RDX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_outer_end)) {
    return 0;
  }
  /* Seed the K lane accumulators. */
  if (init_mode == 0) {
    /* identical: every lane = broadcast(const init). */
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, init_bits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM4, BINARY_GP_RAX)) {
      return 0;
    }
    for (int k = 0; k < K; k++) {
      if (!wcs_avx_vbroadcastsd_ymm_xmm(b, kAcc[k], 4)) {
        return 0;
      }
    }
  } else {
    /* divergent: lane g (global) seeds from seed(p+g). Evaluate scalar per lane
     * into the stack seed array, then load each accumulator's 4 lanes. */
    for (int g = 0; g < GROUP; g++) {
      if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
          !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)g) ||
          !ol_emit_uniform_prog(generator, b, args, init_start, init_nmicro,
                                n_fconst, BINARY_GP_R11, BINARY_GP_R10, 4, 5) ||
          !wcs_avx_vmovsd_mem_xmm(b, BINARY_GP_RSP, seedarr_off + 8 * g, 4)) {
        return 0;
      }
    }
    for (int k = 0; k < K; k++) {
      if (!wcs_avx_vmovups_ymm_mem(b, kAcc[k], BINARY_GP_RSP,
                                   seedarr_off + 32 * k)) {
        return 0;
      }
    }
  }
  if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RCX, (uint64_t)i0)) {
    return 0;
  }

  size_t inner_top = b->size;
  size_t j_inner_end = 0;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, inner_cmp ? 0x8F /* jg */ : 0x8D /* jge */, &j_inner_end)) {
    return 0;
  }
  /* Replay the recurrence chain on the lane vector. */
  for (int s = 0; s < n_chain; s++) {
    int c_op = (int)args[chain_off + 4 * s].int_value;
    int side = (int)args[chain_off + 4 * s + 1].int_value;
    int kind = (int)args[chain_off + 4 * s + 2].int_value;
    int idx = (int)args[chain_off + 4 * s + 3].int_value;
    if (kind == 0) { /* const term: broadcast already on the stack -> ymm3 */
      if (idx < 0 || idx >= n_fconst ||
          !wcs_avx_vmovups_ymm_mem(b, 3, BINARY_GP_RSP, 32 * idx)) {
        code_generator_set_error(generator, "vloop outer: const idx");
        return 0;
      }
    } else { /* uniform-of-i term: evaluate scalar (xmm4) then broadcast -> ymm3 */
      if (idx < 0 || idx >= n_unif ||
          !ol_emit_uniform_prog(generator, b, args, unif_start[idx],
                                unif_nmicro[idx], n_fconst, BINARY_GP_RCX,
                                BINARY_GP_R10, 4, 5) ||
          !wcs_avx_vbroadcastsd_ymm_xmm(b, 3, 4)) {
        if (idx < 0 || idx >= n_unif) {
          code_generator_set_error(generator, "vloop outer: unif idx");
        }
        return 0;
      }
    }
    /* Apply term (ymm3) to every lane accumulator: acc = acc OP term (side 0)
     * or acc = term OP acc (side 1). The K vops are independent -> they fill the
     * divide/FP pipeline. */
    for (int k = 0; k < K; k++) {
      int a = kAcc[k];
      int ok = 0;
      if (c_op == OLK_C_ADD) ok = wcs_avx_vaddpd_ymm(b, a, a, 3);
      else if (c_op == OLK_C_MUL) ok = wcs_avx_vmulpd_ymm(b, a, a, 3);
      else if (c_op == OLK_C_SUB)
        ok = side ? wcs_avx_vsubpd_ymm(b, a, 3, a) : wcs_avx_vsubpd_ymm(b, a, a, 3);
      else if (c_op == OLK_C_DIV)
        ok = side ? wcs_avx_vdivpd_ymm(b, a, 3, a) : wcs_avx_vdivpd_ymm(b, a, a, 3);
      else { code_generator_set_error(generator, "vloop outer: chain op"); return 0; }
      if (!ok) {
        return 0;
      }
    }
  }
  /* i += istep; loop. */
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, (unsigned char)istep)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, inner_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_inner_end)) {
    return 0;
  }
  /* xmm5 = running total (reloaded from the stack slot). */
  if (!wcs_avx_vmovsd_xmm_mem(b, 5, BINARY_GP_RSP, total_off)) {
    return 0;
  }
  if (init_mode == 0) {
    /* identical lanes: lane0 of ymm0 == S. Add S to total for each of the
     * min(GROUP, P-p) outer iterations this super-group covers (exact). */
    if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
        !binary_emit_alu_reg_reg(b, 0x29 /* sub */, BINARY_GP_R11, BINARY_GP_RDX) ||
        !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, (uint64_t)GROUP) ||
        !wcs_cmp_reg_imm32(b, BINARY_GP_R11, (uint32_t)GROUP) ||
        !binary_emit_cmovcc_reg_reg(b, 0x4F /* cmovg */, BINARY_GP_R11,
                                    BINARY_GP_RAX)) {
      return 0;
    }
    size_t add_top = b->size;
    if (!wcs_avx_vaddsd(b, 5, 5, 0) || /* total += S (lane0) */
        !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 1) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 1, 1)) {
      return 0;
    }
    size_t j_add_back = 0;
    if (!wcs_jcc(b, 0x85 /* jnz */, &j_add_back) ||
        !wcs_patch_to(b, j_add_back, add_top)) {
      return 0;
    }
    /* rdx was advanced by the group size inside the add-loop. */
  } else {
    /* divergent lanes: lane g (global) holds S for outer iteration p+g. Add each
     * valid lane (p+g < P) to total in p order -> bit-exact. */
    for (int g = 0; g < GROUP; g++) {
      int k = g / 4, j = g % 4;
      int acc = kAcc[k];
      size_t j_skip = 0;
      /* if (p + g >= P) skip this lane. */
      if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RDX) ||
          !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, (unsigned char)g) ||
          !binary_emit_cmp_reg_reg(b, BINARY_GP_R11, BINARY_GP_R9) ||
          !wcs_jcc(b, 0x8D /* jge */, &j_skip)) {
        return 0;
      }
      /* bring lane j of `acc` into xmm4 low, then total += it. */
      int ok = 1;
      if (j == 0) {
        ok = wcs_avx_vaddsd(b, 5, 5, acc); /* lane0 = acc low */
      } else if (j == 1) {
        ok = wcs_avx_vunpckhpd_xmm(b, 4, acc, acc) && wcs_avx_vaddsd(b, 5, 5, 4);
      } else if (j == 2) {
        ok = wcs_avx_vextractf128(b, 4, acc, 1) && wcs_avx_vaddsd(b, 5, 5, 4);
      } else { /* j == 3 */
        ok = wcs_avx_vextractf128(b, 4, acc, 1) &&
             wcs_avx_vunpckhpd_xmm(b, 4, 4, 4) && wcs_avx_vaddsd(b, 5, 5, 4);
      }
      if (!ok || !wcs_patch_here(b, j_skip)) {
        return 0;
      }
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, (unsigned char)GROUP)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovsd_mem_xmm(b, BINARY_GP_RSP, total_off, 5)) {
    return 0;
  }
  {
    size_t j_back = 0;
    if (!wcs_jcc(b, 0, &j_back) || !wcs_patch_to(b, j_back, outer_top)) {
      return 0;
    }
  }

  if (!wcs_patch_here(b, j_outer_end) || !wcs_avx_vzeroupper(b)) {
    return 0;
  }
  /* Load the final total from the stack, free the frame, store to dest. */
  if (!wcs_movsd_xmm_mem(b, 0, BINARY_GP_RSP, total_off) ||
      !binary_emit_movq_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM0)) {
    return 0;
  }
  if (!binary_emit_add_rsp_imm32(b, cbytes)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Counted-loop counter reduction:  dest += sum_{i=0}^{n-1} (int64)trunc(CHAIN(i))
 * where CHAIN is the float64 expression applied to (float64)i described by the
 * (op,const) pairs in arguments[1..]. arguments[0] = trip count n (constant).
 *
 * The recognizer (ir_simd_i2f_reduce_pass) has already proven every per-element
 * value fits int32 and the integer sum stays < 2^52, so this kernel:
 *   - processes 4 lanes/iter: i_vec=[k..k+3] -> vcvtdq2pd -> replay CHAIN with
 *     packed ops (bit-identical to scalar) -> vcvttpd2dq (exact truncate) ->
 *     vcvtdq2pd back -> accumulate in a packed-f64 accumulator (ymm2);
 *   - mops up the < 4 tail with the identical scalar chain into xmm3;
 *   - folds ymm2+xmm3 to one double (an exact integer), truncates once to int64,
 *     and adds it to dest's prior value.
 * Registers: rcx=i, r8=vec_end, r9=n, rax/r10 scratch; ymm5=iota[0,1,2,3],
 * ymm0=chain value, ymm1=scratch/broadcast, ymm2=packed acc, xmm3=scalar tail. */
int code_generator_binary_emit_simd_i2f_reduce_f64(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  long long bound = 0;
  long long vec_end = 0;
  size_t nsteps = 0;
  size_t vec_top = 0;
  size_t rem_top = 0;
  size_t j_after_vec = 0;
  size_t j_done = 0;
  size_t back = 0;

  if (!generator || !context || !instruction || instruction->argument_count < 3 ||
      (instruction->argument_count % 2) == 0 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_i2f_reduce_f64");
    return 0;
  }
  b = &context->code;
  bound = instruction->arguments[0].int_value;
  vec_end = bound & ~3LL;
  nsteps = (instruction->argument_count - 1) / 2;
  uint32_t const_bytes = (uint32_t)(32 * nsteps);

  /* acc(ymm2)=0, tail(xmm3)=0, iota(ymm5)=[0,1,2,3], i(rcx)=0,
   * vec_end(r8), n(r9). */
  if (!wcs_avx_vpxor_ymm(b, 2, 2, 2) || !wcs_avx_vpxor_ymm(b, 3, 3, 3) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000100000000ULL) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, 0x0000000300000002ULL) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
      !wcs_avx_vpunpcklqdq_xmm(b, 5, 0, 1) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_RCX, 0) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R8, (uint64_t)vec_end) ||
      !binary_emit_mov_reg_imm64(b, BINARY_GP_R9, (uint64_t)bound)) {
    return 0;
  }

  /* Hoist the chain constants: broadcast each once into a 32-byte stack slot so
   * the loop body re-reads them from L1 instead of rematerializing a movabs +
   * vbroadcastsd every iteration. r11 -> the constant array base. */
  if (!binary_emit_sub_rsp_imm32(b, const_bytes) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    double kd = instruction->arguments[2 + 2 * s].float_value;
    uint64_t kbits = 0;
    memcpy(&kbits, &kd, sizeof(kbits));
    if (!binary_emit_mov_reg_imm64(b, BINARY_GP_RAX, kbits) ||
        !binary_emit_movq_xmm_reg(b, BINARY_XMM1, BINARY_GP_RAX) ||
        !wcs_avx_vbroadcastsd_ymm_xmm(b, 1, 1) ||
        !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_R11, (int)(32 * s), 1)) {
      return 0;
    }
  }

  /* ---- vector loop: while (i < vec_end) ---- */
  vec_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R8) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_after_vec) ||
      !wcs_broadcast_i32_to_ymm(b, 1, BINARY_GP_RCX) ||
      !wcs_avx_vpaddd_ymm(b, 1, 1, 5) ||
      !wcs_avx_vcvtdq2pd_ymm_xmm(b, 0, 1)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[1 + 2 * s].int_value;
    if (!wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_R11, (int)(32 * s))) {
      return 0;
    }
    int ok = 0;
    switch (op) {
    case 0: ok = wcs_avx_vmulpd_ymm(b, 0, 0, 1); break; /* x*k */
    case 1: ok = wcs_avx_vaddpd_ymm(b, 0, 0, 1); break; /* x+k */
    case 2: ok = wcs_avx_vsubpd_ymm(b, 0, 0, 1); break; /* x-k */
    case 3: ok = wcs_avx_vsubpd_ymm(b, 0, 1, 0); break; /* k-x */
    case 4: ok = wcs_avx_vdivpd_ymm(b, 0, 0, 1); break; /* x/k */
    default: code_generator_set_error(generator, "bad i2f step op"); return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  if (!wcs_avx_vcvttpd2dq_xmm_ymm(b, 1, 0) ||
      !wcs_avx_vcvtdq2pd_ymm_xmm(b, 0, 1) || !wcs_avx_vaddpd_ymm(b, 2, 2, 0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
      !wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, vec_top)) {
    return 0;
  }

  /* ---- scalar tail: while (i < n) ---- */
  if (!wcs_patch_here(b, j_after_vec)) {
    return 0;
  }
  rem_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x8D /* jge */, &j_done) ||
      !binary_emit_cvtsi2sd_xmm_reg(b, BINARY_XMM0, BINARY_GP_RCX)) {
    return 0;
  }
  for (size_t s = 0; s < nsteps; s++) {
    int op = (int)instruction->arguments[1 + 2 * s].int_value;
    if (!wcs_movsd_xmm_mem(b, 1, BINARY_GP_R11, (int)(32 * s))) {
      return 0;
    }
    int ok = 0;
    switch (op) {
    case 0: ok = binary_emit_mulsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 1: ok = binary_emit_addsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 2: ok = binary_emit_subsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    case 3: /* k - x: compute in xmm1, move back to xmm0 */
      ok = binary_emit_subsd_xmm_xmm(b, BINARY_XMM1, BINARY_XMM0) &&
           wcs_sse_f2(b, 0x10, 0, 1) /* movsd xmm0, xmm1 */;
      break;
    case 4: ok = binary_emit_divsd_xmm_xmm(b, BINARY_XMM0, BINARY_XMM1); break;
    default: code_generator_set_error(generator, "bad i2f step op"); return 0;
    }
    if (!ok) {
      return 0;
    }
  }
  /* trunc to int64 then back to an integer-valued double, add into the tail. */
  if (!binary_emit_cvttsd2si_reg_xmm(b, BINARY_GP_RAX, BINARY_XMM0) ||
      !binary_emit_cvtsi2sd_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_addsd_xmm_xmm(b, BINARY_XMM3, BINARY_XMM0) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 1) ||
      !wcs_jcc(b, 0, &back) || !wcs_patch_to(b, back, rem_top)) {
    return 0;
  }

  /* ---- fold + finalize: total = (int64)(reduce(ymm2)+xmm3); dest += total ---- */
  if (!wcs_patch_here(b, j_done) ||
      !binary_emit_add_rsp_imm32(b, const_bytes) /* free the constant array */ ||
      !wcs_reduce_pd_acc_to_rax(b) ||
      !binary_emit_movq_xmm_reg(b, BINARY_XMM0, BINARY_GP_RAX) ||
      !binary_emit_cvttsd2si_reg_xmm(b, BINARY_GP_R10, BINARY_XMM0) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !wcs_add_reg_reg64(b, BINARY_GP_RAX, BINARY_GP_R10)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* Float32 dot product of a[0..n-1]*b[0..n-1], ADDED to dest's prior value.
 * dest = float32 sum, lhs = a, rhs = b, arguments[0] = element count. */
int code_generator_binary_emit_simd_dot_f32(
    CodeGenerator *generator, BinaryFunctionContext *context,
    const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0;
  size_t j_done = 0;
  size_t j_vec2 = 0;
  size_t j_vec = 0;
  size_t j_scalar = 0;

  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_dot_f32");
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RAX) ||
      !binary_emit_movd_xmm_reg(b, BINARY_XMM3, BINARY_GP_RAX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->lhs,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->rhs,
                                               BINARY_GP_RDX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !wcs_mov_reg_reg32(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !wcs_shift_reg_imm(b, BINARY_GP_R9, 0, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !wcs_avx_vpxor_ymm(b, 2, 2, 2) ||
      !wcs_avx_vpxor_ymm(b, 4, 4, 4)) {
    return 0;
  }

  loop_top = b->size;
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_done) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) ||
      !binary_emit_alu_reg_reg(b, 0x29, BINARY_GP_R10, BINARY_GP_RCX) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 64) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec2) ||
      !wcs_cmp_reg_imm32(b, BINARY_GP_R10, 32) ||
      !wcs_jcc(b, 0x83 /* jae */, &j_vec) ||
      !wcs_jcc(b, 0, &j_scalar)) {
    return 0;
  }

  /* Two-accumulator FMA unroll: 16 floats/iter into the independent ymm2/ymm4
   * chains via vfmadd231ps. */
  if (!wcs_patch_here(b, j_vec2) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231ps_ymm(b, 2, 0, 1) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 32) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 32) ||
      !wcs_avx_vfmadd231ps_ymm(b, 4, 0, 1) ||
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

  if (!wcs_patch_here(b, j_vec) ||
      !wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_avx_vmovups_ymm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_avx_vfmadd231ps_ymm(b, 2, 0, 1) ||
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
      !wcs_movss_xmm_mem(b, 0, BINARY_GP_RCX, 0) ||
      !wcs_movss_xmm_mem(b, 1, BINARY_GP_RDX, 0) ||
      !wcs_fmadd231ss(b, 3, 0, 1) ||
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
      !wcs_avx_vaddps_ymm(b, 2, 2, 4) ||
      !wcs_reduce_ps_acc_to_rax(b)) {
    return 0;
  }
  return code_generator_binary_emit_destination_store(generator, context,
                                                      &instruction->dest,
                                                      BINARY_GP_RAX);
}

/* ===================== vectorized exp() (float32) ========================= */
/* AVX2 Cephes single-precision exp, 8 lanes at a time. exp(x) = 2^k * exp(r)
 * with k = floor(x/ln2 + 0.5) and r = x - k*ln2 reduced into a small interval,
 * exp(r) a degree-6 polynomial. Constants are broadcast on demand from a GP
 * register (VEX vmovd + vpbroadcastd) so the kernel touches only the Win64
 * volatile xmm0..5, with no callee-saved vector registers to preserve. The
 * scalar exp_f32 the recognizer matches uses the identical polynomial, so the
 * vectorized result tracks it (validated within tolerance and against libm). */

static uint32_t exp_f32_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

/* Constant pool laid out at [rsp + EXP_POOL_OFF]; 14 float32 slots. The kernel
 * fills it once, and exp_compute broadcasts each constant from there per use
 * (vbroadcastss, a single L1 op) -- far cheaper than re-materializing constants
 * through a GP register every iteration, and it leaves the callee-saved vector
 * registers untouched. */
#define EXP_POOL_OFF 32
#define EXP_C(i) (EXP_POOL_OFF + (i) * 4)
/* slot 0=clamp_hi 1=clamp_lo 2=LOG2EF 3=0.5 4=ln2_hi 5=ln2_lo 6..11=p0..p5
 * 12=1.0 13=127(int). */
static const float exp_pool_consts[13] = {
    88.3762626647949f,    -88.3762626647949f,  1.44269504088896341f,
    0.5f,                 0.693359375f,        -2.12194440e-4f,
    1.9875691500E-4f,     1.3981999507E-3f,    8.3334519073E-3f,
    4.1665795894E-2f,     1.6666665459E-1f,    5.0000001201E-1f,
    1.0f};

/* x in ymm0 -> exp(x) in ymm2. Constants from the [rsp] pool. Clobbers
 * ymm0..5 (RAX is untouched). */
static int exp_compute(BinaryCodeBuffer *b) {
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(0)) ||
      !wcs_avx_vminps_ymm(b, 0, 0, 4) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(1)) ||
      !wcs_avx_vmaxps_ymm(b, 0, 0, 4)) {
    return 0;
  }
  /* fx = floor(x * LOG2EF + 0.5) */
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 1, BINARY_GP_RSP, EXP_C(2)) ||
      !wcs_avx_vmulps_ymm(b, 1, 0, 1) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(3)) ||
      !wcs_avx_vaddps_ymm(b, 1, 1, 4) ||
      !wcs_avx_vroundps_ymm(b, 1, 1, 1 /* floor */)) {
    return 0;
  }
  /* r = x - fx*ln2_hi - fx*ln2_lo  (Cody-Waite split) */
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(4)) ||
      !wcs_avx_vmulps_ymm(b, 5, 1, 4) || !wcs_avx_vsubps_ymm(b, 0, 0, 5) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(5)) ||
      !wcs_avx_vmulps_ymm(b, 5, 1, 4) || !wcs_avx_vsubps_ymm(b, 0, 0, 5)) {
    return 0;
  }
  /* Horner: y = ((((p0*r+p1)*r+p2)*r+p3)*r+p4)*r+p5 */
  if (!wcs_avx_vbroadcastss_ymm_mem(b, 2, BINARY_GP_RSP, EXP_C(6)) ||
      !wcs_avx_vmulps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(7)) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
    return 0;
  }
  for (int i = 8; i <= 11; i++) {
    if (!wcs_avx_vmulps_ymm(b, 2, 2, 0) ||
        !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(i)) ||
        !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
      return 0;
    }
  }
  /* y = y*r*r + r + 1 */
  if (!wcs_avx_vmulps_ymm(b, 3, 0, 0) || !wcs_avx_vmulps_ymm(b, 2, 2, 3) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 0) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(12)) ||
      !wcs_avx_vaddps_ymm(b, 2, 2, 4)) {
    return 0;
  }
  /* 2^fx via ((int)fx + 127) << 23 reinterpreted as float; y *= 2^fx */
  if (!wcs_avx_vcvttps2dq_ymm(b, 1, 1) ||
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(13)) ||
      !wcs_avx_vpaddd_ymm(b, 1, 1, 4) || !wcs_avx_vpslld_ymm_imm(b, 1, 1, 23) ||
      !wcs_avx_vmulps_ymm(b, 2, 2, 1)) {
    return 0;
  }
  return 1;
}

/* Fill the [rsp + EXP_POOL_OFF] constant pool (called once after the kernel's
 * stack reservation). */
static int exp_fill_pool(BinaryCodeBuffer *b) {
  for (int i = 0; i < 14; i++) {
    uint32_t bits = (i == 13) ? 127u : exp_f32_bits(exp_pool_consts[i]);
    if (!binary_emit_mov_reg_imm32_zero_extend(b, BINARY_GP_RAX, bits) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_RSP, EXP_C(i), BINARY_GP_RAX)) {
      return 0;
    }
  }
  return 1;
}

/* RCX has passed end-32, so 1..7 elements are left. Put that count in R8 and
 * jump to `tail`, which the caller patches to its n<8 gather/scatter path.
 *
 * Both in-place float kernels used to clamp RCX back to end-32 here and run
 * one more full vector, overlapping elements the loop had already done. That
 * is sound for a map into a separate destination; these write over their own
 * input, so the overlap applied the kernel twice. RDX needs no adjustment
 * because it advanced in lockstep with RCX. */
static int simd_tail_count_to_r8(BinaryCodeBuffer *b, size_t *tail) {
  return binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R9) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_R10, 0 /* add */, 32) &&
         wcs_sub_reg_reg64(b, BINARY_GP_R10, BINARY_GP_RCX) &&
         binary_emit_shift_reg_imm8(b, 5 /* shr */, BINARY_GP_R10, 2) &&
         binary_emit_mov_reg_reg(b, BINARY_GP_R8, BINARY_GP_R10) &&
         wcs_jcc(b, 0, tail);
}

/* One 8-wide exp at [RCX], then the loop's two exits: `done_main` when RCX has
 * reached end-32 and this was the last whole vector, otherwise advance 32 and
 * take `noclamp` unless that step left a partial tail. */
static int simd_exp_step(BinaryCodeBuffer *b, size_t *done_main,
                         size_t *noclamp) {
  return wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RCX, 0) && exp_compute(b) &&
         wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RCX, 0, 2) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) &&
         wcs_jcc(b, 0x83 /* jae */, done_main) &&
         wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32) &&
         binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) &&
         wcs_jcc(b, 0x86 /* jbe */, noclamp);
}

/* IR_OP_SIMD_EXP_F32: in-place a[i] = exp(a[i]) over a float32 array.
 * dest = array base, arguments[0] = element count. */
int code_generator_binary_emit_simd_exp_f32(CodeGenerator *generator,
                                            BinaryFunctionContext *context,
                                            const IRInstruction *instruction) {
  BinaryCodeBuffer *b = NULL;
  size_t loop_top = 0, done_main = 0, small = 0, fin = 0, noclamp = 0;
  size_t tail = 0;
  if (!generator || !context || !instruction ||
      instruction->argument_count < 1 || !instruction->arguments) {
    code_generator_set_error(generator, "Malformed simd_exp_f32");
    return 0;
  }
  b = &context->code;

  if (!code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->dest,
                                               BINARY_GP_RCX) ||
      !code_generator_binary_emit_operand_load(generator, context,
                                               &instruction->arguments[0],
                                               BINARY_GP_R8) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  /* Reserve 96 bytes once: [rsp+0..31] = the n<8 scratch buffer, [rsp+32..87] =
   * the constant pool. rsp stays put for the whole kernel (no calls), so both
   * the pool and the buffer are at fixed offsets. R9 (end) is a heap pointer,
   * unaffected by the stack reservation. */
  if (!binary_emit_sub_rsp_imm32(b, 96) || !exp_fill_pool(b)) {
    return 0;
  }
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R8, 8) ||
      !wcs_jcc(b, 0x82 /* jb */, &small)) {
    return 0;
  }
  /* R9 = last8 = end - 32; the 8-wide loop runs only over whole vectors. */
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 /* sub */, 32)) {
    return 0;
  }
  loop_top = b->size;
  if (!simd_exp_step(b, &done_main, &noclamp)) {
    return 0;
  }
  /* In-place: an overlapped element came back as exp(exp(x)). */
  if (!simd_tail_count_to_r8(b, &tail)) {
    return 0;
  }
  if (!wcs_patch_here(b, noclamp)) {
    return 0;
  }
  {
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, done_main) || !wcs_jcc(b, 0, &fin)) {
    return 0;
  }

  /* n < 8: copy n floats into the [rsp+0] scratch buffer (already reserved),
   * exp it 8-wide, copy the n results back. R9 saves the base; RCX/R10/R11/RAX
   * are scratch. */
  if (!wcs_patch_here(b, small) || !wcs_patch_here(b, tail) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t ci = b->size, di = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 /* jz */, &di) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RCX, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R11, 0, BINARY_GP_RAX) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, ci) ||
        !wcs_patch_here(b, di)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovups_ymm_mem(b, 0, BINARY_GP_RSP, 0) || !exp_compute(b) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 0, 2) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t co = b->size, dout = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 /* jz */, &dout) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_R11, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R9, 0, BINARY_GP_RAX) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R9, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, co) ||
        !wcs_patch_here(b, dout)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, fin)) {
    return 0;
  }
  if (!binary_emit_add_rsp_imm32(b, 96)) {
    return 0;
  }
  return wcs_avx_vzeroupper(b);
}

/* ===================== vectorized SiLU / SwiGLU gate (float32) ============= */
/* out[i] = silu(g[i]) * u[i] = (g[i] / (1 + exp(-g[i]))) * u[i], 8 lanes at a
 * time, reusing the exp polynomial. `has_mul` selects the SwiGLU `* u[i]` form
 * (RDX = u base) vs plain SiLU (no second array). Stack layout mirrors the exp
 * kernel: [rsp+0..31] g scratch, [rsp+32..87] exp constant pool (exp_compute
 * reads it at the fixed EXP_POOL_OFF), [rsp+88..119] u scratch. */
#define SILU_USCRATCH 88

/* g in ymm6 (and u in ymm7 when has_mul) -> result in ymm6. Clobbers ymm0..5
 * (exp_compute); ymm6/ymm7 survive it. */
static int silu_vec(BinaryCodeBuffer *b, int has_mul) {
  if (!wcs_avx_vpxor_ymm(b, 0, 0, 0) ||       /* ymm0 = 0 */
      !wcs_avx_vsubps_ymm(b, 0, 0, 6) ||      /* ymm0 = -g */
      !exp_compute(b) ||                       /* ymm2 = exp(-g) */
      !wcs_avx_vbroadcastss_ymm_mem(b, 4, BINARY_GP_RSP, EXP_C(12)) || /* 1.0 */
      !wcs_avx_vaddps_ymm(b, 2, 2, 4) ||      /* ymm2 = 1 + exp(-g) */
      !wcs_avx_vdivps_ymm(b, 6, 6, 2)) {      /* ymm6 = g / (1+exp(-g)) */
    return 0;
  }
  if (has_mul && !wcs_avx_vmulps_ymm(b, 6, 6, 7)) { /* ymm6 *= u */
    return 0;
  }
  return 1;
}

/* The SiLU/SwiGLU compute body, assuming RCX = out/g base, RDX = u base (when
 * has_mul), R8 = count are already marshalled: R9 = end pointer, the exp
 * constant pool, the 8-wide overlap loop, and the n<8 scratch path. Shared by
 * the fallback lowering (above) and the MIR inline passthrough (MIR_SIMD_SILU). */
int code_generator_binary_emit_simd_silu_f32_inline(BinaryCodeBuffer *b,
                                                    int has_mul) {
  size_t loop_top = 0, done_main = 0, small = 0, fin = 0, noclamp = 0;
  size_t tail = 0;
  /* end = out + count*4. */
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_R8) ||
      !binary_emit_shift_reg_imm8(b, 4, BINARY_GP_R9, 2) ||
      !wcs_add_reg_reg64(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  if (!binary_emit_sub_rsp_imm32(b, 128) || !exp_fill_pool(b)) {
    return 0;
  }
  if (!wcs_cmp_reg_imm32(b, BINARY_GP_R8, 8) ||
      !wcs_jcc(b, 0x82 /* jb */, &small)) {
    return 0;
  }
  /* R9 = end - 32: 8-wide loop overlapping the final vector. R11 tracks u so the
   * overlap clamp keeps g and u in lockstep (delta added to both). */
  if (!wcs_addsub_reg_imm8(b, BINARY_GP_R9, 1 /* sub */, 32)) {
    return 0;
  }
  loop_top = b->size;
  if (!wcs_avx_vmovups_ymm_mem(b, 6, BINARY_GP_RCX, 0)) {
    return 0;
  }
  if (has_mul && !wcs_avx_vmovups_ymm_mem(b, 7, BINARY_GP_RDX, 0)) {
    return 0;
  }
  if (!silu_vec(b, has_mul) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RCX, 0, 6) ||
      !binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x83 /* jae */, &done_main) ||
      !wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 32)) {
    return 0;
  }
  if (has_mul && !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 32)) {
    return 0;
  }
  if (!binary_emit_cmp_reg_reg(b, BINARY_GP_RCX, BINARY_GP_R9) ||
      !wcs_jcc(b, 0x86 /* jbe */, &noclamp)) {
    return 0;
  }
  /* In-place: an overlapped element came back as silu(silu(x)) * u * u. For
   * n = 11 that was elements 3..7, a 57% error on the first of them. */
  if (!simd_tail_count_to_r8(b, &tail)) {
    return 0;
  }
  if (!wcs_patch_here(b, noclamp)) {
    return 0;
  }
  {
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, loop_top)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, done_main) || !wcs_jcc(b, 0, &fin)) {
    return 0;
  }

  /* n < 8: gather g (and u) into the scratch buffers, run one 8-wide body on
   * them, scatter n results back. R9 saves the out base. */
  if (!wcs_patch_here(b, small) || !wcs_patch_here(b, tail) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R9, BINARY_GP_RCX)) {
    return 0;
  }
  /* Copy n floats of g -> [rsp+0], and (has_mul) n of u -> [rsp+SILU_USCRATCH].
   * RCX walks g, RDX walks u, R11 walks g-scratch, R10 counts down. */
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t ci = b->size, di = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 /* jz */, &di) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RCX, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R11, 0, BINARY_GP_RAX)) {
      return 0;
    }
    if (has_mul &&
        (!binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_RDX, 0) ||
         !binary_emit_mov_mem_reg32(b, BINARY_GP_R11, SILU_USCRATCH,
                                    BINARY_GP_RAX) ||
         !wcs_addsub_reg_imm8(b, BINARY_GP_RDX, 0, 4))) {
      return 0;
    }
    if (!wcs_addsub_reg_imm8(b, BINARY_GP_RCX, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, ci) ||
        !wcs_patch_here(b, di)) {
      return 0;
    }
  }
  if (!wcs_avx_vmovups_ymm_mem(b, 6, BINARY_GP_RSP, 0)) {
    return 0;
  }
  if (has_mul &&
      !wcs_avx_vmovups_ymm_mem(b, 7, BINARY_GP_RSP, SILU_USCRATCH)) {
    return 0;
  }
  if (!silu_vec(b, has_mul) ||
      !wcs_avx_vmovups_mem_ymm(b, BINARY_GP_RSP, 0, 6)) {
    return 0;
  }
  /* Scatter n results [rsp+0] -> out (R9). */
  if (!binary_emit_mov_reg_reg(b, BINARY_GP_R11, BINARY_GP_RSP) ||
      !binary_emit_mov_reg_reg(b, BINARY_GP_R10, BINARY_GP_R8)) {
    return 0;
  }
  {
    size_t co = b->size, dout = 0;
    if (!binary_emit_test_reg_reg(b, BINARY_GP_R10) ||
        !wcs_jcc(b, 0x84 /* jz */, &dout) ||
        !binary_emit_mov_reg_mem32(b, BINARY_GP_RAX, BINARY_GP_R11, 0) ||
        !binary_emit_mov_mem_reg32(b, BINARY_GP_R9, 0, BINARY_GP_RAX) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R9, 0, 4) ||
        !wcs_addsub_reg_imm8(b, BINARY_GP_R11, 0, 4) ||
        !binary_emit_sub_reg_imm32(b, BINARY_GP_R10, 1)) {
      return 0;
    }
    size_t jb = 0;
    if (!wcs_jcc(b, 0, &jb) || !wcs_patch_to(b, jb, co) ||
        !wcs_patch_here(b, dout)) {
      return 0;
    }
  }
  if (!wcs_patch_here(b, fin) || !binary_emit_add_rsp_imm32(b, 128)) {
    return 0;
  }
  return wcs_avx_vzeroupper(b);
}
