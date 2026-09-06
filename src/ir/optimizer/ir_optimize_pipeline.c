#include "ir_optimize_internal.h"
#include "../ir_verify.h"

typedef struct {
  IROptPassId id;
  IROptFunctionPass run;
  struct {
    unsigned all;
    unsigned any;
  } gate;
} IROptScheduledPass;

typedef struct {
  const char *name;
  const IROptScheduledPass *passes;
  size_t pass_count;
  int max_iterations;
} IROptFixpointStage;

#define IR_OPT_REQUIRE_NONE 0u
#define IR_OPT_FIXPOINT_MAX_ITERATIONS 8
#define IR_OPT_LABEL_JUMP (IR_OPT_FEATURE_LABEL | IR_OPT_FEATURE_JUMP)
#define IR_OPT_BRANCH_TESTS                                                   \
  (IR_OPT_FEATURE_JUMP | IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ)
#define IR_OPT_PASS_ALWAYS(id, fn)                                            \
  { IR_OPT_PASS_##id, fn, {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE} }
#define IR_OPT_PASS_WHEN_ALL(id, fn, all_features)                            \
  { IR_OPT_PASS_##id, fn, {all_features, IR_OPT_REQUIRE_NONE} }
#define IR_OPT_PASS_WHEN_ALL_ANY(id, fn, all_features, any_features)           \
  { IR_OPT_PASS_##id, fn, {all_features, any_features} }

/* Loop canonical form. The recognizers behind these read loop bodies, and
 * each of these rewrites a shape that would otherwise hide the body from all
 * of them: a declaration sitting inside the body, a global array's base
 * computed where no recognizer reads it as a base, a conditional accumulator,
 * a scan seeded from its first element. They run to a checked fixpoint; the
 * driver stops the build if the form fails to converge, so a recognizer never
 * matches against a shape the compiler has not finished normalizing. */
#define IR_OPT_CANONICAL_MAX_ITERATIONS 6
/* Two, because a third sweep was measured to change nothing.
 *
 * The worklist re-offers a recognizer whenever the IR moved after it last
 * came back clean, so the bound is only reached when the previous sweep
 * actually changed something. Two sweeps give every pass the second chance
 * the old array hand-coded by listing seven of them twice. A third produced
 * byte-identical object code across all 78 examples and test inputs, while
 * costing up to 36% on a function holding hundreds of loops, where one claim
 * un-cleans every other recognizer and forces a full re-scan. Raising this
 * again is fine, but measure the output, not just the runtime: if the bytes
 * do not move, the sweep is pure cost. */
#define IR_OPT_RECOGNIZER_MAX_ITERATIONS 2

#define IR_GATE_LOOP {IR_OPT_LABEL_JUMP, IR_OPT_REQUIRE_NONE}
#define IR_GATE_LOOP_LOAD                                                     \
  {IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}
#define IR_GATE_LOOP_BRANCH                                                   \
  {IR_OPT_LABEL_JUMP, IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ}

static const IROptNamedPass g_ir_pre_inline_canonical[] = {
    {"drop_dead_narrowing", ir_drop_dead_narrowing_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"hoist_global_bases", ir_hoist_global_bases_pass, IR_GATE_LOOP},
    {"hoist_row_pointers", ir_hoist_row_pointers_pass, IR_GATE_LOOP},
    {"if_convert_accumulate", ir_if_convert_accumulate_pass,
     IR_GATE_LOOP_BRANCH},
    {"scan_from_first", ir_normalize_scan_from_first_pass, IR_GATE_LOOP_LOAD},
};

/* Recognizers, each listed once in dependency order. The worklist driver
 * re-offers a pass whenever a later one changes the IR, which is what the
 * old duplicated entries did by hand. simd_minmax_i32 runs before
 * induction_pointer because the latter rewrites an int32 scan's counter into
 * a walking pointer, after which the extremum diamond indexes off something
 * no longer recognizable as the loop counter. */
static const IROptNamedPass g_ir_pre_inline_recognizers[] = {
    {"user_rewrite", ir_user_rewrite_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"simd_minmax_i32", ir_simd_minmax_i32_pass, IR_GATE_LOOP_LOAD},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass, IR_GATE_LOOP_LOAD},
    {"induction_pointer", ir_pointer_induction_pass, IR_GATE_LOOP},
    {"simd_dot_i32", ir_simd_dot_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_dot_i8", ir_simd_dot_i8_pass, IR_GATE_LOOP_LOAD},
    {"simd_insertion_sort_i32", ir_simd_insertion_sort_i32_pass,
     IR_GATE_LOOP_LOAD},
    {"lower_bound_i32", ir_lower_bound_i32_pass, IR_GATE_LOOP_LOAD},
};

/* Post-inline loop canonical form. hoist_body_locals joins here because
 * inlining plants a fresh declaration per parameter of every call it folded
 * into a body. */
static const IROptNamedPass g_ir_loop_canonical_passes[] = {
    {"drop_dead_narrowing", ir_drop_dead_narrowing_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"unify_param_copy_spelling", ir_unify_param_copy_spelling_pass,
     IR_GATE_LOOP},
    {"hoist_body_locals", ir_hoist_body_locals_pass, IR_GATE_LOOP},
    {"hoist_global_bases", ir_hoist_global_bases_pass, IR_GATE_LOOP},
    {"hoist_row_pointers", ir_hoist_row_pointers_pass, IR_GATE_LOOP},
    {"hoist_descriptor_loads", ir_hoist_descriptor_loads_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_load_bases", ir_hoist_load_bases_pass, IR_GATE_LOOP},
    {"hoist_dead_temps", ir_eliminate_dead_temp_writes_pass,
     {IR_OPT_REQUIRE_NONE, IR_OPT_REQUIRE_NONE}},
    {"hoist_invariant_assigns", ir_hoist_invariant_assigns_pass, IR_GATE_LOOP},
    {"if_convert_accumulate", ir_if_convert_accumulate_pass,
     IR_GATE_LOOP_BRANCH},
    {"scan_from_first", ir_normalize_scan_from_first_pass, IR_GATE_LOOP_LOAD},
};

/* Recognizers, each listed once in dependency order; the worklist driver
 * re-offers earlier passes when later ones change the IR. The two orderings
 * that matter: simd_minmax_reduce before induction_pointer (a rewritten
 * counter is no longer recognizable to the extremum diamond), and simd_fill
 * after induction_pointer (so range-for fills, already in pointer-walk form,
 * and while-loop fills, still indexed, both match). */
static const IROptNamedPass g_ir_recognizer_passes[] = {
    {"simd_minmax_reduce", ir_simd_minmax_reduce_pass, IR_GATE_LOOP_LOAD},
    {"induction_pointer", ir_pointer_induction_pass, IR_GATE_LOOP},
    {"simd_fill", ir_simd_fill_pass, IR_GATE_LOOP},
    {"prefix_sum_i32", ir_prefix_sum_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_minmax_i32", ir_simd_minmax_i32_pass, IR_GATE_LOOP_LOAD},
    {"simd_affine_map_float", ir_simd_affine_map_float_pass, IR_GATE_LOOP},
    {"simd_exp_f32", ir_simd_exp_f32_pass, IR_GATE_LOOP},
    {"simd_silu_f32", ir_simd_silu_f32_pass, IR_GATE_LOOP},
    {"simd_lcg", ir_simd_lcg_pass, IR_GATE_LOOP},
    {"simd_i2f_reduce", ir_simd_i2f_reduce_pass, IR_GATE_LOOP},
    {"simd_dot_float", ir_simd_dot_float_pass, IR_GATE_LOOP_LOAD},
    {"simd_sum_float", ir_simd_sum_float_pass, IR_GATE_LOOP_LOAD},
    {"auto_vectorize", ir_auto_vectorize_pass, IR_GATE_LOOP},
    {"auto_vectorize_int", ir_auto_vectorize_int_pass, IR_GATE_LOOP},
    {"auto_vectorize_find", ir_auto_vectorize_find_pass, IR_GATE_LOOP},
    {"outer_vectorize", ir_outer_vectorize_pass, IR_GATE_LOOP},
    {"simd_memory_map", ir_simd_memory_map_pass, IR_GATE_LOOP},
    {"lower_bound_i32", ir_lower_bound_i32_pass, IR_GATE_LOOP_LOAD},
    {"detect_shift_loops", ir_detect_shift_loops_pass, IR_GATE_LOOP},
    {"eliminate_congruent_ivs", ir_eliminate_congruent_ivs_pass,
     IR_GATE_LOOP},
    /* After congruent-IV merge so parallel lane indices appear as base+J.
     * SLP matches straight-line chains, so no loop gate. */
    {"simd_slp_mac_i32", ir_simd_slp_mac_i32_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"simd_slp_mac_i8", ir_simd_slp_mac_i8_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
};

/* Run once, in order, after the recognizer worklist has converged. Both
 * mutate loop bodies in ways that would defeat every recognizer above:
 * if_convert collapses register-only diamonds to branchless selects, and
 * prefetch_indirect inserts control flow into loops with load-fed accesses
 * -- shapes no vectorizer can claim. Keeping them out of the worklist is
 * what keeps the recognizers from ever seeing their output. */
static const IROptNamedPass g_ir_post_recognizer_tail[] = {
    {"if_convert", ir_if_convert_pass,
     {IR_OPT_FEATURE_LABEL,
      IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_BRANCH_EQ}},
    {"prefetch_indirect", ir_prefetch_indirect_pass, IR_GATE_LOOP_LOAD},
};

/* Lowering cleanups. Each erases a shape a recognizer reads -- a load becomes
 * a copy, two arms become one, a widening cast disappears -- so they run after
 * every recognizer has had its chance AND outside the stage --explain re-runs
 * to test a hypothesis, which would otherwise measure the cleaned-up body
 * instead of the one the programmer wrote. The two general passes behind them
 * retire the copies they leave; nothing later in the pipeline would. */
static const IROptNamedPass g_ir_lowering_cleanup[] = {
    /* This one matches the shape the programmer wrote: two arms that differ
     * only in a field offset. The hoists below rewrite exactly those arms, and
     * an arm reading a base the hoist lifted out no longer looks like its
     * partner, so the branch survives as a data-dependent branch the hardware
     * cannot predict. It goes first, where the shape is still intact. */
    {"select_field_load", ir_select_adjacent_field_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_BRANCH_ZERO, IR_OPT_REQUIRE_NONE}},
    {"promote_loop_memory", ir_promote_loop_memory_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_invariant_loads", ir_hoist_invariant_loads_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"hoist_invariant_arith", ir_hoist_invariant_arith_pass,
     {IR_OPT_FEATURE_LABEL, IR_OPT_REQUIRE_NONE}},
    {"widen_subword_cast", ir_widen_subword_load_cast_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"widen_byte_pack", ir_widen_byte_pack_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"ascii_casefold_range", ir_ascii_casefold_range_pass,
     {IR_OPT_FEATURE_BRANCH_ZERO, IR_OPT_REQUIRE_NONE}},
    /* After the casefold pass, which claims the two-range letter test and
     * produces something better than a single subtract-and-compare. */
    {"fold_range_test", ir_fold_range_test_pass,
     {IR_OPT_FEATURE_BRANCH_ZERO, IR_OPT_REQUIRE_NONE}},
    {"or_chain_bitset", ir_or_chain_to_bitset_pass,
     {IR_OPT_FEATURE_BRANCH_EQ | IR_OPT_FEATURE_BRANCH_ZERO,
      IR_OPT_REQUIRE_NONE}},
    {"float_pow2_reciprocal", ir_float_divide_by_power_of_two_pass,
     {IR_OPT_FEATURE_DIV, IR_OPT_REQUIRE_NONE}},
    {"redundancy_elim", ir_redundancy_elimination_pass,
     {IR_OPT_FEATURE_LOAD, IR_OPT_REQUIRE_NONE}},
    {"redundancy_copy_prop", ir_copy_and_constant_propagation_pass, {0, 0}},
    {"redundancy_dead_temps", ir_eliminate_dead_temp_writes_pass,
     {IR_OPT_FEATURE_TEMP_WRITE, IR_OPT_REQUIRE_NONE}},
    {"auto_vectorize_class_find", ir_auto_vectorize_find_pass,
     {IR_OPT_FEATURE_LOAD | IR_OPT_FEATURE_LABEL,
      IR_OPT_FEATURE_BRANCH_ZERO}},
};


/* SROA runs after copy/coalesce fold inlined struct copies into clean
 * symbol-to-symbol form, and before CSE/dead-temp cleanup. */
static const IROptScheduledPass g_ir_fixpoint_passes[] = {
    IR_OPT_PASS_WHEN_ALL(REDUCTION_UNROLL, ir_reduction_unroll_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(COPY_AND_CONSTANT_PROPAGATION,
                       ir_copy_and_constant_propagation_pass),
    IR_OPT_PASS_ALWAYS(USER_REWRITE, ir_user_rewrite_pass),
    IR_OPT_PASS_ALWAYS(FUSE_TENSOR_MMA_CHAINS,
                       ir_fuse_tensor_mma_chains_pass),
    IR_OPT_PASS_ALWAYS(FUSE_ROTATE_ADD, ir_fuse_rotate_add_pass),
    IR_OPT_PASS_WHEN_ALL(STRENGTH_REDUCE_ROTATE_LOOPS,
                         ir_strength_reduce_rotate_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(UNROLL_SMALL_CONST_BOUND_LOOPS,
                         ir_unroll_small_const_bound_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(UNROLL_ANNOTATED_LOOPS,
                         ir_unroll_annotated_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_WHEN_ALL(FOLD_POPCOUNT_BYTE_LOOP,
                         ir_fold_popcount_byte_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(FUSE_POPCOUNT_BUFFER_LOOP,
                         ir_fuse_popcount_buffer_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(COLLATZ_ODD_STEP_FOLD,
                         ir_collatz_odd_step_fold_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(COALESCE_SINGLE_USE_TEMP_ASSIGN,
                         ir_coalesce_single_use_temp_assign_pass,
                         IR_OPT_FEATURE_ASSIGN),
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_SINGLE_USE_FLOAT_SYMBOL_COPIES,
                         ir_eliminate_single_use_float_symbol_copies_pass,
                         IR_OPT_FEATURE_ASSIGN),
    IR_OPT_PASS_ALWAYS(SROA, ir_sroa_pass),
    IR_OPT_PASS_ALWAYS(COMMON_SUBEXPRESSION_ELIMINATION,
                       ir_common_subexpression_elimination_pass),
    IR_OPT_PASS_ALWAYS(CONSTANT_AND_BRANCH_SIMPLIFY,
                       ir_constant_and_branch_simplify_pass),
    IR_OPT_PASS_WHEN_ALL(REASSOCIATE_CONSTANTS, ir_reassociate_constants_pass,
                         IR_OPT_FEATURE_BINARY),
    /* E-class pilot (METTLE_EGRAPH=1): inert by default; when enabled it
     * rides this driver's verify snapshot like any other pass. */
    IR_OPT_PASS_WHEN_ALL(EGRAPH_SIMPLIFY, ir_egraph_simplify_pass,
                         IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(COUNT_WORD_STARTS, ir_count_word_starts_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_DEAD_TEMP_WRITES,
                         ir_eliminate_dead_temp_writes_pass,
                         IR_OPT_FEATURE_TEMP_WRITE),
    IR_OPT_PASS_WHEN_ALL_ANY(THREAD_JUMP_TARGETS,
                             ir_thread_jump_targets_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
    IR_OPT_PASS_WHEN_ALL(NULL_CHECK_LICM, ir_null_check_licm_pass,
                         IR_OPT_FEATURE_WHILE_LABEL |
                             IR_OPT_FEATURE_BRANCH_ZERO | IR_OPT_FEATURE_CALL),
    IR_OPT_PASS_WHEN_ALL_ANY(REMOVE_EMPTY_CONDITIONAL_DIAMONDS,
                             ir_remove_empty_conditional_diamonds_pass,
                             IR_OPT_LABEL_JUMP,
                             IR_OPT_FEATURE_BRANCH_ZERO |
                                 IR_OPT_FEATURE_BRANCH_EQ),
    IR_OPT_PASS_WHEN_ALL_ANY(REMOVE_REDUNDANT_FALLTHROUGH_BRANCHES,
                             ir_remove_redundant_fallthrough_branches_pass,
                             IR_OPT_FEATURE_LABEL,
                             IR_OPT_FEATURE_BRANCH_ZERO |
                                 IR_OPT_FEATURE_BRANCH_EQ),
    IR_OPT_PASS_WHEN_ALL(REMOVE_REDUNDANT_JUMPS,
                         ir_remove_redundant_jumps_pass, IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(ELIMINATE_UNREACHABLE_STRAIGHTLINE,
                       ir_eliminate_unreachable_straightline_pass),
    IR_OPT_PASS_WHEN_ALL_ANY(ELIMINATE_UNREACHABLE_BLOCKS,
                             ir_eliminate_unreachable_blocks_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
    IR_OPT_PASS_WHEN_ALL(REMOVE_UNUSED_LABELS, ir_remove_unused_labels_pass,
                         IR_OPT_FEATURE_LABEL),
    IR_OPT_PASS_ALWAYS(MEMCPY_INLINE, ir_memcpy_inline_pass),
    IR_OPT_PASS_WHEN_ALL(MEMCMP_BYTE_LOOP, ir_memcmp_byte_loop_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_BRANCH_ZERO |
                             IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_ALWAYS(ELIMINATE_LOAD_SYMBOL_COPY,
                       ir_eliminate_load_symbol_copy_pass),
    IR_OPT_PASS_WHEN_ALL(SIMD_SUM_I32, ir_simd_sum_i32_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_SUM_U8, ir_simd_sum_u8_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_BYTE_MAP, ir_simd_byte_map_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_DOT_I32, ir_simd_dot_i32_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_DOT_I8, ir_simd_dot_i8_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(SIMD_INSERTION_SORT_I32,
                         ir_simd_insertion_sort_i32_pass,
                         IR_OPT_LABEL_JUMP | IR_OPT_FEATURE_LOAD),
};

static const IROptFixpointStage g_ir_fixpoint_stage = {
    "main fixpoint",
    g_ir_fixpoint_passes,
    IR_ARRAY_COUNT(g_ir_fixpoint_passes),
    IR_OPT_FIXPOINT_MAX_ITERATIONS,
};

/* Portable targets consume the same scalar/control-flow IR but cannot accept
 * the x86-only SIMD idioms produced by the full pipeline. Keep this schedule
 * intentionally target-neutral: no vector opcodes, rotate fusion, host memory
 * intrinsics, prefetch, or target-specific cost model. */
static const IROptScheduledPass g_ir_portable_fixpoint_passes[] = {
    IR_OPT_PASS_ALWAYS(DROP_DEAD_NARROWING, ir_drop_dead_narrowing_pass),
    IR_OPT_PASS_WHEN_ALL(UNROLL_ANNOTATED_LOOPS,
                         ir_unroll_annotated_loops_pass,
                         IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(COPY_AND_CONSTANT_PROPAGATION,
                       ir_copy_and_constant_propagation_pass),
    IR_OPT_PASS_ALWAYS(FUSE_TENSOR_MMA_CHAINS,
                       ir_fuse_tensor_mma_chains_pass),
    IR_OPT_PASS_WHEN_ALL(PROMOTE_GPU_ASYNC_STAGING,
                         ir_promote_gpu_async_staging_pass,
                         IR_OPT_FEATURE_LOAD),
    IR_OPT_PASS_WHEN_ALL(COALESCE_SINGLE_USE_TEMP_ASSIGN,
                         ir_coalesce_single_use_temp_assign_pass,
                         IR_OPT_FEATURE_ASSIGN),
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_SINGLE_USE_FLOAT_SYMBOL_COPIES,
                         ir_eliminate_single_use_float_symbol_copies_pass,
                         IR_OPT_FEATURE_ASSIGN),
    IR_OPT_PASS_ALWAYS(SROA, ir_sroa_pass),
    IR_OPT_PASS_ALWAYS(COMMON_SUBEXPRESSION_ELIMINATION,
                       ir_common_subexpression_elimination_pass),
    IR_OPT_PASS_ALWAYS(CONSTANT_AND_BRANCH_SIMPLIFY,
                       ir_constant_and_branch_simplify_pass),
    IR_OPT_PASS_WHEN_ALL(REASSOCIATE_CONSTANTS, ir_reassociate_constants_pass,
                         IR_OPT_FEATURE_BINARY),
    IR_OPT_PASS_WHEN_ALL(ELIMINATE_DEAD_TEMP_WRITES,
                         ir_eliminate_dead_temp_writes_pass,
                         IR_OPT_FEATURE_TEMP_WRITE),
    IR_OPT_PASS_WHEN_ALL_ANY(THREAD_JUMP_TARGETS,
                             ir_thread_jump_targets_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
    IR_OPT_PASS_WHEN_ALL_ANY(REMOVE_EMPTY_CONDITIONAL_DIAMONDS,
                             ir_remove_empty_conditional_diamonds_pass,
                             IR_OPT_LABEL_JUMP,
                             IR_OPT_FEATURE_BRANCH_ZERO |
                                 IR_OPT_FEATURE_BRANCH_EQ),
    IR_OPT_PASS_WHEN_ALL_ANY(REMOVE_REDUNDANT_FALLTHROUGH_BRANCHES,
                             ir_remove_redundant_fallthrough_branches_pass,
                             IR_OPT_FEATURE_LABEL,
                             IR_OPT_FEATURE_BRANCH_ZERO |
                                 IR_OPT_FEATURE_BRANCH_EQ),
    IR_OPT_PASS_WHEN_ALL(REMOVE_REDUNDANT_JUMPS,
                         ir_remove_redundant_jumps_pass, IR_OPT_LABEL_JUMP),
    IR_OPT_PASS_ALWAYS(ELIMINATE_UNREACHABLE_STRAIGHTLINE,
                       ir_eliminate_unreachable_straightline_pass),
    IR_OPT_PASS_WHEN_ALL_ANY(ELIMINATE_UNREACHABLE_BLOCKS,
                             ir_eliminate_unreachable_blocks_pass,
                             IR_OPT_FEATURE_LABEL, IR_OPT_BRANCH_TESTS),
    IR_OPT_PASS_WHEN_ALL(REMOVE_UNUSED_LABELS, ir_remove_unused_labels_pass,
                         IR_OPT_FEATURE_LABEL),
    IR_OPT_PASS_ALWAYS(ELIMINATE_LOAD_SYMBOL_COPY,
                       ir_eliminate_load_symbol_copy_pass),
};

static const IROptFixpointStage g_ir_portable_fixpoint_stage = {
    "target-neutral fixpoint",
    g_ir_portable_fixpoint_passes,
    IR_ARRAY_COUNT(g_ir_portable_fixpoint_passes),
    IR_OPT_FIXPOINT_MAX_ITERATIONS,
};

int ir_optimize_pre_inline_function(IRFunction *function) {
  mettle_compiler_ctx_set_pass_name("pre-inline canonicalization");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_pre_inline_canonical,
          IR_ARRAY_COUNT(g_ir_pre_inline_canonical),
          IR_OPT_CANONICAL_MAX_ITERATIONS, "pre-inline loop canonical form",
          "IR optimization pre-inline pass failed", 1)) {
    return 0;
  }
  mettle_compiler_ctx_set_pass_name("pre-inline idiom recognition");
  return ir_run_named_stage_fixpoint(
      function, g_ir_pre_inline_recognizers,
      IR_ARRAY_COUNT(g_ir_pre_inline_recognizers),
      IR_OPT_RECOGNIZER_MAX_ITERATIONS, "pre-inline idiom recognition",
      "IR optimization pre-inline pass failed", 0);
}

/* METTLE_LOOP_FINGERPRINT=1: pair every counted loop's dataflow fingerprint
 * with whether a recognizer claimed it (its header label is gone after the
 * stages). CI diffs these lines across compiler versions; a fingerprint that
 * held still while its claim flipped is recognizer rot. */
#define IR_FP_MAX_LOOPS 64

typedef struct {
  char *label; /* owned copy: a recognizer that claims the loop frees the
                  label instruction's text before the report runs */
  unsigned long long fp;
} IRLoopFpEntry;

static int ir_loop_fp_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *spec = getenv("METTLE_LOOP_FINGERPRINT");
    cached = (spec && spec[0] != '\0' && strcmp(spec, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static int ir_loop_fp_snapshot(IRFunction *function, IRLoopFpEntry *entries) {
  int count = 0;
  int total = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LABEL || !ir_label_is_while_header(ins->text)) {
      continue;
    }
    IRAffineLoop loop;
    if (!ir_affine_model_loop(function, i, &loop)) {
      continue;
    }
    total++;
    if (count >= IR_FP_MAX_LOOPS) {
      continue; /* keep counting so the cap can be reported honestly */
    }
    entries[count].label = mettle_strdup(ins->text);
    if (!entries[count].label) {
      continue;
    }
    entries[count].fp = ir_affine_loop_fingerprint(function, &loop);
    count++;
  }
  /* A gate that quietly covered a prefix of the loops would read as a clean
   * run. Say what was left out instead. */
  if (total > count) {
    fprintf(stderr,
            "[loop-fp] NOTE function=%s has %d modelled loops but the cap is "
            "%d; %d are unchecked\n",
            function->name ? function->name : "<anonymous>", total,
            IR_FP_MAX_LOOPS, total - count);
  }
  return count;
}

static void ir_loop_fp_report(const IRFunction *function,
                              const IRLoopFpEntry *entries, int count) {
  for (int e = 0; e < count; e++) {
    int survives = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *ins = &function->instructions[i];
      if (ins->op == IR_OP_LABEL && ins->text &&
          strcmp(ins->text, entries[e].label) == 0) {
        survives = 1;
        break;
      }
    }
    fprintf(stderr, "[loop-fp] function=%s loop=%s fp=%016llx claimed=%d\n",
            function->name ? function->name : "<anonymous>", entries[e].label,
            entries[e].fp, survives ? 0 : 1);
  }
}

static void ir_loop_fp_destroy(IRLoopFpEntry *entries, int count) {
  for (int e = 0; e < count; e++) {
    free(entries[e].label);
  }
}

/* Canonical form (checked fixpoint) -> recognizer worklist -> tail. */
static int ir_run_post_fixpoint_stages(IRFunction *function) {
  mettle_compiler_ctx_set_pass_name("loop canonical form");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_loop_canonical_passes,
          IR_ARRAY_COUNT(g_ir_loop_canonical_passes),
          IR_OPT_CANONICAL_MAX_ITERATIONS, "loop canonical form",
          "IR optimization pass failed", 1)) {
    return 0;
  }
  /* The recognizers start here, and every one of them assumes the form the
   * stage above was supposed to establish. Check it structurally before they
   * run: a canonicalizer whose matcher stopped firing converges just as
   * quietly as one with nothing to do, and this is the difference.
   *
   * --verify is the one caller allowed to break the form on purpose: a
   * divergence quarantines the offending pass and restores the pre-pass IR,
   * so a canonicalizer that was supposed to run did not. That is a reported
   * loss of optimization, not rot, and ir_verify.h promises it "costs only
   * optimization, never correctness" -- aborting here charged a false
   * positive in the input generator as a compiler crash. */
  {
    char detail[192];
    detail[0] = '\0';
    if (!ir_verify_loop_canonical_form(function, detail, sizeof(detail))) {
      int quarantined = 0;
      for (size_t i = 0; i < IR_ARRAY_COUNT(g_ir_loop_canonical_passes); i++) {
        if (ir_verify_pass_quarantined(function,
                                       g_ir_loop_canonical_passes[i].name)) {
          quarantined = 1;
          break;
        }
      }
      if (!quarantined) {
        fprintf(stderr,
                "mettle: internal error: loop canonical form does not hold in "
                "function '%s': %s\n",
                function->name ? function->name : "<anonymous>", detail);
        mettle_compiler_ice("IR loop canonical form violated");
      }
    }
  }

  mettle_compiler_ctx_set_pass_name("post-fixpoint idiom recognition");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_recognizer_passes,
          IR_ARRAY_COUNT(g_ir_recognizer_passes),
          IR_OPT_RECOGNIZER_MAX_ITERATIONS, "post-fixpoint idiom recognition",
          "IR optimization pass failed", 0)) {
    return 0;
  }
  mettle_compiler_ctx_set_pass_name("post-recognizer tail");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_post_recognizer_tail,
          IR_ARRAY_COUNT(g_ir_post_recognizer_tail), 1, "post-recognizer tail",
          "IR optimization pass failed", 0)) {
    return 0;
  }
  return 1;
}

static int ir_scheduled_pass_is_enabled(const IROptScheduledPass *pass,
                                        unsigned features) {
  if ((features & pass->gate.all) != pass->gate.all) {
    return 0;
  }
  return pass->gate.any == IR_OPT_REQUIRE_NONE ||
         (features & pass->gate.any) != 0;
}

/* Cached: getenv takes a lock and walks the whole environment on Windows, and
 * this is asked once per function. */
static int ir_time_functions_enabled(void) {
  static int cached = -1;

  if (cached < 0) {
    cached = getenv("METTLE_TIME_FUNCTIONS") != NULL;
  }
  return cached;
}

static int ir_run_fixpoint_stage(IRFunction *function,
                                 const IROptFixpointStage *stage) {
  if (!stage || !stage->passes || stage->max_iterations <= 0) {
    return 0;
  }

  unsigned long long version = 1;
  int used = 1;
  unsigned long long clean_version[IR_OPT_PASS_COUNT];
  for (int i = 0; i < IR_OPT_PASS_COUNT; i++) {
    clean_version[i] = 0;
  }

  for (int iteration = 0; iteration < stage->max_iterations; iteration++) {
    int changed = 0;
    IROptFunctionFeatures features;

    /* Retiring an instruction leaves a NOP behind, and by the third iteration
     * of a big body most of it is holes that every pass below still walks.
     * Positions move, so every pass has to be offered the body again. */
    if (ir_function_drop_dead_nops(function) > 0) {
      version++;
    }
    mettle_compiler_ctx_set_fixpoint_iteration(iteration + 1);
    ir_collect_function_features(function, &features);
    unsigned feature_flags = ir_opt_feature_flags(&features);

    for (size_t pass_index = 0; pass_index < stage->pass_count; pass_index++) {
      const IROptScheduledPass *pass = &stage->passes[pass_index];
      int enabled = ir_scheduled_pass_is_enabled(pass, feature_flags);
      if (!ir_run_fixpoint_pass(function, pass->id, pass->run, enabled, &version,
                                clean_version, &changed)) {
        return 0;
      }
    }

    if (!changed) {
      break;
    }
    used = iteration + 2;
  }

  if (ir_time_functions_enabled() && function->instruction_count > 800) {
    fprintf(stderr, "   fixpoint %s: %d iterations over %zu instructions\n",
            function->name ? function->name : "?", used,
            function->instruction_count);
  }
  mettle_compiler_ctx_set_fixpoint_iteration(0);
  return 1;
}

int ir_optimize_function_pipeline(IRFunction *function) {
  if (!function) {
    return 0;
  }

  /* Weigh the function on the way in and out: the difference is what the whole
   * pipeline achieved, which is the one number the per-pass ledger cannot show. */
  ir_explain_function_before(function);

  {
    int pre_changed = 0;
    if (!ir_fuse_rotate_add_pass(function, &pre_changed)) {
      return 0;
    }
  }

  /* Snapshot ahead of the main fixpoint, not just ahead of the post-fixpoint
   * worklist: the sum, dot, byte-map and SLP recognizers run as fixpoint
   * passes, so a loop one of them claims has already lost its header by the
   * time the worklist starts. Sampling here is what makes the gate cover
   * every recognizer instead of most of them. */
  IRLoopFpEntry fp_entries[IR_FP_MAX_LOOPS];
  int fp_count = 0;
  if (ir_loop_fp_enabled()) {
    fp_count = ir_loop_fp_snapshot(function, fp_entries);
  }

  if (!ir_run_fixpoint_stage(function, &g_ir_fixpoint_stage)) {
    ir_loop_fp_destroy(fp_entries, fp_count);
    return 0;
  }

  if (!ir_run_post_fixpoint_stages(function)) {
    ir_loop_fp_destroy(fp_entries, fp_count);
    return 0;
  }

  if (fp_count > 0) {
    ir_loop_fp_report(function, fp_entries, fp_count);
  }
  ir_loop_fp_destroy(fp_entries, fp_count);

  /* Enforce `@simd` contracts now that every vectorizer has had its chance,
   * then strip the markers before CFG rebuild / codegen. */
  double t0 = ir_pass_time_begin();
  if (!ir_verify_simd_contracts(function)) {
    return 0;
  }
  ir_pass_time_end("verify_simd_contracts [stage]", t0);

  /* Weigh the body --explain reports on before the lowering cleanups touch it:
   * its fix simulations re-run the recognizers on a clone, and a cleanup has
   * already erased the shapes they read. */
  ir_explain_function_after(function);

  mettle_compiler_ctx_set_pass_name("lowering cleanup");
  if (!ir_run_named_stage_fixpoint(
          function, g_ir_lowering_cleanup,
          IR_ARRAY_COUNT(g_ir_lowering_cleanup), 1, "lowering cleanup",
          "IR optimization pass failed", 0)) {
    return 0;
  }

  t0 = ir_pass_time_begin();
  int ok = ir_function_rebuild_cfg(function);
  ir_pass_time_end("rebuild_cfg [stage]", t0);
  return ok;
}

/* --explain hypothesis testing: re-run the optimization stages (including
 * every vectorizer) on a scratch clone that carries a simulated fix. No
 * contract verification, no CFG rebuild -- the caller inspects the clone's
 * marker regions itself and then throws it away. */
int ir_optimize_function_revectorize(IRFunction *function) {
  if (!function) {
    return 0;
  }
  if (!ir_run_fixpoint_stage(function, &g_ir_fixpoint_stage)) {
    return 0;
  }
  return ir_run_post_fixpoint_stages(function);
}

static void ir_set_current_function_context(IRFunction *function) {
  if (function) {
    mettle_compiler_ctx_set_function_name(
        function->name ? function->name : "<anonymous>");
  }
}

static int ir_run_program_stage_for_each_function(
    IRProgram *program, int (*run)(IRFunction *function)) {
  int report = ir_time_functions_enabled();
  double slowest = 0.0;
  double total = 0.0;
  double tiny_ms = 0.0;
  size_t tiny_count = 0;
  const char *slowest_name = NULL;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    double began = report ? mettle_now_ms() : 0.0;
    if (function && function->rewrite_role) {
      continue;
    }
    ir_set_current_function_context(function);
    if (!run(function)) {
      return 0;
    }
    if (report) {
      double took = mettle_now_ms() - began;
      total += took;
      if (function->instruction_count < 64) {
        tiny_ms += took;
        tiny_count++;
      }
      if (took > slowest) {
        slowest = took;
        slowest_name = function->name;
      }
    }
  }
  if (report) {
    fprintf(stderr,
            "-- stage over %zu functions: %.1f ms total, slowest %.1f ms (%s); "
            "%zu under 64 instructions cost %.1f ms --\n",
            program->function_count, total, slowest,
            slowest_name ? slowest_name : "?", tiny_count, tiny_ms);
  }
  return 1;
}

static int ir_optimize_portable_program_pipeline(
    IRProgram *program, const IROptimizeOptions *options) {
  IRGpuCallGraph graph = {0};
  char *graph_error = NULL;
  int gpu_only = options && options->gpu_device_only;
  int ok = 1;

  ir_optimize_reset_user_error();
  ir_optimize_set_simd_report(0);
  ir_optimize_set_explain(options && options->explain,
                          options ? options->explain_focus_file : NULL);
  ir_function_index_reset();
  ir_verify_begin_program(program);

  if (gpu_only &&
      !ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
    fprintf(stderr, "GPU optimization eligibility failed: %s\n",
            graph_error ? graph_error : "invalid device module");
    free(graph_error);
    /* That message is the user diagnostic (uniformity violation, recursive
     * device call graph, ...). Marking it a user error keeps the driver from
     * escalating to an internal-compiler-error report attributed to whatever
     * function happened to hold the compiler context last. */
    ir_optimize_note_user_error();
    ir_verify_end_program();
    ir_function_index_reset();
    return 0;
  }

  ir_explain_set_program(program);

  /* Inlining, for device modules only. It matters more here than on the host:
   * a helper left out of line is a PTX `.func` reached by `call.uni`, which
   * means the call ABI's parameter space plus a register allocation that stops
   * at the call boundary -- a cost every work item pays. The GPU call-graph
   * verifier above has already rejected recursion and indirect calls, so what
   * reaches this point is a DAG of direct calls.
   *
   * Restricted to `gpu_only` rather than every target-neutral compile: the
   * AArch64 path shares this pipeline, and its inlining policy is its own
   * question. */
  if (gpu_only && (!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("inline_small_functions")) {
    int inlining_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_small_functions");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_inline_small_functions_pass(program, &inlining_changed)) {
      mettle_compiler_ice("IR optimization inlining pass failed");
    }
    ir_pass_time_end("inline_small_functions [program]", t0);

    if (inlining_changed) {
      /* Inlining rewrites bodies, so every derived structure is stale. The
       * per-function stage below reads the CFG, and the reachability filter
       * reads the call graph: a helper every caller absorbed has no callers
       * left and is no longer device code. Rebuild both before either is
       * consulted again. */
      for (size_t i = 0; i < program->function_count; i++) {
        if (program->functions[i] &&
            !ir_function_rebuild_cfg(program->functions[i])) {
          ok = 0;
        }
      }
      ir_gpu_call_graph_destroy(&graph);
      memset(&graph, 0, sizeof(graph));
      graph_error = NULL;
      if (ok &&
          !ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
        fprintf(stderr, "GPU optimization eligibility failed: %s\n",
                graph_error ? graph_error : "invalid device module");
        free(graph_error);
        ir_optimize_note_user_error();
        ok = 0;
      }
    }
  }

  /* `@pure` loop-invariant call hoisting, after inlining so a body the caller
   * absorbed is hoisted as ordinary loop-invariant code instead. */
  if (ok && gpu_only && !ir_pass_name_is_skipped("hoist_pure_calls")) {
    int pure_licm_changed = 0;
    mettle_compiler_ctx_set_pass_name("hoist_pure_calls");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_hoist_pure_calls_pass(program, &pure_licm_changed)) {
      mettle_compiler_ice("IR optimization pure-call hoisting pass failed");
    }
    ir_pass_time_end("hoist_pure_calls [program]", t0);
  }

  for (size_t i = 0; ok && i < program->function_count; i++) {
    if (gpu_only && (!graph.reachable || !graph.reachable[i])) continue;
    IRFunction *function = program->functions[i];
    ir_set_current_function_context(function);
    if (!ir_run_fixpoint_stage(function, &g_ir_portable_fixpoint_stage) ||
        !ir_function_rebuild_cfg(function)) {
      ok = 0;
      break;
    }
  }

  /* `@inline!` is a contract: a surviving call site fails the build. It cannot
   * mean that on one target and nothing on another. */
  if (ok && gpu_only && (!options || !options->preserve_function_boundaries) &&
      !ir_inline_enforce_contracts(program)) {
    ir_optimize_note_user_error();
    ok = 0;
  }

  ir_explain_set_program(NULL);
  ir_gpu_call_graph_destroy(&graph);
  ir_explain_flush();
  ir_pass_time_report();
  ir_verify_end_program();
  ir_function_index_reset();
  return ok;
}

static int ir_safety_analysis_function(IRFunction *function) {
  static const IROptNamedPass passes[] = {
      {"safety_narrowing", ir_drop_dead_narrowing_pass, {0, 0}},
      {"safety_copy_constants", ir_copy_and_constant_propagation_pass, {0, 0}},
      {"safety_coalesce", ir_coalesce_single_use_temp_assign_pass, {0, 0}},
      {"safety_branches", ir_constant_and_branch_simplify_pass, {0, 0}},
      {"safety_dead_temps", ir_eliminate_dead_temp_writes_pass, {0, 0}},
  };
  return ir_run_named_stage_fixpoint(function, passes, IR_ARRAY_COUNT(passes),
      IR_OPT_FIXPOINT_MAX_ITERATIONS, "safety scalar analysis",
      "Safety scalar analysis failed", 0);
}

int ir_optimize_safety_analysis(IRProgram *program, int preserve_boundaries) {
  if (!program) return 0;
  ir_optimize_set_program(program);
  ir_function_index_reset();
  ir_verify_begin_program(program);
  int ok = ir_run_program_stage_for_each_function(program,
                                                  ir_safety_analysis_function);
  if (ok && !preserve_boundaries &&
      !ir_pass_name_is_skipped("inline_small_functions")) {
    int changed = 0;
    ok = ir_inline_small_functions_pass(program, &changed);
  }
  if (ok) ok = ir_run_program_stage_for_each_function(program,
                                                      ir_safety_analysis_function);
  ir_verify_end_program();
  ir_function_index_reset();
  return ok;
}

int ir_optimize_program_pipeline(IRProgram *program,
                                 const IROptimizeOptions *options) {
  if (!program) {
    return 0;
  }
  ir_optimize_set_program(program);
  if (options && options->target_neutral_only) {
    return ir_optimize_portable_program_pipeline(program, options);
  }

  ir_optimize_reset_user_error();
  ir_optimize_set_simd_report(options && options->simd_report);
  ir_optimize_set_explain(options && options->explain,
                          options ? options->explain_focus_file : NULL);
  ir_function_index_reset();
  ir_verify_begin_program(program);
  /* hoist_global_bases declares a pointer local, which the backend resolves by
   * name; a program whose source never spelled one would have no such type. */
  ir_program_register_scalar_pointer_types(program);

  if (!ir_user_rewrite_begin(program)) {
    ir_verify_end_program();
    ir_function_index_reset();
    return 0;
  }

  /* Fold never-written global integer vars to their initializer constants
   * first, so every later pass (strength reduction, vectorizers, TRE) sees
   * plain constants instead of opaque global reads. */
  if (options && options->global_int_consts &&
      !ir_pass_name_is_skipped("fold_readonly_globals")) {
    int fold_changed = 0;
    mettle_compiler_ctx_set_pass_name("fold_readonly_globals");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_fold_readonly_globals_pass(program, options->global_int_consts,
                                       options->global_int_const_count,
                                       &fold_changed)) {
      mettle_compiler_ice("IR read-only global fold pass failed");
    }
    ir_pass_time_end("fold_readonly_globals [program]", t0);
  }

  {
    double t0 = ir_pass_time_begin();
    if (!ir_run_program_stage_for_each_function(
            program, ir_optimize_pre_inline_function)) {
      ir_function_index_reset();
      return 0;
    }
    ir_pass_time_end("pre_inline [stage]", t0);
  }

  /* Loop-invariant call hoisting, first run: BEFORE inlining. Inlining a
   * read-only callee into a loop body dissolves the single invariant-arg call
   * into residual calls whose arguments vary per iteration (a recursive
   * callee's self-calls, say), which no later pass can lift. Hoisting first
   * sees the call while its arguments are still the caller's loop-invariant
   * locals; the inliner then treats the hoisted preheader site like any other
   * call. The post-inline run below still catches calls a round of inlining
   * exposes. */
  if (!ir_pass_name_is_skipped("hoist_pure_calls")) {
    int pure_licm_changed = 0;
    mettle_compiler_ctx_set_pass_name("hoist_pure_calls");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_hoist_pure_calls_pass(program, &pure_licm_changed)) {
      mettle_compiler_ice("IR optimization pure-call hoisting pass failed");
    }
    ir_pass_time_end("hoist_pure_calls_pre_inline [program]", t0);
  }

  /* Tail-recursion elimination before any inlining: converting the tail
   * self call into a loop first means the regular inliner sees a loop-shaped
   * callee and the bounded self-recursion expander only has the remaining
   * non-tail calls to amortize. */
  if ((!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("tail_recursion_elim")) {
    int tre_changed = 0;
    mettle_compiler_ctx_set_pass_name("tail_recursion_elim");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_tail_recursion_elimination_pass(program, &tre_changed)) {
      mettle_compiler_ice("IR tail-recursion elimination pass failed");
    }
    ir_pass_time_end("tail_recursion_elim [program]", t0);
  }

  if ((!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("inline_small_functions")) {
    int inlining_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_small_functions");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_inline_small_functions_pass(program, &inlining_changed)) {
      mettle_compiler_ice("IR optimization inlining pass failed");
    }
    ir_pass_time_end("inline_small_functions [program]", t0);
  }

  /* Bounded recursive inlining: expand a recursive function's direct
   * self-call sites into copies of its own body (depth- and size-capped), so
   * each remaining real call amortizes prologue/epilogue and argument-passing
   * overhead across a subtree of the recursion. Runs after the regular
   * inliner so a self-recursive helper is first inlined into callers where
   * possible, then expanded in place. */
  if ((!options || !options->preserve_function_boundaries) &&
      !ir_pass_name_is_skipped("inline_self_recursion")) {
    int self_inline_changed = 0;
    mettle_compiler_ctx_set_pass_name("inline_self_recursion");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_inline_self_recursion_pass(program, &self_inline_changed)) {
      mettle_compiler_ice("IR optimization self-recursion inlining failed");
    }
    ir_pass_time_end("inline_self_recursion [program]", t0);
  }

  /* `@pure` loop-invariant call hoisting. Program-level (resolves callees by
   * name) and run after inlining so an inlined pure body is hoisted as ordinary
   * loop-invariant code; the per-function fixpoint below then cleans up. */
  if (!ir_pass_name_is_skipped("hoist_pure_calls")) {
    int pure_licm_changed = 0;
    mettle_compiler_ctx_set_pass_name("hoist_pure_calls");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_hoist_pure_calls_pass(program, &pure_licm_changed)) {
      mettle_compiler_ice("IR optimization pure-call hoisting pass failed");
    }
    ir_pass_time_end("hoist_pure_calls [program]", t0);
  }

  /* Allocation-site layout factorization: re-map provably-private malloc
   * pools (compact padded strides / factor into per-field SoA arrays).
   * Whole-program only: rewriting a callee body to a new pool layout is
   * sound only when every call site is visible. Runs after inlining so
   * field-accessor helpers are already folded into their callers, and
   * before the per-function stage so the vectorizers see the rewritten
   * unit-stride form. NOT under per-function --verify: the transform
   * preserves program behavior but changes the buffer's byte image, which
   * the per-function validator counts as an observation (and a coordinated
   * multi-function rewrite must never be quarantined one function at a
   * time). METTLE_SKIP_PASS=layout_factor disables it. */
  if (options && options->whole_program &&
      !options->preserve_function_boundaries &&
      !ir_pass_name_is_skipped("layout_factor")) {
    int layout_changed = 0;
    mettle_compiler_ctx_set_pass_name("layout_factor");
    mettle_compiler_ctx_set_fixpoint_iteration(0);
    double t0 = ir_pass_time_begin();
    if (!ir_layout_factor_pass(program, &layout_changed)) {
      mettle_compiler_ice("IR layout factorization pass failed");
    }
    ir_pass_time_end("layout_factor [program]", t0);
  }

  /* Whole-program alias facts, built once the call graph has settled: the
   * memory passes in the per-function stage ask which parameters can reach
   * the same allocation, and the answer comes from every call site at once.
   * After inlining, because an inlined body's arguments are the caller's own
   * values and no longer a question about parameters. */
  if (!ir_pass_name_is_skipped("alias_facts")) {
    double t0 = ir_pass_time_begin();
    ir_alias_facts_build(program);
    ir_pass_time_end("alias_facts [program]", t0);
  }

  /* Give the per-function contract verifier program access for the duration
   * of the stage: the call-in-body fix simulation re-runs the inliner on a
   * caller clone, which needs callee lookup. */
  ir_explain_set_program(program);
  if (!ir_run_program_stage_for_each_function(
          program, ir_optimize_function_pipeline)) {
    ir_explain_set_program(NULL);
    ir_alias_facts_reset();
    /* A violated `@simd!` contract already printed a user diagnostic; don't
     * dress it up as an internal compiler error. */
    if (!ir_optimize_had_user_error()) {
      mettle_compiler_ice_report("IR optimization failed", NULL);
    }
    ir_function_index_reset();
    return 0;
  }
  ir_explain_set_program(NULL);
  /* The facts index functions by pointer; nothing may consult them once the
   * program can be rewritten or freed. */
  ir_alias_facts_reset();

  /* Function-level contracts, now that every optimization that could satisfy
   * them has run. `@inline!` is skipped when function boundaries are pinned
   * (--profile-runtime disables inlining entirely; failing every contract
   * there would be noise, not information). Check both before deciding the
   * outcome so a build with several violations reports them all. */
  int contracts_ok = 1;
  if (!options || !options->preserve_function_boundaries) {
    contracts_ok &= ir_inline_enforce_contracts(program);
  }
  contracts_ok &= ir_enforce_noalloc_contracts(program);
  contracts_ok &= ir_user_rewrite_end(program);

  /* --explain: every inline that happened was recorded as it happened; record
   * each surviving call with the reason it was refused, then print the whole
   * sorted report. (No-ops unless explain is enabled.) */
  ir_inline_explain_report_remaining(program);
  ir_explain_flush();
  if (!contracts_ok) {
    /* Compilation stops before codegen, so the backend flush (the normal
     * report-routing point) never runs: print the buffered report now. */
    ir_explain_finalize(1);
  }
  ir_pass_time_report();
  ir_verify_end_program();

  ir_function_index_reset();
  return contracts_ok;
}
