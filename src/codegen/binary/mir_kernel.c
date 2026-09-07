#include "codegen/binary/mir.h"

/* The inline-kernel table: legacy vector kernels the register-allocated backend
 * runs in place instead of dropping the whole enclosing function to the
 * spill-everything fallback.
 *
 * Every kernel here has the same shape -- a prologue of
 * `emit_operand_load(<some operand of the instruction>, <some register>)`, a
 * self-contained loop, and an epilogue of `emit_destination_store(...)`. Only
 * that prologue and epilogue tie a kernel to the fallback's named stack slots;
 * the body in between is register-to-register code that is already correct
 * anywhere. MIR_IR_KERNEL stages the operands into frame slots and points those
 * loads and stores at them, so a kernel needs no MIR-specific variant and no
 * awareness that it is running inside an allocated frame.
 *
 * A row is therefore just the IR opcode, the existing emitter, and the register
 * hazards the MIR allocator cannot see for itself. The kernels' own operand
 * shape checks still run: the recognizers in the optimizer only emit these
 * opcodes in forms their kernel accepts, and a malformed one still reports
 * through code_generator_set_error exactly as it does in the fallback.
 *
 * NOT in the table, and why:
 *   - IR_OP_SIMD_LCG_U32 writes ymm8/ymm9. Those alias xmm8..15, which is the
 *     allocator's second-tier float pool, and the kernel neither saves them nor
 *     is there a MIR clobber channel for XMM (gp_clobbers covers GP only).
 *   - IR_OP_SIMD_INSERTION_SORT_I32 loads its operands but stores nothing back
 *     by name, sorting through a pointer; it is only ever generated for a whole
 *     array, so it brings no function with it that the rest of the table does
 *     not already cover.
 *   - IR_OP_ROTATE_ADD, the tensor ops, and the async/barrier ops are not
 *     load-prologue/store-epilogue kernels at all: they interleave operand
 *     loads and destination stores through the body, so a staged slot is not
 *     enough to make them frame-independent.
 *   - The five kernels with their own MIR opcode (fill, SLP MAC, the two affine
 *     maps, SiLU, vloop) marshal through fixed registers and stay as they are:
 *     they run in the hottest inner loops in the codebase, where the register
 *     handoff beats a memory round trip.
 *
 * Clobber notes (audited against each kernel's body):
 *   - Only simd_minmax_i32 touches a callee-saved GP register, R14, and it does
 *     not preserve it. In the fallback that is invisible because no allocated
 *     value lives in a register across the kernel; here a spanning value could,
 *     so the row declares it.
 *   - lower_bound_i32 uses RSI but pushes and pops it; sum_u8, byte_map, and
 *     dot_f32 borrow stack below rsp with a balanced sub/add. Neither is a
 *     clobber the allocator must know about, but both are the reason a function
 *     running an inline kernel keeps its rbp frame: the staging slots are
 *     addressed off the frame base, and rsp is not stable across these bodies.
 *   - simd_lcg_u32 is the only kernel here that writes xmm6..15 (xmm6..9), and
 *     it saves and restores those four around its own body, so a caller's
 *     callee-saved lanes survive it. Nothing else touches them. A float value
 *     cannot span any kernel in a register regardless: a kernel is a call
 *     barrier and is not xmm-preserving, so mir_color_reg_mask gives such a
 *     value no register at all. */

#define GPCLOB(reg) (1u << (unsigned)(reg))

static const MirIrKernel kMirIrKernels[] = {
    /* Integer reductions and scans. */
    {IR_OP_SIMD_SUM_I32, "simd_sum_i32",
     code_generator_binary_emit_simd_sum_i32, 0},
    {IR_OP_SIMD_SUM_U8, "simd_sum_u8", code_generator_binary_emit_simd_sum_u8,
     0},
    {IR_OP_SIMD_DOT_I32, "simd_dot_i32",
     code_generator_binary_emit_simd_dot_i32, 0},
    {IR_OP_SIMD_DOT_I8, "simd_dot_i8", code_generator_binary_emit_simd_dot_i8,
     0},
    {IR_OP_SIMD_MINMAX_I32, "simd_minmax_i32",
     code_generator_binary_emit_simd_minmax_i32, GPCLOB(BINARY_GP_R14)},
    {IR_OP_PREFIX_SUM_I32, "prefix_sum_i32",
     code_generator_binary_emit_prefix_sum_i32, 0},
    {IR_OP_LOWER_BOUND_I32, "lower_bound_i32",
     code_generator_binary_emit_lower_bound_i32, 0},

    /* Integer element-wise maps. */
    {IR_OP_SIMD_SCALE_I32, "simd_scale_i32",
     code_generator_binary_emit_simd_scale_i32, 0},
    {IR_OP_SIMD_CLAMP_I32, "simd_clamp_i32",
     code_generator_binary_emit_simd_clamp_i32, 0},
    {IR_OP_SIMD_REVERSE_COPY_I32, "simd_reverse_copy_i32",
     code_generator_binary_emit_simd_reverse_copy_i32, 0},
    {IR_OP_SIMD_BYTE_MAP, "simd_byte_map",
     code_generator_binary_emit_simd_byte_map, 0},

    /* Searches. */
    {IR_OP_SIMD_FIND, "simd_find", code_generator_binary_emit_simd_find, 0},
    {IR_OP_COUNT_WORD_STARTS, "count_word_starts",
     code_generator_binary_emit_count_word_starts, 0},

    /* Float reductions. */
    {IR_OP_SIMD_SUM_F64, "simd_sum_f64",
     code_generator_binary_emit_simd_sum_f64, 0},
    {IR_OP_SIMD_SUM_F32, "simd_sum_f32",
     code_generator_binary_emit_simd_sum_f32, 0},
    {IR_OP_SIMD_DOT_F64, "simd_dot_f64",
     code_generator_binary_emit_simd_dot_f64, 0},
    {IR_OP_SIMD_DOT_F32, "simd_dot_f32",
     code_generator_binary_emit_simd_dot_f32, 0},

    /* Block move. */
    {IR_OP_MEMCPY_INLINE, "memcpy_inline",
     code_generator_binary_emit_memcpy_inline, 0},

    /* Vloop REDUCTIONS only (maps take the marshalled MIR_SIMD_VLOOP path;
     * the explicit gate/lowering cases route by reduce_op). Registers: ymm0-5,
     * kGp + R10/R11/RAX -- all volatile. */
    {IR_OP_SIMD_VLOOP_F64, "simd_vloop_f64",
     code_generator_binary_emit_simd_vloop_unmarshaled, 0},
    {IR_OP_SIMD_VLOOP_I32, "simd_vloop_i32",
     code_generator_binary_emit_simd_vloop_unmarshaled, 0},

    /* Audited load-prologue/store-epilogue kernels within ymm0-5 + volatile
     * GPs. insertion_sort stores nothing back by name (it sorts through a
     * pointer), which is fine: the bridge's read-back of an input slot is a
     * no-op. */
    {IR_OP_SIMD_INSERTION_SORT_I32, "simd_insertion_sort_i32",
     code_generator_binary_emit_simd_insertion_sort_i32, 0},
    {IR_OP_SIMD_I2F_REDUCE_F64, "simd_i2f_reduce_f64",
     code_generator_binary_emit_simd_i2f_reduce_f64, 0},
    {IR_OP_SIMD_EXP_F32, "simd_exp_f32",
     code_generator_binary_emit_simd_exp_f32, 0},
    {IR_OP_SIMD_AFFINE_MAP_F32, "simd_affine_map_f32",
     code_generator_binary_emit_simd_affine_map_f32, 0},
    {IR_OP_SIMD_AFFINE_MAP_F64, "simd_affine_map_f64",
     code_generator_binary_emit_simd_affine_map_f64, 0},
    {IR_OP_SIMD_OUTER_LANE_F64, "simd_outer_lane_f64",
     code_generator_binary_emit_simd_outer_lane_f64, 0},
    {IR_OP_SIMD_LCG_U32, "simd_lcg_u32",
     code_generator_binary_emit_simd_lcg_u32, 0},
};

#define MIR_IR_KERNEL_COUNT \
  ((int)(sizeof(kMirIrKernels) / sizeof(kMirIrKernels[0])))

int mir_ir_kernel_index_for_op(IROpcode op) {
  for (int i = 0; i < MIR_IR_KERNEL_COUNT; i++) {
    if (kMirIrKernels[i].ir_op == op) {
      return i;
    }
  }
  return -1;
}

const MirIrKernel *mir_ir_kernel_for_op(IROpcode op) {
  int i = mir_ir_kernel_index_for_op(op);
  return i < 0 ? NULL : &kMirIrKernels[i];
}

const MirIrKernel *mir_ir_kernel_at(int index) {
  if (index < 0 || index >= MIR_IR_KERNEL_COUNT) {
    return NULL;
  }
  return &kMirIrKernels[index];
}
