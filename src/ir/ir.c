#include "ir.h"
#include "../common.h"
#include "../string_intern.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define IR_OPERAND_FMT_BUFSIZE 128

/* Single seam for allocating an IR operand name / instruction text copy, and
 * the pair of mettle_free_string on every destroy path (which also tolerates
 * interned/static strings). Interning these names was tried and measured
 * SLOWER on 500k-LOC fixtures: temp/label names are unique, so the pool paid
 * a hash, an entry allocation, and periodic whole-table rehashes per name
 * while clones almost never hit. Plain owned copies win; keep the seam so a
 * future allocator change is one line. */
static char *ir_intern_name(const char *name) {
  return mettle_strdup(name);
}

typedef struct {
  const char *name;
  MtlcIntrinsic intrinsic;
  int arity;
} IRIntrinsicName;

static const IRIntrinsicName g_ir_intrinsics[] = {
    {"gpu_tid_x", MTLC_INTRINSIC_GPU_LOCAL_ID_X, 0},
    {"gpu_tid_y", MTLC_INTRINSIC_GPU_LOCAL_ID_Y, 0},
    {"gpu_tid_z", MTLC_INTRINSIC_GPU_LOCAL_ID_Z, 0},
    {"gpu_ntid_x", MTLC_INTRINSIC_GPU_LOCAL_SIZE_X, 0},
    {"gpu_ntid_y", MTLC_INTRINSIC_GPU_LOCAL_SIZE_Y, 0},
    {"gpu_ntid_z", MTLC_INTRINSIC_GPU_LOCAL_SIZE_Z, 0},
    {"gpu_ctaid_x", MTLC_INTRINSIC_GPU_GROUP_ID_X, 0},
    {"gpu_ctaid_y", MTLC_INTRINSIC_GPU_GROUP_ID_Y, 0},
    {"gpu_ctaid_z", MTLC_INTRINSIC_GPU_GROUP_ID_Z, 0},
    {"gpu_nctaid_x", MTLC_INTRINSIC_GPU_NUM_GROUPS_X, 0},
    {"gpu_nctaid_y", MTLC_INTRINSIC_GPU_NUM_GROUPS_Y, 0},
    {"gpu_nctaid_z", MTLC_INTRINSIC_GPU_NUM_GROUPS_Z, 0},
    {"subgroup_local_id", MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID, 0},
    {"subgroup_size", MTLC_INTRINSIC_GPU_SUBGROUP_SIZE, 0},
    {"subgroup_broadcast_u32", MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32, 2},
    {"subgroup_broadcast_f32", MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32, 2},
    {"subgroup_reduce_add_u32", MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32, 1},
    {"subgroup_reduce_add_f32", MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32, 1},
    {"subgroup_reduce_min_u32", MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32, 1},
    {"subgroup_reduce_min_f32", MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32, 1},
    {"subgroup_reduce_max_u32", MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32, 1},
    {"subgroup_reduce_max_f32", MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32, 1},
    {"subgroup_scan_inclusive_add_u32",
     MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32, 1},
    {"subgroup_scan_inclusive_add_f32",
     MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32, 1},
    {"subgroup_scan_exclusive_add_u32",
     MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32, 1},
    {"subgroup_scan_exclusive_add_f32",
     MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32, 1},
    {"subgroup_shuffle_u32", MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32, 2},
    {"subgroup_shuffle_f32", MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32, 2},
    {"subgroup_ballot_word", MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD, 2},
    {"subgroup_any", MTLC_INTRINSIC_GPU_SUBGROUP_ANY, 1},
    {"subgroup_all", MTLC_INTRINSIC_GPU_SUBGROUP_ALL, 1},
    {"gpu_barrier", MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER, 0},
    {"sqrtf", MTLC_INTRINSIC_GPU_SQRT_F32, 1},
    {"rsqrtf", MTLC_INTRINSIC_GPU_RSQRT_F32, 1},
    {"fabsf", MTLC_INTRINSIC_GPU_ABS_F32, 1},
    {"sinf", MTLC_INTRINSIC_GPU_SIN_F32, 1},
    {"cosf", MTLC_INTRINSIC_GPU_COS_F32, 1},
    {"logf", MTLC_INTRINSIC_GPU_LOG_F32, 1},
    {"expf", MTLC_INTRINSIC_GPU_EXP_F32, 1},
    {"h2f", MTLC_INTRINSIC_GPU_F16_BITS_TO_F32, 1},
    {"f2h", MTLC_INTRINSIC_GPU_F32_TO_F16_BITS, 1},
    {"f32_from_bits", MTLC_INTRINSIC_GPU_F32_FROM_BITS, 1},
    {"bits_from_f32", MTLC_INTRINSIC_GPU_F32_TO_BITS, 1},
    {"dp4a_u32", MTLC_INTRINSIC_GPU_DP4A_U32, 3},
    {"dp4a_s32", MTLC_INTRINSIC_GPU_DP4A_S32, 3},
    {"dp2a_lo_u32", MTLC_INTRINSIC_GPU_DP2A_LO_U32, 3},
    {"dp2a_lo_s32", MTLC_INTRINSIC_GPU_DP2A_LO_S32, 3},
    {"dp2a_hi_u32", MTLC_INTRINSIC_GPU_DP2A_HI_U32, 3},
    {"dp2a_hi_s32", MTLC_INTRINSIC_GPU_DP2A_HI_S32, 3},
    {"prmt_b32", MTLC_INTRINSIC_GPU_PRMT_B32, 3},
    {"hadd2", MTLC_INTRINSIC_GPU_HADD2, 2},
    {"hmul2", MTLC_INTRINSIC_GPU_HMUL2, 2},
    {"hfma2", MTLC_INTRINSIC_GPU_HFMA2, 3},
    {"h2f_lo", MTLC_INTRINSIC_GPU_H2F_LO, 1},
    {"h2f_hi", MTLC_INTRINSIC_GPU_H2F_HI, 1},
    {"f2h2", MTLC_INTRINSIC_GPU_F2H2, 2},
    {"bf2f", MTLC_INTRINSIC_GPU_BF2F, 1},
    {"f2bf", MTLC_INTRINSIC_GPU_F2BF, 1},
    {"gpu_print", MTLC_INTRINSIC_GPU_PRINT, 1},
    {"gpu_print_i32", MTLC_INTRINSIC_GPU_PRINT_I32, 2},
    {"gpu_print_f32", MTLC_INTRINSIC_GPU_PRINT_F32, 2},
    {"gpu_print_2i32", MTLC_INTRINSIC_GPU_PRINT_2I32, 3},
    {"gpu_assert", MTLC_INTRINSIC_GPU_ASSERT, 1},
    {"load4_f32", MTLC_INTRINSIC_GPU_LOAD4_F32, 2},
    {"load4_u32", MTLC_INTRINSIC_GPU_LOAD4_U32, 2},
    {"store4_f32", MTLC_INTRINSIC_GPU_STORE4_F32, 2},
    {"store4_u32", MTLC_INTRINSIC_GPU_STORE4_U32, 2},
    {"atomic_min_u32", MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32, 3},
    {"atomic_min_u64", MTLC_INTRINSIC_GPU_ATOMIC_MIN_U64, 3},
    {"atomic_add_u32", MTLC_INTRINSIC_GPU_ATOMIC_ADD_U32, 3},
    {"atomic_add_u64", MTLC_INTRINSIC_GPU_ATOMIC_ADD_U64, 3},
    {"atomic_sub_u32", MTLC_INTRINSIC_GPU_ATOMIC_SUB_U32, 3},
    {"atomic_sub_u64", MTLC_INTRINSIC_GPU_ATOMIC_SUB_U64, 3},
    {"atomic_max_u32", MTLC_INTRINSIC_GPU_ATOMIC_MAX_U32, 3},
    {"atomic_max_u64", MTLC_INTRINSIC_GPU_ATOMIC_MAX_U64, 3},
    {"atomic_and_u32", MTLC_INTRINSIC_GPU_ATOMIC_AND_U32, 3},
    {"atomic_and_u64", MTLC_INTRINSIC_GPU_ATOMIC_AND_U64, 3},
    {"atomic_or_u32", MTLC_INTRINSIC_GPU_ATOMIC_OR_U32, 3},
    {"atomic_or_u64", MTLC_INTRINSIC_GPU_ATOMIC_OR_U64, 3},
    {"atomic_xor_u32", MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32, 3},
    {"atomic_xor_u64", MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64, 3},
    {"atomic_exchange_u32", MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U32, 3},
    {"atomic_exchange_u64", MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U64, 3},
    {"atomic_compare_exchange_u32",
     MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U32, 4},
    {"atomic_compare_exchange_u64",
     MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U64, 4},
    {"atomic_load_u32", MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U32, 2},
    {"atomic_load_u64", MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U64, 2},
    {"atomic_store_u32", MTLC_INTRINSIC_GPU_ATOMIC_STORE_U32, 3},
    {"atomic_store_u64", MTLC_INTRINSIC_GPU_ATOMIC_STORE_U64, 3},
};

MtlcIntrinsic ir_intrinsic_from_name(const char *name) {
  if (!name) {
    return MTLC_INTRINSIC_NONE;
  }
  for (size_t i = 0; i < sizeof(g_ir_intrinsics) / sizeof(g_ir_intrinsics[0]);
       i++) {
    if (strcmp(name, g_ir_intrinsics[i].name) == 0) {
      return g_ir_intrinsics[i].intrinsic;
    }
  }
  return MTLC_INTRINSIC_NONE;
}

const char *ir_gpu_only_construct_name(IROpcode op) {
  switch (op) {
  case IR_OP_ADDRESS_SPACE_ALLOC:
    return "workgroup/private device storage";
  case IR_OP_BARRIER:
    return "barrier";
  case IR_OP_ASYNC_COPY:
    return "async_copy";
  case IR_OP_ASYNC_COMMIT:
    return "async_commit";
  case IR_OP_ASYNC_WAIT:
    return "async_wait";
  case IR_OP_TENSOR_TRANSFER:
    return "tensor_transfer";
  case IR_OP_TENSOR_MMA:
    return "tensor_mma";
  case IR_OP_TENSOR_MATMUL:
    return "tensor_matmul";
  case IR_OP_TENSOR_EPILOGUE:
    return "tensor_epilogue";
  case IR_OP_TENSOR_COMMIT:
    return "tensor_commit";
  default:
    return NULL;
  }
}

const char *ir_intrinsic_name(MtlcIntrinsic intrinsic) {
  for (size_t i = 0; i < sizeof(g_ir_intrinsics) / sizeof(g_ir_intrinsics[0]);
       i++) {
    if (intrinsic == g_ir_intrinsics[i].intrinsic) {
      return g_ir_intrinsics[i].name;
    }
  }
  return NULL;
}

int ir_intrinsic_arity(MtlcIntrinsic intrinsic) {
  for (size_t i = 0; i < sizeof(g_ir_intrinsics) / sizeof(g_ir_intrinsics[0]);
       i++) {
    if (intrinsic == g_ir_intrinsics[i].intrinsic) {
      return g_ir_intrinsics[i].arity;
    }
  }
  return -1;
}

int ir_intrinsic_is_atomic(MtlcIntrinsic intrinsic) {
  return intrinsic >= MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32 &&
         intrinsic <= MTLC_INTRINSIC_GPU_ATOMIC_STORE_U64;
}

int ir_intrinsic_is_compare_exchange(MtlcIntrinsic intrinsic) {
  return intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U32 ||
         intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U64;
}

int ir_intrinsic_is_atomic_load(MtlcIntrinsic intrinsic) {
  return intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U32 ||
         intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U64;
}

int ir_intrinsic_is_atomic_store(MtlcIntrinsic intrinsic) {
  return intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_STORE_U32 ||
         intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_STORE_U64;
}

MtlcTypeKind ir_intrinsic_atomic_value_kind(MtlcIntrinsic intrinsic) {
  if (!ir_intrinsic_is_atomic(intrinsic)) return MTLC_TYPE_VOID;
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_ATOMIC_MIN_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_ADD_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_SUB_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_MAX_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_AND_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_OR_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U64:
  case MTLC_INTRINSIC_GPU_ATOMIC_STORE_U64:
    return MTLC_TYPE_UINT64;
  default:
    return MTLC_TYPE_UINT32;
  }
}

MtlcTypeKind ir_intrinsic_atomic_result_kind(MtlcIntrinsic intrinsic) {
  if (!ir_intrinsic_is_atomic(intrinsic) ||
      ir_intrinsic_is_atomic_store(intrinsic))
    return MTLC_TYPE_VOID;
  return ir_intrinsic_atomic_value_kind(intrinsic);
}

int ir_intrinsic_is_subgroup(MtlcIntrinsic intrinsic) {
  return intrinsic >= MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID &&
         intrinsic <= MTLC_INTRINSIC_GPU_SUBGROUP_ALL;
}

MtlcTypeKind ir_intrinsic_subgroup_result_kind(MtlcIntrinsic intrinsic) {
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SIZE:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD:
    return MTLC_TYPE_UINT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32:
    return MTLC_TYPE_FLOAT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_ANY:
  case MTLC_INTRINSIC_GPU_SUBGROUP_ALL:
    return MTLC_TYPE_BOOL;
  default:
    return MTLC_TYPE_VOID;
  }
}

MtlcTypeKind ir_tensor_element_storage_kind(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return MTLC_TYPE_UINT16;
  case MTLC_TENSOR_ELEMENT_TFLOAT32:
  case MTLC_TENSOR_ELEMENT_FLOAT32:
    return MTLC_TYPE_FLOAT32;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return MTLC_TYPE_FLOAT64;
  case MTLC_TENSOR_ELEMENT_INT8:
    return MTLC_TYPE_INT8;
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
  case MTLC_TENSOR_ELEMENT_SCALE_UE8M0:
  case MTLC_TENSOR_ELEMENT_SCALE_UE4M3:
  case MTLC_TENSOR_ELEMENT_UINT8:
  case MTLC_TENSOR_ELEMENT_INT4:
  case MTLC_TENSOR_ELEMENT_UINT4:
  case MTLC_TENSOR_ELEMENT_BIT1:
    return MTLC_TYPE_UINT8;
  case MTLC_TENSOR_ELEMENT_INT32:
    return MTLC_TYPE_INT32;
  default:
    return MTLC_TYPE_VOID;
  }
}

static int ir_tensor_layout_valid(MtlcTensorLayout layout) {
  return layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ||
         layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
}

static uint32_t ir_tensor_leading_min(uint32_t rows, uint32_t columns,
                                      MtlcTensorLayout layout) {
  return layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? columns : rows;
}

static int ir_tensor_data_element_valid(MtlcTensorElement element) {
  return ir_tensor_element_storage_kind(element) != MTLC_TYPE_VOID &&
         element != MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
         element != MTLC_TENSOR_ELEMENT_SCALE_UE4M3;
}

static int ir_tensor_subbyte_element(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
  case MTLC_TENSOR_ELEMENT_INT4:
  case MTLC_TENSOR_ELEMENT_UINT4:
  case MTLC_TENSOR_ELEMENT_BIT1: return 1;
  default: return 0;
  }
}

size_t ir_tensor_transfer_element_bytes(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_TFLOAT32:
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return 8;
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
  case MTLC_TENSOR_ELEMENT_SCALE_UE8M0:
  case MTLC_TENSOR_ELEMENT_SCALE_UE4M3:
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
  case MTLC_TENSOR_ELEMENT_INT4:
  case MTLC_TENSOR_ELEMENT_UINT4:
  case MTLC_TENSOR_ELEMENT_BIT1:
    return 1;
  default:
    return 0;
  }
}

size_t ir_tensor_transfer_tile_elements(const MtlcTensorTransferDesc *desc) {
  size_t elements = 1;
  if (!desc || desc->rank == 0 || desc->rank > MTLC_TENSOR_MAX_RANK)
    return 0;
  for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
    uint32_t extent = desc->tile_extent[dimension];
    if (extent == 0 || elements > SIZE_MAX / extent) return 0;
    elements *= extent;
  }
  return elements;
}

int mtlc_tensor_transfer_desc_is_valid(const MtlcTensorTransferDesc *desc) {
  size_t element_bytes = desc ? ir_tensor_transfer_element_bytes(desc->element)
                              : 0;
  size_t tile_elements = ir_tensor_transfer_tile_elements(desc);
  uint64_t maximum_offset = 0;
  if (!desc || desc->rank == 0 || desc->rank > MTLC_TENSOR_MAX_RANK ||
      desc->direction < MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP ||
      desc->direction > MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL ||
      element_bytes == 0 || desc->packing != MTLC_TENSOR_PACKING_LOGICAL ||
      desc->bounds != MTLC_TENSOR_BOUNDS_ZERO ||
      desc->scope != MTLC_MEMORY_SCOPE_WORKGROUP || tile_elements == 0 ||
      tile_elements > 65536u / element_bytes) {
    return 0;
  }
  for (uint8_t dimension = 0; dimension < MTLC_TENSOR_MAX_RANK;
       dimension++) {
    if (dimension >= desc->rank) {
      if (desc->global_extent[dimension] ||
          desc->global_stride_bytes[dimension] ||
          desc->tile_extent[dimension] || desc->element_stride[dimension])
        return 0;
      continue;
    }
    uint64_t extent = desc->global_extent[dimension];
    uint64_t stride = desc->global_stride_bytes[dimension];
    if (extent == 0 || stride < element_bytes ||
        stride % element_bytes != 0 || desc->tile_extent[dimension] == 0 ||
        desc->element_stride[dimension] == 0 ||
        (extent - 1u) > (UINT64_MAX - maximum_offset) / stride) {
      return 0;
    }
    maximum_offset += (extent - 1u) * stride;
  }
  return maximum_offset <= UINT64_MAX - element_bytes;
}

int ir_tensor_transfer_desc_valid(const MtlcTensorTransferDesc *desc) {
  return mtlc_tensor_transfer_desc_is_valid(desc);
}

size_t ir_tensor_transfer_operand_count(const MtlcTensorTransferDesc *desc,
                                        int has_prepared_view) {
  if (!ir_tensor_transfer_desc_valid(desc) ||
      (has_prepared_view != 0 && has_prepared_view != 1))
    return 0;
  return 2u + (has_prepared_view ? 1u : 0u) + desc->rank;
}

static int ir_tensor_scale_valid(MtlcTensorScaleMode mode,
                                 MtlcTensorElement element,
                                 uint32_t leading_dimension, uint32_t k) {
  if (mode == MTLC_TENSOR_SCALE_NONE)
    return element == MTLC_TENSOR_ELEMENT_INVALID && leading_dimension == 0;
  if (element != MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
      element != MTLC_TENSOR_ELEMENT_SCALE_UE4M3)
    return 0;
  uint32_t columns =
      mode == MTLC_TENSOR_SCALE_PER_TENSOR
          ? 1u
          : (k + (mode == MTLC_TENSOR_SCALE_BLOCK_16 ? 15u : 31u)) /
                (mode == MTLC_TENSOR_SCALE_BLOCK_16 ? 16u : 32u);
  return leading_dimension == 0 || leading_dimension >= columns;
}

static uint32_t ir_tensor_sparse_group_size(MtlcTensorSparsity sparsity) {
  switch (sparsity) {
  case MTLC_TENSOR_SPARSITY_STRUCTURED_1_TO_2:
    return 2;
  case MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4:
    return 4;
  case MTLC_TENSOR_SPARSITY_STRUCTURED_4_TO_8:
    return 8;
  default:
    return 0;
  }
}

int mtlc_tensor_mma_desc_is_valid(const MtlcTensorMmaDesc *desc) {
  uint32_t a_rows, a_columns, b_rows, b_columns, a_storage_k;
  if (!desc || desc->m == 0 || desc->n == 0 || desc->k == 0 ||
      desc->math_mode < MTLC_TENSOR_MATH_MULTIPLY_ADD ||
      desc->math_mode > MTLC_TENSOR_MATH_AND_POPCOUNT ||
      desc->sparsity < MTLC_TENSOR_SPARSITY_DENSE ||
      desc->sparsity > MTLC_TENSOR_SPARSITY_STRUCTURED_4_TO_8 ||
      !ir_tensor_data_element_valid(desc->a_element) ||
      !ir_tensor_data_element_valid(desc->b_element) ||
      !ir_tensor_data_element_valid(desc->accumulator_element) ||
      !ir_tensor_data_element_valid(desc->result_element) ||
      !ir_tensor_layout_valid(desc->a_layout) ||
      !ir_tensor_layout_valid(desc->b_layout) ||
      !ir_tensor_layout_valid(desc->c_layout) ||
      !ir_tensor_layout_valid(desc->d_layout) ||
      desc->rounding < MTLC_TENSOR_ROUND_DEFAULT ||
      desc->rounding > MTLC_TENSOR_ROUND_UP ||
      desc->overflow < MTLC_TENSOR_OVERFLOW_WRAP ||
      desc->overflow > MTLC_TENSOR_OVERFLOW_SATURATE_FINITE ||
      desc->a_scale_mode < MTLC_TENSOR_SCALE_NONE ||
      desc->a_scale_mode > MTLC_TENSOR_SCALE_BLOCK_32 ||
      desc->b_scale_mode < MTLC_TENSOR_SCALE_NONE ||
      desc->b_scale_mode > MTLC_TENSOR_SCALE_BLOCK_32 ||
      desc->a_packing < MTLC_TENSOR_PACKING_LOGICAL ||
      desc->a_packing > MTLC_TENSOR_PACKING_DENSE_SUBBYTE ||
      desc->b_packing < MTLC_TENSOR_PACKING_LOGICAL ||
      desc->b_packing > MTLC_TENSOR_PACKING_DENSE_SUBBYTE ||
      desc->transpose_a > 1 || desc->transpose_b > 1 ||
      (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP &&
       desc->scope != MTLC_MEMORY_SCOPE_WORKGROUP)) {
    return 0;
  }
  if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD &&
      (desc->a_element != MTLC_TENSOR_ELEMENT_BIT1 ||
       desc->b_element != MTLC_TENSOR_ELEMENT_BIT1 ||
       desc->accumulator_element != MTLC_TENSOR_ELEMENT_INT32 ||
       desc->result_element != MTLC_TENSOR_ELEMENT_INT32)) {
    return 0;
  }
  if ((desc->a_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
       !ir_tensor_subbyte_element(desc->a_element)) ||
      (desc->b_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
       !ir_tensor_subbyte_element(desc->b_element)) ||
      !ir_tensor_scale_valid(desc->a_scale_mode, desc->a_scale_element,
                             desc->a_scale_leading_dimension, desc->k) ||
      !ir_tensor_scale_valid(desc->b_scale_mode, desc->b_scale_element,
                             desc->b_scale_leading_dimension, desc->k)) {
    return 0;
  }
  a_storage_k = desc->k;
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
    uint32_t group = ir_tensor_sparse_group_size(desc->sparsity);
    if (!group || desc->k % group != 0) return 0;
    a_storage_k = desc->k / 2u;
  }
  a_rows = desc->transpose_a ? a_storage_k : desc->m;
  a_columns = desc->transpose_a ? desc->m : a_storage_k;
  b_rows = desc->transpose_b ? desc->n : desc->k;
  b_columns = desc->transpose_b ? desc->k : desc->n;
  return (desc->a_leading_dimension == 0 ||
          desc->a_leading_dimension >=
              ir_tensor_leading_min(a_rows, a_columns, desc->a_layout)) &&
         (desc->b_leading_dimension == 0 ||
          desc->b_leading_dimension >=
              ir_tensor_leading_min(b_rows, b_columns, desc->b_layout)) &&
         (desc->c_leading_dimension == 0 ||
          desc->c_leading_dimension >=
              ir_tensor_leading_min(desc->m, desc->n, desc->c_layout)) &&
         (desc->d_leading_dimension == 0 ||
          desc->d_leading_dimension >=
              ir_tensor_leading_min(desc->m, desc->n, desc->d_layout));
}

int ir_tensor_mma_desc_valid(const MtlcTensorMmaDesc *desc) {
  return mtlc_tensor_mma_desc_is_valid(desc);
}

int mtlc_tensor_epilogue_desc_is_valid(
    const MtlcTensorEpilogueDesc *desc) {
  if (!desc || desc->m == 0 || desc->n == 0 ||
      (desc->element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
       desc->element != MTLC_TENSOR_ELEMENT_BFLOAT16 &&
       desc->element != MTLC_TENSOR_ELEMENT_FLOAT32 &&
       desc->element != MTLC_TENSOR_ELEMENT_FLOAT64) ||
      !ir_tensor_layout_valid(desc->layout) ||
      (desc->leading_dimension != 0 &&
       desc->leading_dimension <
           ir_tensor_leading_min(desc->m, desc->n, desc->layout)) ||
      desc->bias_mode < MTLC_TENSOR_BIAS_NONE ||
      desc->bias_mode > MTLC_TENSOR_BIAS_MATRIX ||
      desc->activation < MTLC_TENSOR_ACTIVATION_IDENTITY ||
      desc->activation > MTLC_TENSOR_ACTIVATION_CLAMP ||
      desc->scale_output > 1 || desc->scale_bias > 1 ||
      (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP &&
       desc->scope != MTLC_MEMORY_SCOPE_WORKGROUP)) {
    return 0;
  }

  switch (desc->bias_mode) {
  case MTLC_TENSOR_BIAS_NONE:
    return desc->bias_layout == MTLC_TENSOR_LAYOUT_INVALID &&
           desc->bias_leading_dimension == 0 && !desc->scale_bias;
  case MTLC_TENSOR_BIAS_PER_ROW:
  case MTLC_TENSOR_BIAS_PER_COLUMN:
    return desc->bias_layout == MTLC_TENSOR_LAYOUT_INVALID &&
           desc->bias_leading_dimension == 0;
  case MTLC_TENSOR_BIAS_MATRIX:
    return ir_tensor_layout_valid(desc->bias_layout) &&
           (desc->bias_leading_dimension == 0 ||
            desc->bias_leading_dimension >= ir_tensor_leading_min(
                desc->m, desc->n, desc->bias_layout));
  default:
    return 0;
  }
}

int ir_tensor_epilogue_desc_valid(const MtlcTensorEpilogueDesc *desc) {
  return mtlc_tensor_epilogue_desc_is_valid(desc);
}

size_t ir_tensor_epilogue_operand_count(
    const MtlcTensorEpilogueDesc *desc) {
  if (!ir_tensor_epilogue_desc_valid(desc)) return 0;
  return 1u + (desc->bias_mode != MTLC_TENSOR_BIAS_NONE ? 1u : 0u) +
         (desc->scale_output ? 1u : 0u) +
         (desc->scale_bias ? 1u : 0u) +
         (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP ? 2u : 0u) +
         (desc->leading_dimension == 0 ? 1u : 0u) +
         (desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX &&
                  desc->bias_leading_dimension == 0
              ? 1u
              : 0u);
}

size_t ir_tensor_mma_operand_count(const MtlcTensorMmaDesc *desc) {
  if (!ir_tensor_mma_desc_valid(desc)) return 0;
  return 4u + (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
         (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
         (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
         (desc->a_leading_dimension == 0 ? 1u : 0u) +
         (desc->b_leading_dimension == 0 ? 1u : 0u) +
         (desc->c_leading_dimension == 0 ? 1u : 0u) +
         (desc->d_leading_dimension == 0 ? 1u : 0u);
}

size_t ir_tensor_matmul_operand_count(const MtlcTensorMmaDesc *desc) {
  size_t mma_operands = ir_tensor_mma_operand_count(desc);
  return mma_operands && mma_operands <= SIZE_MAX - 5u
             ? mma_operands + 5u
             : 0u;
}

unsigned ir_tensor_mma_runtime_stride_mask(const MtlcTensorMmaDesc *desc) {
  unsigned mask = MTLC_TENSOR_RUNTIME_STRIDE_NONE;
  if (!desc) return mask;
  if (desc->a_leading_dimension == 0) mask |= MTLC_TENSOR_RUNTIME_STRIDE_A;
  if (desc->b_leading_dimension == 0) mask |= MTLC_TENSOR_RUNTIME_STRIDE_B;
  if (desc->c_leading_dimension == 0) mask |= MTLC_TENSOR_RUNTIME_STRIDE_C;
  if (desc->d_leading_dimension == 0) mask |= MTLC_TENSOR_RUNTIME_STRIDE_D;
  return mask;
}

size_t ir_tensor_mma_instruction_count(const IRInstruction *instruction) {
  return instruction && instruction->tensor_mma_count > 1
             ? (size_t)instruction->tensor_mma_count
             : 1u;
}

int ir_tensor_mma_desc_equal(const MtlcTensorMmaDesc *a,
                             const MtlcTensorMmaDesc *b) {
  return a && b && a->m == b->m && a->n == b->n && a->k == b->k &&
         a->math_mode == b->math_mode && a->sparsity == b->sparsity &&
         a->a_element == b->a_element && a->b_element == b->b_element &&
         a->accumulator_element == b->accumulator_element &&
         a->result_element == b->result_element &&
         a->a_layout == b->a_layout && a->b_layout == b->b_layout &&
         a->c_layout == b->c_layout && a->d_layout == b->d_layout &&
         a->a_leading_dimension == b->a_leading_dimension &&
         a->b_leading_dimension == b->b_leading_dimension &&
         a->c_leading_dimension == b->c_leading_dimension &&
         a->d_leading_dimension == b->d_leading_dimension &&
         a->rounding == b->rounding && a->overflow == b->overflow &&
         a->a_scale_mode == b->a_scale_mode &&
         a->b_scale_mode == b->b_scale_mode &&
         a->a_scale_element == b->a_scale_element &&
         a->b_scale_element == b->b_scale_element &&
         a->a_packing == b->a_packing && a->b_packing == b->b_packing &&
         a->a_scale_leading_dimension == b->a_scale_leading_dimension &&
         a->b_scale_leading_dimension == b->b_scale_leading_dimension &&
         a->transpose_a == b->transpose_a &&
         a->transpose_b == b->transpose_b && a->scope == b->scope;
}

int ir_operand_same(const IROperand *a, const IROperand *b) {
  if (!a || !b || a->kind != b->kind || a->int_value != b->int_value ||
      a->float_bits != b->float_bits)
    return 0;
  if (a->kind == IR_OPERAND_FLOAT && a->float_value != b->float_value)
    return 0;
  if ((a->name == NULL) != (b->name == NULL)) return 0;
  return !a->name || strcmp(a->name, b->name) == 0;
}

static const char *ir_address_space_name(MtlcAddressSpace value) {
  switch (value) {
  case MTLC_ADDRESS_SPACE_DEFAULT: return "default";
  case MTLC_ADDRESS_SPACE_GENERIC: return "generic";
  case MTLC_ADDRESS_SPACE_GLOBAL: return "global";
  case MTLC_ADDRESS_SPACE_WORKGROUP: return "workgroup";
  case MTLC_ADDRESS_SPACE_CONSTANT: return "constant";
  case MTLC_ADDRESS_SPACE_PRIVATE: return "private";
  }
  return "invalid";
}

static const char *ir_memory_order_name(MtlcMemoryOrder value) {
  switch (value) {
  case MTLC_MEMORY_ORDER_DEFAULT: return "default";
  case MTLC_MEMORY_ORDER_RELAXED: return "relaxed";
  case MTLC_MEMORY_ORDER_ACQUIRE: return "acquire";
  case MTLC_MEMORY_ORDER_RELEASE: return "release";
  case MTLC_MEMORY_ORDER_ACQ_REL: return "acq_rel";
  case MTLC_MEMORY_ORDER_SEQ_CST: return "seq_cst";
  }
  return "invalid";
}

static const char *ir_memory_scope_name(MtlcMemoryScope value) {
  switch (value) {
  case MTLC_MEMORY_SCOPE_DEFAULT: return "default";
  case MTLC_MEMORY_SCOPE_WORK_ITEM: return "work_item";
  case MTLC_MEMORY_SCOPE_SUBGROUP: return "subgroup";
  case MTLC_MEMORY_SCOPE_WORKGROUP: return "workgroup";
  case MTLC_MEMORY_SCOPE_DEVICE: return "device";
  case MTLC_MEMORY_SCOPE_SYSTEM: return "system";
  }
  return "invalid";
}

IROperand ir_operand_none(void) {
  IROperand operand = {0};
  operand.kind = IR_OPERAND_NONE;
  return operand;
}

IROperand ir_operand_temp(const char *name) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_TEMP;
  operand.name = ir_intern_name(name);
  return operand;
}

IROperand ir_operand_symbol(const char *name) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_SYMBOL;
  operand.name = ir_intern_name(name);
  return operand;
}

IROperand ir_operand_int(long long value) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_INT;
  operand.int_value = value;
  return operand;
}

IROperand ir_operand_float(double value) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_FLOAT;
  operand.float_value = value;
  operand.float_bits = 64;
  return operand;
}

IROperand ir_operand_float_sized(double value, int float_bits) {
  IROperand operand = ir_operand_float(value);
  operand.float_bits = (float_bits == 32) ? 32 : 64;
  return operand;
}

char *ir_copy_literal_bytes(const char *value, size_t length) {
  char *copy = NULL;
  if (!value) {
    return NULL;
  }
  copy = malloc(length + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, value, length);
  copy[length] = '\0';
  return copy;
}

IROperand ir_operand_string(const char *value) {
  return ir_operand_string_n(value, value ? strlen(value) : 0);
}

/* `value` may hold interior NULs, so the copy is by length and the terminator
 * is added on top: readers that only want a name keep working, and the ones
 * that build a {chars, length} record get every byte. */
IROperand ir_operand_string_n(const char *value, size_t length) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_STRING;
  operand.name = ir_copy_literal_bytes(value, length);
  operand.int_value = (long long)length;
  return operand;
}

size_t ir_operand_string_length(const IROperand *operand) {
  if (!operand || !operand->name) {
    return 0;
  }
  if (operand->kind == IR_OPERAND_STRING && operand->int_value > 0) {
    return (size_t)operand->int_value;
  }
  return strlen(operand->name);
}

IROperand ir_operand_label(const char *name) {
  IROperand operand = ir_operand_none();
  operand.kind = IR_OPERAND_LABEL;
  operand.name = ir_intern_name(name);
  return operand;
}

IROperand ir_operand_copy(const IROperand *operand) {
  if (!operand) {
    return ir_operand_none();
  }

  switch (operand->kind) {
  case IR_OPERAND_TEMP: {
    IROperand copy = ir_operand_temp(operand->name);
    copy.float_bits = operand->float_bits;
    return copy;
  }
  case IR_OPERAND_SYMBOL: {
    IROperand copy = ir_operand_symbol(operand->name);
    copy.float_bits = operand->float_bits;
    return copy;
  }
  case IR_OPERAND_INT:
    return ir_operand_int(operand->int_value);
  case IR_OPERAND_FLOAT:
    return ir_operand_float_sized(operand->float_value, operand->float_bits);
  case IR_OPERAND_STRING:
    return ir_operand_string_n(operand->name,
                               ir_operand_string_length(operand));
  case IR_OPERAND_LABEL:
    return ir_operand_label(operand->name);
  case IR_OPERAND_NONE:
  default:
    return ir_operand_none();
  }
}

void ir_operand_destroy(IROperand *operand) {
  if (!operand) {
    return;
  }

  switch (operand->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
#ifdef MTLC_POISON_FREED_OPERANDS
    if (operand->name && !string_is_interned(operand->name)) {
      memset(operand->name, 0xA5, strlen(operand->name));
    }
#endif
    mettle_free_string(operand->name);
    break;
  default:
    break;
  }

  *operand = ir_operand_none();
}

static IROperand ir_operand_clone(const IROperand *operand) {
  return ir_operand_copy(operand);
}

/* ---- tensor descriptor block ---------------------------------------------- */

/* Stand-in for an instruction with no block. Const and never written, so every
 * caller reading a descriptor off a non-tensor instruction sees the zeroes the
 * inline fields used to hold. */
static const IRTensorAux g_ir_tensor_absent;

#ifdef METTLE_IR_TENSOR_DEBUG
/* "TNSR". Present on every live block; cleared before the block is freed, so a
 * second free or a later read through a stale alias is caught rather than
 * silently corrupting the free list. */
#define IR_TENSOR_MAGIC 0x544E5352u

static void ir_tensor_check(const IRTensorAux *block, const char *what) {
  if (block->magic != IR_TENSOR_MAGIC) {
    fprintf(stderr,
            "mettle: IR tensor block %s at %p with magic %08x (expected %08x).\n"
            "This means an IRInstruction was duplicated without going through\n"
            "ir_instruction_tensor_copy, or a borrow outlived what it borrowed.\n",
            what, (const void *)block, block->magic, IR_TENSOR_MAGIC);
    abort();
  }
}
#endif

const IRTensorAux *ir_instruction_tensor_ro(const IRInstruction *instruction) {
  if (instruction && instruction->tensor) {
#ifdef METTLE_IR_TENSOR_DEBUG
    ir_tensor_check(instruction->tensor, "read");
#endif
    return instruction->tensor;
  }
  return &g_ir_tensor_absent;
}

void ir_instruction_tensor_clear(IRInstruction *instruction) {
  if (!instruction || !instruction->tensor) {
    return;
  }
  if (instruction->tensor->heap_owned) {
#ifdef METTLE_IR_TENSOR_DEBUG
    ir_tensor_check(instruction->tensor, "freed");
    instruction->tensor->magic = 0;
#endif
    free(instruction->tensor);
  }
  instruction->tensor = NULL;
}

int ir_instruction_tensor_copy(IRInstruction *dst, const IRInstruction *src) {
  if (!dst) {
    return 0;
  }
  ir_instruction_tensor_clear(dst);
  if (!src || !src->tensor) {
    return 1; /* nothing to carry: absent stays absent */
  }
  {
    IRTensorAux *block = (IRTensorAux *)malloc(sizeof(IRTensorAux));
    if (!block) {
      return 0;
    }
    *block = *src->tensor;
    block->heap_owned = 1;
#ifdef METTLE_IR_TENSOR_DEBUG
    block->magic = IR_TENSOR_MAGIC;
#endif
    dst->tensor = block;
  }
  return 1;
}

void ir_instruction_tensor_borrow(IRInstruction *dst, IRTensorAux *block,
                                  const IRInstruction *src) {
  if (!dst || !block) {
    return;
  }
  *block = *ir_instruction_tensor_ro(src);
  block->heap_owned = 0;
#ifdef METTLE_IR_TENSOR_DEBUG
  block->magic = IR_TENSOR_MAGIC;
#endif
  dst->tensor = block;
}

static void ir_instruction_destroy(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }

  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  mettle_free_string(instruction->text);
  ir_instruction_tensor_clear(instruction);

  if (instruction->arguments) {
    for (size_t i = 0; i < instruction->argument_count; i++) {
      ir_operand_destroy(&instruction->arguments[i]);
    }
    free(instruction->arguments);
  }
  free(instruction->argument_types);

  instruction->arguments = NULL;
  instruction->argument_types = NULL;
  instruction->argument_count = 0;
}

static void ir_function_clear_parameters(IRFunction *function) {
  if (!function) {
    return;
  }

  if (function->parameter_names) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      free(function->parameter_names[i]);
    }
    free(function->parameter_names);
  }

  if (function->parameter_types) {
    for (size_t i = 0; i < function->parameter_count; i++) {
      free(function->parameter_types[i]);
    }
    free(function->parameter_types);
  }

  function->parameter_names = NULL;
  function->parameter_types = NULL;
  function->parameter_count = 0;
}

void ir_function_clear_cfg(IRFunction *function) {
  if (!function) {
    return;
  }

  if (function->blocks) {
    for (size_t i = 0; i < function->block_count; i++) {
      free(function->blocks[i].successors);
      free(function->blocks[i].predecessors);
    }
    free(function->blocks);
  }

  function->blocks = NULL;
  function->block_count = 0;
  function->entry_block = 0;
  function->cfg_valid = 0;
}

IRFunction *ir_function_create(const char *name) {
  IRFunction *function = malloc(sizeof(IRFunction));
  if (!function) {
    return NULL;
  }

  function->name = mettle_strdup(name ? name : "<anonymous>");
  function->profile_id = IR_PROFILE_ID_NONE;
  function->parameter_names = NULL;
  function->parameter_types = NULL;
  function->parameter_count = 0;
  function->return_type_name = NULL;
  function->location.line = 0;
  function->location.column = 0;
  function->location.filename = NULL;
  function->instructions = NULL;
  function->instruction_count = 0;
  function->instruction_capacity = 0;
  function->blocks = NULL;
  function->block_count = 0;
  function->entry_block = 0;
  function->cfg_valid = 0;
  function->is_inline = 0;
  function->is_inline_contract = 0;
  function->is_noinline = 0;
  function->is_pure = 0;
  function->is_noalloc = 0;
  function->is_test = 0;
  function->is_swappable = 0;
  function->is_naked = 0;
  function->is_interrupt = 0;
  function->has_volatile_access = 0;
  function->is_kernel = 0;
  function->rewrite_role = 0;
  return function;
}

const char *ir_backend_type_name(const char *source_name) {
  if (source_name && strcmp(source_name, "char") == 0) {
    return "uint8";
  }
  return source_name;
}

int ir_function_set_parameters(IRFunction *function, const char **parameter_names,
                               const char **parameter_types,
                               size_t parameter_count) {
  if (!function) {
    return 0;
  }

  ir_function_clear_parameters(function);

  if (parameter_count == 0) {
    return 1;
  }

  char **name_copies = calloc(parameter_count, sizeof(char *));
  char **type_copies = calloc(parameter_count, sizeof(char *));
  if (!name_copies || !type_copies) {
    free(name_copies);
    free(type_copies);
    return 0;
  }

  for (size_t i = 0; i < parameter_count; i++) {
    if (!parameter_names || !parameter_names[i]) {
      goto fail;
    }

    name_copies[i] = mettle_strdup(parameter_names[i]);
    if (!name_copies[i]) {
      goto fail;
    }

    if (parameter_types && parameter_types[i]) {
      type_copies[i] = mettle_strdup(ir_backend_type_name(parameter_types[i]));
      if (!type_copies[i]) {
        goto fail;
      }
    }
  }

  function->parameter_names = name_copies;
  function->parameter_types = type_copies;
  function->parameter_count = parameter_count;
  return 1;

fail:
  for (size_t i = 0; i < parameter_count; i++) {
    free(name_copies[i]);
    free(type_copies[i]);
  }
  free(name_copies);
  free(type_copies);
  return 0;
}

void ir_function_destroy(IRFunction *function) {
  if (!function) {
    return;
  }

  free(function->name);
  free(function->return_type_name);
  ir_function_clear_parameters(function);
  ir_function_clear_cfg(function);
  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy(&function->instructions[i]);
  }
  free(function->instructions);
  free(function);
}

int ir_function_append_instruction(IRFunction *function,
                                   const IRInstruction *instruction) {
  if (!function || !instruction) {
    return 0;
  }

  if (function->instruction_count >= function->instruction_capacity) {
    /* The first block holds 128, not 64. An IRInstruction is around 576 bytes,
     * so the 64 -> 128 step copies ~36KB, and on a 13k-function program almost
     * every function took it: peak working set is identical either way, which is
     * the measurement saying the arrays were reaching 128 regardless. Starting
     * there just skips the copy -- worth ~50ms of the IR lowering phase. Going
     * further (192, 256, 2048) measured slower: the extra pages have to be
     * touched, and past 512KB each array becomes an individual OS mapping. */
    size_t new_capacity = function->instruction_capacity == 0
                              ? 128
                              : function->instruction_capacity * 2;
    IRInstruction *new_instructions =
        realloc(function->instructions, new_capacity * sizeof(IRInstruction));
    if (!new_instructions) {
      return 0;
    }
    function->instructions = new_instructions;
    function->instruction_capacity = new_capacity;
  }

  IRInstruction *slot = &function->instructions[function->instruction_count];
  *slot = *instruction;
  if (instruction->is_volatile) {
    function->has_volatile_access = 1;
  }

  /* The shallow copy above aliased the source's tensor block; take our own
   * before anything can free either instruction. */
  slot->tensor = NULL;
  if (!ir_instruction_tensor_copy(slot, instruction)) {
    return 0;
  }
  slot->dest = ir_operand_clone(&instruction->dest);
  slot->lhs = ir_operand_clone(&instruction->lhs);
  slot->rhs = ir_operand_clone(&instruction->rhs);
  slot->text = ir_intern_name(instruction->text);
  slot->is_float = instruction->is_float;
  slot->arguments = NULL;
  slot->argument_types = NULL;
  slot->argument_count = instruction->argument_count;

  if (instruction->argument_count > 0) {
    slot->arguments = malloc(instruction->argument_count * sizeof(IROperand));
    if (!slot->arguments) {
      ir_instruction_destroy(slot);
      return 0;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      slot->arguments[i] = ir_operand_clone(&instruction->arguments[i]);
    }
  }

  if (instruction->argument_types && instruction->argument_count > 0) {
    slot->argument_types =
        malloc(instruction->argument_count * sizeof(*slot->argument_types));
    if (!slot->argument_types) {
      ir_instruction_destroy(slot);
      return 0;
    }
    memcpy(slot->argument_types, instruction->argument_types,
           instruction->argument_count * sizeof(*slot->argument_types));
  }

  function->instruction_count++;
  ir_function_clear_cfg(function);
  return 1;
}

int ir_function_insert_instruction(IRFunction *function, size_t index,
                                   const IRInstruction *instruction) {
  if (!function || !instruction || index > function->instruction_count) {
    return 0;
  }

  if (function->instruction_count >= function->instruction_capacity) {
    size_t new_capacity = function->instruction_capacity == 0
                              ? 64
                              : function->instruction_capacity * 2;
    IRInstruction *new_instructions =
        realloc(function->instructions, new_capacity * sizeof(IRInstruction));
    if (!new_instructions) {
      return 0;
    }
    function->instructions = new_instructions;
    function->instruction_capacity = new_capacity;
  }

  if (index < function->instruction_count) {
    memmove(&function->instructions[index + 1], &function->instructions[index],
            (function->instruction_count - index) * sizeof(IRInstruction));
  }

  IRInstruction *slot = &function->instructions[index];
  memset(slot, 0, sizeof(*slot));
  slot->op = instruction->op;
  slot->intrinsic = instruction->intrinsic;
  slot->address_space = instruction->address_space;
  slot->memory_order = instruction->memory_order;
  slot->failure_memory_order = instruction->failure_memory_order;
  slot->memory_scope = instruction->memory_scope;
  slot->memory_regions = instruction->memory_regions;
  slot->async_copy_element_count = instruction->async_copy_element_count;
  slot->async_copy_transaction_bytes =
      instruction->async_copy_transaction_bytes;
  slot->async_copy_pending_groups = instruction->async_copy_pending_groups;
  slot->async_copy_cache = instruction->async_copy_cache;
  slot->async_copy_generated = instruction->async_copy_generated;
  if (!ir_instruction_tensor_copy(slot, instruction)) {
    return 0;
  }
  slot->tensor_transfer_has_prepared_view =
      instruction->tensor_transfer_has_prepared_view;
  slot->tensor_mma_count = instruction->tensor_mma_count;
  slot->tensor_residency_id = instruction->tensor_residency_id;
  slot->tensor_residency_role = instruction->tensor_residency_role;
  slot->tensor_residency_scope = instruction->tensor_residency_scope;
  slot->location = instruction->location;
  slot->is_float = instruction->is_float;
  slot->is_unsigned = instruction->is_unsigned;
  slot->float_bits = instruction->float_bits;
  slot->is_volatile = instruction->is_volatile;
  slot->allocates = instruction->allocates;
  slot->alias_class = instruction->alias_class;
  slot->expansion_note = instruction->expansion_note;
  slot->ast_ref = instruction->ast_ref;
  slot->value_type = instruction->value_type;
  slot->dest = ir_operand_clone(&instruction->dest);
  slot->lhs = ir_operand_clone(&instruction->lhs);
  slot->rhs = ir_operand_clone(&instruction->rhs);
  slot->text = ir_intern_name(instruction->text);
  slot->argument_count = instruction->argument_count;
  slot->arguments = NULL;
  slot->argument_types = NULL;

  if (instruction->argument_count > 0) {
    slot->arguments = malloc(instruction->argument_count * sizeof(IROperand));
    if (!slot->arguments) {
      goto fail_unshift;
    }
    for (size_t i = 0; i < instruction->argument_count; i++) {
      slot->arguments[i] = ir_operand_clone(&instruction->arguments[i]);
    }
  }


  if (instruction->argument_types && instruction->argument_count > 0) {
    slot->argument_types =
        malloc(instruction->argument_count * sizeof(*slot->argument_types));
    if (!slot->argument_types) {
      goto fail_unshift;
    }
    memcpy(slot->argument_types, instruction->argument_types,
           instruction->argument_count * sizeof(*slot->argument_types));
  }

  function->instruction_count++;
  ir_function_clear_cfg(function);
  return 1;

fail_unshift:
  /* The tail was already shifted up to make room. Destroy the partial clone
   * and shift the tail back so the caller sees the function unchanged rather
   * than a stream with a dead slot spliced in. */
  ir_instruction_destroy(slot);
  if (index < function->instruction_count) {
    memmove(&function->instructions[index], &function->instructions[index + 1],
            (function->instruction_count - index) * sizeof(IRInstruction));
  }
  return 0;
}

typedef struct {
  const char *label;
  size_t block_index;
} IRLabelBlock;

static int ir_instruction_is_terminator(const IRInstruction *instruction) {
  return instruction &&
         (instruction->op == IR_OP_JUMP || instruction->op == IR_OP_RETURN);
}

static int ir_instruction_is_branch(const IRInstruction *instruction) {
  return instruction && (instruction->op == IR_OP_BRANCH_ZERO ||
                         instruction->op == IR_OP_BRANCH_EQ);
}

static int ir_block_append_index_unique(size_t **items, size_t *count,
                                        size_t value) {
  if (!items || !count) {
    return 0;
  }

  for (size_t i = 0; i < *count; i++) {
    if ((*items)[i] == value) {
      return 1;
    }
  }

  size_t *grown = realloc(*items, (*count + 1) * sizeof(size_t));
  if (!grown) {
    return 0;
  }

  grown[*count] = value;
  *items = grown;
  (*count)++;
  return 1;
}

static int ir_cfg_append_edge(IRFunction *function, size_t from, size_t to) {
  if (!function || from >= function->block_count || to >= function->block_count) {
    return 0;
  }

  IRBasicBlock *source = &function->blocks[from];
  IRBasicBlock *target = &function->blocks[to];
  if (!ir_block_append_index_unique(&source->successors,
                                    &source->successor_count, to)) {
    return 0;
  }
  return ir_block_append_index_unique(&target->predecessors,
                                      &target->predecessor_count, from);
}

/* Label name -> its block, open addressed.
 *
 * Resolving a branch's target by scanning the label list is quadratic in the
 * number of blocks, and the CFG is rebuilt several times per function. On a
 * body built out of if/else, where nearly every block starts with a label, that
 * was the largest remaining cost in optimizing one. Slots hold label_index + 1
 * so zero stays "empty". */
typedef struct {
  size_t *slots;
  size_t capacity;
} IRCfgLabelIndex;

static void ir_cfg_label_index_destroy(IRCfgLabelIndex *index) {
  free(index->slots);
  index->slots = NULL;
  index->capacity = 0;
}

static int ir_cfg_label_index_build(IRCfgLabelIndex *index,
                                    const IRLabelBlock *labels,
                                    size_t label_count) {
  size_t capacity = 16;
  size_t mask;

  index->slots = NULL;
  index->capacity = 0;
  if (label_count == 0) {
    return 1;
  }
  while (capacity < label_count * 2) {
    capacity *= 2;
  }
  index->slots = (size_t *)calloc(capacity, sizeof(*index->slots));
  if (!index->slots) {
    return 0;
  }
  index->capacity = capacity;
  mask = capacity - 1;
  for (size_t i = 0; i < label_count; i++) {
    size_t slot;
    if (!labels[i].label) {
      continue;
    }
    slot = (size_t)mettle_fnv1a_hash(labels[i].label) & mask;
    while (index->slots[slot]) {
      if (strcmp(labels[index->slots[slot] - 1].label, labels[i].label) == 0) {
        break; /* the first block for a name wins, as the scan did */
      }
      slot = (slot + 1) & mask;
    }
    if (!index->slots[slot]) {
      index->slots[slot] = i + 1;
    }
  }
  return 1;
}

static int ir_cfg_find_label_block(const IRCfgLabelIndex *index,
                                   const IRLabelBlock *labels,
                                   const char *label, size_t *out_block) {
  size_t mask;
  size_t slot;

  if (!index || !index->capacity || !labels || !label || !out_block) {
    return 0;
  }
  mask = index->capacity - 1;
  slot = (size_t)mettle_fnv1a_hash(label) & mask;
  while (index->slots[slot]) {
    const IRLabelBlock *candidate = &labels[index->slots[slot] - 1];
    if (candidate->label && strcmp(candidate->label, label) == 0) {
      *out_block = candidate->block_index;
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

static int ir_cfg_block_last_non_nop(const IRBasicBlock *block,
                                     size_t *out_offset) {
  if (!block || !out_offset || block->instruction_count == 0) {
    return 0;
  }

  for (size_t remaining = block->instruction_count; remaining > 0; remaining--) {
    size_t offset = remaining - 1;
    if (block->instructions[offset].op != IR_OP_NOP) {
      *out_offset = offset;
      return 1;
    }
  }

  return 0;
}

int ir_function_rebuild_cfg(IRFunction *function) {
  if (!function) {
    return 0;
  }

  ir_function_clear_cfg(function);

  size_t instruction_count = function->instruction_count;
  if (instruction_count == 0) {
    function->entry_block = 0;
    function->cfg_valid = 1;
    return 1;
  }

  unsigned char *block_starts = calloc(instruction_count, 1);
  if (!block_starts) {
    return 0;
  }

  block_starts[0] = 1;
  for (size_t i = 0; i < instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_LABEL) {
      block_starts[i] = 1;
    }
    if ((ir_instruction_is_terminator(instruction) ||
         ir_instruction_is_branch(instruction)) &&
        i + 1 < instruction_count) {
      block_starts[i + 1] = 1;
    }
  }

  size_t block_count = 0;
  for (size_t i = 0; i < instruction_count; i++) {
    if (block_starts[i]) {
      block_count++;
    }
  }

  IRBasicBlock *blocks = calloc(block_count, sizeof(IRBasicBlock));
  IRLabelBlock *labels = calloc(block_count, sizeof(IRLabelBlock));
  if (!blocks || !labels) {
    free(blocks);
    free(labels);
    free(block_starts);
    return 0;
  }

  size_t block_index = 0;
  size_t label_count = 0;
  for (size_t start = 0; start < instruction_count;) {
    if (!block_starts[start]) {
      start++;
      continue;
    }

    size_t end = start + 1;
    while (end < instruction_count && !block_starts[end]) {
      end++;
    }

    IRBasicBlock *block = &blocks[block_index];
    block->instructions = &function->instructions[start];
    block->instruction_count = end - start;
    block->first_instruction = start;

    IRInstruction *first = &function->instructions[start];
    if (first->op == IR_OP_LABEL && first->text) {
      block->label = first->text;
      labels[label_count].label = first->text;
      labels[label_count].block_index = block_index;
      label_count++;
    }

    block_index++;
    start = end;
  }

  IRCfgLabelIndex label_index;
  if (!ir_cfg_label_index_build(&label_index, labels, label_count)) {
    free(blocks);
    free(labels);
    free(block_starts);
    return 0;
  }

  function->blocks = blocks;
  function->block_count = block_count;
  function->entry_block = 0;

  for (size_t i = 0; i < block_count; i++) {
    IRBasicBlock *block = &function->blocks[i];
    size_t last_offset = 0;
    if (!ir_cfg_block_last_non_nop(block, &last_offset)) {
      if (i + 1 < block_count && !ir_cfg_append_edge(function, i, i + 1)) {
        ir_function_clear_cfg(function);
        ir_cfg_label_index_destroy(&label_index);
        free(labels);
        free(block_starts);
        return 0;
      }
      continue;
    }

    IRInstruction *last = &block->instructions[last_offset];
    if (last->op == IR_OP_JUMP) {
      size_t target = 0;
      if (ir_cfg_find_label_block(&label_index, labels, last->text, &target) &&
          !ir_cfg_append_edge(function, i, target)) {
        ir_function_clear_cfg(function);
        ir_cfg_label_index_destroy(&label_index);
        free(labels);
        free(block_starts);
        return 0;
      }
    } else if (ir_instruction_is_branch(last)) {
      size_t target = 0;
      if (ir_cfg_find_label_block(&label_index, labels, last->text, &target) &&
          !ir_cfg_append_edge(function, i, target)) {
        ir_function_clear_cfg(function);
        ir_cfg_label_index_destroy(&label_index);
        free(labels);
        free(block_starts);
        return 0;
      }
      if (i + 1 < block_count && !ir_cfg_append_edge(function, i, i + 1)) {
        ir_function_clear_cfg(function);
        ir_cfg_label_index_destroy(&label_index);
        free(labels);
        free(block_starts);
        return 0;
      }
    } else if (last->op != IR_OP_RETURN) {
      if (i + 1 < block_count && !ir_cfg_append_edge(function, i, i + 1)) {
        ir_function_clear_cfg(function);
        ir_cfg_label_index_destroy(&label_index);
        free(labels);
        free(block_starts);
        return 0;
      }
    }
  }

  function->cfg_valid = 1;
  ir_cfg_label_index_destroy(&label_index);
  free(labels);
  free(block_starts);
  return 1;
}

const IRBasicBlock *ir_function_blocks(IRFunction *function,
                                       size_t *block_count) {
  if (block_count) {
    *block_count = 0;
  }
  if (!function) {
    return NULL;
  }
  if (!function->cfg_valid && !ir_function_rebuild_cfg(function)) {
    return NULL;
  }
  if (block_count) {
    *block_count = function->block_count;
  }
  return function->blocks;
}

/* True if any function in the module takes the address of the module-level
 * variable `name`. A pointer that arrives as a parameter carries no
 * provenance, so a global another function pointed at may be written through
 * it here, even in a function that never spells `&g`. Built once per program;
 * the array is owned, the names are borrowed interned IR strings. */
int ir_program_global_address_taken(IRProgram *program, const char *name) {
  if (!program || !name) {
    return 0;
  }
  if (!program->alias_globals_computed) {
    program->alias_globals_computed = 1;
    for (size_t f = 0; f < program->function_count; f++) {
      const IRFunction *fn = program->functions[f];
      if (!fn) {
        continue;
      }
      for (size_t i = 0; i < fn->instruction_count; i++) {
        const IRInstruction *in = &fn->instructions[i];
        const IRModuleSymbol *sym = NULL;
        const char **grown = NULL;
        int present = 0;
        if (in->op != IR_OP_ADDRESS_OF || in->lhs.kind != IR_OPERAND_SYMBOL ||
            !in->lhs.name) {
          continue;
        }
        sym = ir_program_lookup_symbol(program, in->lhs.name);
        if (!sym || sym->kind != IR_MODSYM_VARIABLE) {
          continue;
        }
        for (size_t k = 0; k < program->alias_global_count; k++) {
          if (strcmp(program->alias_globals[k], in->lhs.name) == 0) {
            present = 1;
            break;
          }
        }
        if (present) {
          continue;
        }
        grown = (const char **)realloc(
            (void *)program->alias_globals,
            (program->alias_global_count + 1) * sizeof(*grown));
        if (!grown) {
          return 1;
        }
        program->alias_globals = grown;
        program->alias_globals[program->alias_global_count++] = in->lhs.name;
      }
    }
  }
  for (size_t k = 0; k < program->alias_global_count; k++) {
    if (strcmp(program->alias_globals[k], name) == 0) {
      return 1;
    }
  }
  return 0;
}

IRProgram *ir_program_create(void) {
  IRProgram *program = malloc(sizeof(IRProgram));
  if (!program) {
    return NULL;
  }

  program->functions = NULL;
  program->function_count = 0;
  program->function_capacity = 0;
  program->profile_entries = NULL;
  program->profile_entry_count = 0;
  program->profile_entry_capacity = 0;
  program->debug_local_entries = NULL;
  program->debug_local_entry_count = 0;
  program->debug_local_entry_capacity = 0;
  program->type_registry = NULL;
  program->type_registry_count = 0;
  program->type_registry_capacity = 0;
  program->owned_types = NULL;
  program->owned_type_count = 0;
  program->owned_type_capacity = 0;
  program->module_symbols = NULL;
  program->module_symbol_count = 0;
  program->module_symbol_capacity = 0;
  program->main_wants_argc_argv = 0;
  program->dead_functions_eliminated = 0;
  program->alias_globals = NULL;
  program->alias_global_count = 0;
  program->alias_globals_computed = 0;
  return program;
}

static void ir_symbol_index_invalidate(const IRProgram *program);
static void ir_type_index_invalidate(const IRProgram *program);

static void ir_symbol_index_reset(void);

static void ir_module_symbol_release(IRModuleSymbol *symbol) {
  free(symbol->name);
  free(symbol->link_name);
  free(symbol->init_string);
  free(symbol->init_symbol_ref);
  for (size_t r = 0; r < symbol->init_reloc_count; r++) {
    free(symbol->init_relocs[r].symbol);
    free(symbol->init_relocs[r].string);
  }
  free(symbol->init_relocs);
  free(symbol->init_bytes);
  free(symbol->param_types);
  free(symbol->codegen_view);
}

void ir_program_destroy(IRProgram *program) {
  if (!program) {
    return;
  }
  /* A later program allocated at this address must not inherit the cached
   * name->index table (the incremental update path trusts entries [0, cached)
   * to be unchanged, which only holds within one program's lifetime). */
  ir_symbol_index_invalidate(program);
  ir_type_index_invalidate(program);
  free(program->alias_globals);
  program->alias_globals = NULL;
  program->alias_global_count = 0;
  program->alias_globals_computed = 0;

  if (program->functions) {
    for (size_t i = 0; i < program->function_count; i++) {
      ir_function_destroy(program->functions[i]);
    }
    free(program->functions);
  }
  if (program->profile_entries) {
    for (size_t i = 0; i < program->profile_entry_count; i++) {
      free(program->profile_entries[i].name);
      free(program->profile_entries[i].filename);
    }
    free(program->profile_entries);
  }
  if (program->debug_local_entries) {
    for (size_t i = 0; i < program->debug_local_entry_count; i++) {
      free(program->debug_local_entries[i].name);
      free(program->debug_local_entries[i].type_name);
    }
    free(program->debug_local_entries);
  }
  if (program->type_registry) {
    for (size_t i = 0; i < program->type_registry_count; i++) {
      free(program->type_registry[i].name);
    }
    free(program->type_registry);
  }
  if (program->owned_types) {
    for (size_t i = 0; i < program->owned_type_count; i++) {
      MtlcType *type = program->owned_types[i];
      if (type) {
        free((char *)type->name);
        free(type);
      }
    }
    free(program->owned_types);
  }
  if (program->module_symbols) {
    for (size_t i = 0; i < program->module_symbol_count; i++) {
      ir_module_symbol_release(&program->module_symbols[i]);
    }
    free(program->module_symbols);
  }
  free(program);
}

int ir_program_drop_rewrite_rules(IRProgram *program) {
  size_t kept = 0;
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function || !function->rewrite_role) {
      program->functions[kept++] = function;
      continue;
    }
    size_t symbols_kept = 0;
    for (size_t s = 0; s < program->module_symbol_count; s++) {
      IRModuleSymbol *symbol = &program->module_symbols[s];
      if (symbol->kind == IR_MODSYM_FUNCTION && symbol->name &&
          function->name && strcmp(symbol->name, function->name) == 0) {
        ir_module_symbol_release(symbol);
        continue;
      }
      if (symbols_kept != s) {
        program->module_symbols[symbols_kept] = *symbol;
      }
      symbols_kept++;
    }
    program->module_symbol_count = symbols_kept;
    ir_symbol_index_reset();
    ir_function_destroy(function);
  }
  program->function_count = kept;
  return 1;
}

/* Name -> type-registry index. Same shape and lifecycle as IRSymbolIndex
 * below: the registry is append-only (update-in-place never changes a name),
 * so the cache tops up incrementally and only rebuilds on load-factor
 * overflow. ir_program_lookup_type runs per codegen instruction
 * (mir_addressof_kind and friends), so the old linear scan was a measurable
 * constant on 500k-LOC modules. */
typedef struct {
  size_t *slots; /* index+1 into type_registry; 0 = empty */
  size_t slot_count; /* power of two */
  const IRProgram *program;
  size_t entry_count;
} IRTypeIndex;

static IRTypeIndex g_ir_type_index = {0};

static void ir_type_index_reset(void) {
  free(g_ir_type_index.slots);
  g_ir_type_index.slots = NULL;
  g_ir_type_index.slot_count = 0;
  g_ir_type_index.program = NULL;
  g_ir_type_index.entry_count = 0;
}

static void ir_type_index_invalidate(const IRProgram *program) {
  if (g_ir_type_index.program == program) {
    ir_type_index_reset();
  }
}

static void ir_type_index_insert(const IRProgram *program, size_t i) {
  size_t mask = g_ir_type_index.slot_count - 1;
  const char *name = program->type_registry[i].name;
  if (!name) {
    return;
  }
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (g_ir_type_index.slots[h]) {
    if (strcmp(program->type_registry[g_ir_type_index.slots[h] - 1].name,
               name) == 0) {
      return; /* names are unique; keep the first entry */
    }
    h = (h + 1) & mask;
  }
  g_ir_type_index.slots[h] = i + 1;
}

/* Returns the registry index of `name`, or (size_t)-1 when absent. Falls back
 * to the linear scan if the table cannot be allocated. */
static size_t ir_type_index_find(const IRProgram *program, const char *name) {
  if (!(g_ir_type_index.program == program && g_ir_type_index.slots &&
        g_ir_type_index.entry_count <= program->type_registry_count &&
        program->type_registry_count * 2 <= g_ir_type_index.slot_count)) {
    ir_type_index_reset();
    size_t slot_count = 64;
    while (slot_count < program->type_registry_count * 4) {
      slot_count *= 2;
    }
    size_t *slots = calloc(slot_count, sizeof(*slots));
    if (!slots) {
      for (size_t i = 0; i < program->type_registry_count; i++) {
        if (strcmp(program->type_registry[i].name, name) == 0) {
          return i;
        }
      }
      return (size_t)-1;
    }
    g_ir_type_index.slots = slots;
    g_ir_type_index.slot_count = slot_count;
    g_ir_type_index.program = program;
    g_ir_type_index.entry_count = 0;
  }
  for (size_t i = g_ir_type_index.entry_count;
       i < program->type_registry_count; i++) {
    ir_type_index_insert(program, i);
  }
  g_ir_type_index.entry_count = program->type_registry_count;

  size_t mask = g_ir_type_index.slot_count - 1;
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (g_ir_type_index.slots[h]) {
    size_t idx = g_ir_type_index.slots[h] - 1;
    if (strcmp(program->type_registry[idx].name, name) == 0) {
      return idx;
    }
    h = (h + 1) & mask;
  }
  return (size_t)-1;
}

int ir_program_register_type(IRProgram *program, const char *name,
                             MtlcType *type) {
  if (!program || !name) {
    return 0;
  }
  size_t existing = ir_type_index_find(program, name);
  if (existing != (size_t)-1) {
    program->type_registry[existing].type = type; /* update in place */
    return 1;
  }
  if (program->type_registry_count == program->type_registry_capacity) {
    size_t next = program->type_registry_capacity
                      ? program->type_registry_capacity * 2
                      : 32;
    IRTypeEntry *grown =
        realloc(program->type_registry, next * sizeof(IRTypeEntry));
    if (!grown) {
      return 0;
    }
    program->type_registry = grown;
    program->type_registry_capacity = next;
  }
  IRTypeEntry *entry = &program->type_registry[program->type_registry_count];
  entry->name = mettle_strdup(name);
  if (!entry->name) {
    return 0;
  }
  entry->type = type;
  program->type_registry_count++;
  return 1;
}

MtlcType *ir_program_lookup_type(const IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }
  size_t idx = ir_type_index_find(program, name);
  return idx != (size_t)-1 ? program->type_registry[idx].type : NULL;
}

IRModuleSymbol *ir_program_add_symbol(IRProgram *program,
                                      const IRModuleSymbol *proto) {
  if (!program || !proto || !proto->name) {
    return NULL;
  }
  if (program->module_symbol_count == program->module_symbol_capacity) {
    size_t next = program->module_symbol_capacity
                      ? program->module_symbol_capacity * 2
                      : 32;
    IRModuleSymbol *grown =
        realloc(program->module_symbols, next * sizeof(IRModuleSymbol));
    if (!grown) {
      return NULL;
    }
    program->module_symbols = grown;
    program->module_symbol_capacity = next;
  }
  IRModuleSymbol *dst = &program->module_symbols[program->module_symbol_count];
  *dst = *proto; /* shallow copy scalars + borrowed MtlcType* */
  dst->name = mettle_strdup(proto->name);
  dst->link_name = proto->link_name ? mettle_strdup(proto->link_name) : NULL;
  dst->init_string =
      proto->init_string
          ? ir_copy_literal_bytes(proto->init_string, proto->init_string_length)
          : NULL;
  dst->init_string_length = proto->init_string ? proto->init_string_length : 0;
  dst->init_symbol_ref =
      proto->init_symbol_ref ? mettle_strdup(proto->init_symbol_ref) : NULL;
  /* Deep-copy the aggregate initializer image so the symbol table owns it
   * independently of the frontend AST it was folded on. */
  dst->init_bytes = NULL;
  dst->init_bytes_size = 0;
  dst->init_relocs = NULL;
  dst->init_reloc_count = 0;
  if (proto->init_bytes && proto->init_bytes_size > 0) {
    dst->init_bytes = malloc(proto->init_bytes_size);
    if (!dst->init_bytes) {
      free(dst->name);
      free(dst->link_name);
      free(dst->init_string);
      free(dst->init_symbol_ref);
      return NULL;
    }
    memcpy(dst->init_bytes, proto->init_bytes, proto->init_bytes_size);
    dst->init_bytes_size = proto->init_bytes_size;
    if (proto->init_reloc_count > 0 && proto->init_relocs) {
      dst->init_relocs =
          calloc(proto->init_reloc_count, sizeof(IRInitReloc));
      if (!dst->init_relocs) {
        free(dst->init_bytes);
        free(dst->name);
        free(dst->link_name);
        free(dst->init_string);
        free(dst->init_symbol_ref);
        return NULL;
      }
      for (size_t r = 0; r < proto->init_reloc_count; r++) {
        dst->init_relocs[r].offset = proto->init_relocs[r].offset;
        dst->init_relocs[r].symbol =
            proto->init_relocs[r].symbol
                ? mettle_strdup(proto->init_relocs[r].symbol)
                : NULL;
        dst->init_relocs[r].string =
            proto->init_relocs[r].string
                ? ir_copy_literal_bytes(proto->init_relocs[r].string,
                                        proto->init_relocs[r].string_length)
                : NULL;
        dst->init_relocs[r].string_length =
            proto->init_relocs[r].string ? proto->init_relocs[r].string_length
                                         : 0;
        dst->init_relocs[r].string_wants_record =
            proto->init_relocs[r].string_wants_record;
      }
      dst->init_reloc_count = proto->init_reloc_count;
    }
  }
  dst->param_types = NULL;
  if (proto->param_count > 0 && proto->param_types) {
    dst->param_types = malloc(proto->param_count * sizeof(MtlcType *));
    if (!dst->param_types) {
      free(dst->name);
      free(dst->link_name);
      free(dst->init_string);
      free(dst->init_symbol_ref);
      return NULL;
    }
    memcpy(dst->param_types, proto->param_types,
           proto->param_count * sizeof(MtlcType *));
  }
  if (!dst->name) {
    return NULL;
  }
  program->module_symbol_count++;
  return dst;
}

/* Name -> module-symbol index for ir_program_lookup_symbol.
 *
 * The linear scan below is called for every symbol reference codegen resolves,
 * and a frontend with many string literals pushes module_symbol_count into the
 * tens of thousands, O(references x symbols) strcmp dominated emission on
 * large programs. Cache an open-addressing table keyed on the program, its
 * symbol count, and the array's base address (module_symbols reallocs as
 * symbols are added, so the table stores indices, never pointers), rebuilding
 * whenever any of those change. Same cure BinaryIRFunctionIndex applies to
 * function lookup in the binary backend. */
typedef struct {
  size_t *slots; /* index+1 into module_symbols; 0 = empty */
  size_t slot_count; /* power of two */
  const IRProgram *program;
  const IRModuleSymbol *symbols_base;
  size_t symbol_count;
} IRSymbolIndex;

static IRSymbolIndex g_ir_symbol_index = {0};

static void ir_symbol_index_reset(void) {
  free(g_ir_symbol_index.slots);
  g_ir_symbol_index.slots = NULL;
  g_ir_symbol_index.slot_count = 0;
  g_ir_symbol_index.program = NULL;
  g_ir_symbol_index.symbols_base = NULL;
  g_ir_symbol_index.symbol_count = 0;
}

static void ir_symbol_index_invalidate(const IRProgram *program) {
  if (g_ir_symbol_index.program == program) {
    ir_symbol_index_reset();
  }
}

/* Insert module_symbols[i] into the table. "First entry with a given name
 * wins", matching the original linear scan's duplicate semantics. */
static void ir_symbol_index_insert(const IRProgram *program, size_t i) {
  size_t mask = g_ir_symbol_index.slot_count - 1;
  const char *name = program->module_symbols[i].name;
  if (!name) {
    return;
  }
  size_t h = mettle_fnv1a_hash(name) & mask;
  while (g_ir_symbol_index.slots[h]) {
    if (strcmp(program->module_symbols[g_ir_symbol_index.slots[h] - 1].name,
               name) == 0) {
      return; /* earlier symbol with this name wins */
    }
    h = (h + 1) & mask;
  }
  g_ir_symbol_index.slots[h] = i + 1;
}

/* Returns 1 with the table ready, 0 on allocation failure (callers fall back
 * to the linear scan rather than miss real symbols).
 *
 * The module symbol array is append-only for a program's lifetime, so when the
 * cached table belongs to this program and only trails by newly appended
 * entries, we top it up instead of rebuilding. Rebuilding from scratch on
 * every count change made interleaved add/lookup sequences (each global
 * initializer resolves the globals before it) quadratic: 200k globals sat in
 * IR lowering for over five minutes. The base address is NOT part of the
 * validity check - slots store indices, and realloc preserves contents. */
static int ir_symbol_index_ensure(const IRProgram *program) {
  if (g_ir_symbol_index.program == program && g_ir_symbol_index.slots &&
      g_ir_symbol_index.symbol_count <= program->module_symbol_count &&
      program->module_symbol_count * 2 <= g_ir_symbol_index.slot_count) {
    for (size_t i = g_ir_symbol_index.symbol_count;
         i < program->module_symbol_count; i++) {
      ir_symbol_index_insert(program, i);
    }
    g_ir_symbol_index.symbols_base = program->module_symbols;
    g_ir_symbol_index.symbol_count = program->module_symbol_count;
    return 1;
  }

  ir_symbol_index_reset();

  size_t slot_count = 16;
  /* Size for 4x the current count so appends reuse the table for a while
   * before the load-factor check above forces a rebuild. */
  while (slot_count < program->module_symbol_count * 4) {
    slot_count *= 2;
  }
  size_t *slots = calloc(slot_count, sizeof(*slots));
  if (!slots) {
    return 0;
  }

  g_ir_symbol_index.slots = slots;
  g_ir_symbol_index.slot_count = slot_count;
  g_ir_symbol_index.program = program;
  g_ir_symbol_index.symbols_base = program->module_symbols;
  g_ir_symbol_index.symbol_count = 0;
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    ir_symbol_index_insert(program, i);
  }
  g_ir_symbol_index.symbol_count = program->module_symbol_count;

  return 1;
}

const IRModuleSymbol *ir_program_lookup_symbol(const IRProgram *program,
                                               const char *name) {
  if (!program || !name) {
    return NULL;
  }

  if (ir_symbol_index_ensure(program)) {
    size_t mask = g_ir_symbol_index.slot_count - 1;
    size_t h = mettle_fnv1a_hash(name) & mask;
    while (g_ir_symbol_index.slots[h]) {
      const IRModuleSymbol *sym =
          &program->module_symbols[g_ir_symbol_index.slots[h] - 1];
      if (strcmp(sym->name, name) == 0) {
        return sym;
      }
      h = (h + 1) & mask;
    }
    return NULL;
  }

  /* Fallback: index allocation failed; behave as before. */
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    if (strcmp(program->module_symbols[i].name, name) == 0) {
      return &program->module_symbols[i];
    }
  }
  return NULL;
}

int ir_program_add_function(IRProgram *program, IRFunction *function) {
  if (!program || !function) {
    return 0;
  }

  if (program->function_count >= program->function_capacity) {
    size_t new_capacity =
        program->function_capacity == 0 ? 16 : program->function_capacity * 2;
    IRFunction **new_functions =
        realloc(program->functions, new_capacity * sizeof(IRFunction *));
    if (!new_functions) {
      return 0;
    }
    program->functions = new_functions;
    program->function_capacity = new_capacity;
  }

  program->functions[program->function_count++] = function;
  return 1;
}

const char *ir_opcode_name(IROpcode op) {
  switch (op) {
  case IR_OP_NOP:
    return "nop";
  case IR_OP_LABEL:
    return "label";
  case IR_OP_JUMP:
    return "jump";
  case IR_OP_BRANCH_ZERO:
    return "branch_zero";
  case IR_OP_BRANCH_EQ:
    return "branch_eq";
  case IR_OP_DECLARE_LOCAL:
    return "local";
  case IR_OP_ADDRESS_SPACE_ALLOC:
    return "address_space_alloc";
  case IR_OP_BARRIER:
    return "barrier";
  case IR_OP_ASYNC_COPY:
    return "async_copy";
  case IR_OP_ASYNC_COMMIT:
    return "async_commit";
  case IR_OP_ASYNC_WAIT:
    return "async_wait";
  case IR_OP_TENSOR_TRANSFER:
    return "tensor_transfer";
  case IR_OP_TENSOR_MMA:
    return "tensor_mma";
  case IR_OP_TENSOR_MATMUL:
    return "tensor_matmul";
  case IR_OP_TENSOR_EPILOGUE:
    return "tensor_epilogue";
  case IR_OP_TENSOR_COMMIT:
    return "tensor_commit";
  case IR_OP_ASSIGN:
    return "assign";
  case IR_OP_ADDRESS_OF:
    return "addr_of";
  case IR_OP_LOAD:
    return "load";
  case IR_OP_STORE:
    return "store";
  case IR_OP_PREFETCH:
    return "prefetch";
  case IR_OP_SELECT:
    return "select";
  case IR_OP_SAFETY_CHECK:
    return "safety_check";
  case IR_OP_BINARY:
    return "binary";
  case IR_OP_ROTATE_ADD:
    return "rotate_add";
  case IR_OP_UNARY:
    return "unary";
  case IR_OP_CALL:
    return "call";
  case IR_OP_CALL_INDIRECT:
    return "call_indirect";
  case IR_OP_GPU_LAUNCH:
    return "gpu_launch";
  case IR_OP_NEW:
    return "new";
  case IR_OP_RETURN:
    return "return";
  case IR_OP_INLINE_ASM:
    return "inline_asm";
  case IR_OP_CAST:
    return "cast";
  case IR_OP_COUNT_WORD_STARTS: return "count_word_starts";
  case IR_OP_MEMCPY_INLINE: return "memcpy_inline";
  case IR_OP_SIMD_SUM_I32: return "simd_sum_i32";
  case IR_OP_SIMD_SUM_U8: return "simd_sum_u8";
  case IR_OP_SIMD_BYTE_MAP: return "simd_byte_map";
  case IR_OP_SIMD_FILL: return "simd_fill";
  case IR_OP_SIMD_MATMUL_N32: return "simd_matmul_n32";
  case IR_OP_SIMD_INSERTION_SORT_I32: return "simd_insertion_sort_i32";
  case IR_OP_SIMD_DOT_I32: return "simd_dot_i32";
  case IR_OP_SIMD_DOT_I8: return "simd_dot_i8";
  case IR_OP_SIMD_SLP_MAC_I32: return "simd_slp_mac_i32";
  case IR_OP_SIMD_SLP_MAC_I8: return "simd_slp_mac_i8";
  case IR_OP_SIMD_SCALE_I32: return "simd_scale_i32";
  case IR_OP_SIMD_CLAMP_I32: return "simd_clamp_i32";
  case IR_OP_SIMD_REVERSE_COPY_I32: return "simd_reverse_copy_i32";
  case IR_OP_LOWER_BOUND_I32: return "lower_bound_i32";
  case IR_OP_PREFIX_SUM_I32: return "prefix_sum_i32";
  case IR_OP_SIMD_MINMAX_I32: return "simd_minmax_i32";
  case IR_OP_SIMD_SUM_F64: return "simd_sum_f64";
  case IR_OP_SIMD_SUM_F32: return "simd_sum_f32";
  case IR_OP_SIMD_DOT_F64: return "simd_dot_f64";
  case IR_OP_SIMD_DOT_F32: return "simd_dot_f32";
  case IR_OP_SIMD_AFFINE_MAP_F64: return "simd_affine_map_f64";
  case IR_OP_SIMD_AFFINE_MAP_F32: return "simd_affine_map_f32";
  case IR_OP_SIMD_EXP_F32: return "simd_exp_f32";
  case IR_OP_SIMD_I2F_REDUCE_F64: return "simd_i2f_reduce_f64";
  case IR_OP_SIMD_VLOOP_F64: return "simd_vloop_f64";
  case IR_OP_SIMD_VLOOP_I32: return "simd_vloop_i32";
  case IR_OP_SIMD_FIND: return "simd_find";
  case IR_OP_SIMD_OUTER_LANE_F64: return "simd_outer_lane_f64";
  default:
    return "unknown";
  }
}

static void ir_format_operand(const IROperand *operand, char *buffer,
                              size_t buffer_size) {
  if (!buffer || buffer_size == 0) {
    return;
  }

  if (!operand) {
    snprintf(buffer, buffer_size, "_");
    return;
  }

  switch (operand->kind) {
  case IR_OPERAND_NONE:
    snprintf(buffer, buffer_size, "_");
    break;
  case IR_OPERAND_TEMP:
    snprintf(buffer, buffer_size, "%%%s", operand->name ? operand->name : "?");
    break;
  case IR_OPERAND_SYMBOL:
    snprintf(buffer, buffer_size, "@%s", operand->name ? operand->name : "?");
    break;
  case IR_OPERAND_INT:
    snprintf(buffer, buffer_size, "%lld", operand->int_value);
    break;
  case IR_OPERAND_FLOAT:
    snprintf(buffer, buffer_size, "%f", operand->float_value);
    break;
  case IR_OPERAND_STRING:
    snprintf(buffer, buffer_size, "\"%s\"", operand->name ? operand->name : "");
    break;
  case IR_OPERAND_LABEL:
    snprintf(buffer, buffer_size, "%s", operand->name ? operand->name : "?");
    break;
  default:
    snprintf(buffer, buffer_size, "_");
    break;
  }
}

static int ir_format_control_line(const IRInstruction *instruction,
                             const char *dest, const char *lhs,
                             const char *rhs, char *buffer,
                             size_t buffer_size, int *handled) {
  int written = 0;
  (void)dest;
  (void)lhs;
  (void)rhs;
  *handled = 1;
  switch (instruction->op) {
  case IR_OP_LABEL:
    written = snprintf(buffer, buffer_size, "%s %s", ir_opcode_name(instruction->op),
                       instruction->text ? instruction->text : "<label>");
    break;
  case IR_OP_JUMP:
    written = snprintf(buffer, buffer_size, "%s %s", ir_opcode_name(instruction->op),
                       instruction->text ? instruction->text : "<target>");
    break;
  case IR_OP_BRANCH_ZERO:
    written = snprintf(buffer, buffer_size, "%s %s -> %s",
                       ir_opcode_name(instruction->op), lhs,
                       instruction->text ? instruction->text : "<target>");
    break;
  case IR_OP_BRANCH_EQ:
    written = snprintf(buffer, buffer_size, "%s %s, %s -> %s",
                       ir_opcode_name(instruction->op), lhs, rhs,
                       instruction->text ? instruction->text : "<target>");
    break;
  case IR_OP_DECLARE_LOCAL:
    written = snprintf(buffer, buffer_size, "%s %s : %s",
                       ir_opcode_name(instruction->op), dest,
                       instruction->text ? instruction->text : "<unknown>");
    break;
  default:
    *handled = 0;
    break;
  }
  return written;
}

static int ir_format_value_line(const IRInstruction *instruction,
                             const char *dest, const char *lhs,
                             const char *rhs, char *buffer,
                             size_t buffer_size, int *handled) {
  int written = 0;
  (void)dest;
  (void)lhs;
  (void)rhs;
  *handled = 1;
  switch (instruction->op) {
  case IR_OP_ASSIGN:
    written = snprintf(buffer, buffer_size, "%s <- %s", dest, lhs);
    break;
  case IR_OP_ADDRESS_OF:
    written = snprintf(buffer, buffer_size, "%s <- &%s", dest, lhs);
    break;
  case IR_OP_LOAD:
    written = snprintf(buffer, buffer_size, "%s <- *%s [%s]%s", dest, lhs, rhs,
                       instruction->is_volatile ? " volatile" : "");
    break;
  case IR_OP_STORE:
    written = snprintf(buffer, buffer_size, "*%s <- %s [%s]%s", dest, lhs, rhs,
                       instruction->is_volatile ? " volatile" : "");
    break;
  case IR_OP_BINARY:
    written = snprintf(buffer, buffer_size, "%s = %s %s%s %s", dest, lhs,
                       instruction->text ? instruction->text : "?",
                       instruction->is_float ? " (float)" : "", rhs);
    break;
  case IR_OP_ROTATE_ADD:
    written = snprintf(buffer, buffer_size, "%s = rotate_add(%s, %s)", dest, lhs,
                       rhs);
    break;
  case IR_OP_UNARY:
    written = snprintf(buffer, buffer_size, "%s = %s%s%s", dest,
                       instruction->text ? instruction->text : "?", lhs,
                       instruction->is_float ? " (float)" : "");
    break;
  case IR_OP_NEW:
    written = snprintf(buffer, buffer_size, "%s = %s [%s]", dest,
                       instruction->text ? instruction->text : "<type>", rhs);
    break;
  case IR_OP_RETURN:
    written = snprintf(buffer, buffer_size, "return %s", lhs);
    break;
  case IR_OP_INLINE_ASM:
    written = snprintf(buffer, buffer_size, "inline_asm \"%s\"",
                       instruction->text ? instruction->text : "");
    break;
  case IR_OP_CAST:
    written = snprintf(buffer, buffer_size, "%s = (%s)%s%s", dest,
                       instruction->text ? instruction->text : "<type>", lhs,
                       instruction->is_float ? " (float)" : "");
    break;
  case IR_OP_COUNT_WORD_STARTS:
    written = snprintf(buffer, buffer_size, "%s = count_word_starts(buf=%s, len=%s)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SELECT: {
    char else_val[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      else_val, sizeof(else_val));
    if (instruction->text && instruction->argument_count > 1) {
      char cmp_rhs[128];
      ir_format_operand(&instruction->arguments[1], cmp_rhs, sizeof(cmp_rhs));
      written = snprintf(buffer, buffer_size, "%s = select(%s %s %s, %s, %s)",
                         dest, lhs, instruction->text, cmp_rhs, rhs, else_val);
    } else {
      written = snprintf(buffer, buffer_size, "%s = select(%s, %s, %s)", dest,
                         lhs, rhs, else_val);
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return written;
}

static int ir_format_call_line(const IRInstruction *instruction,
                             const char *dest, const char *lhs,
                             const char *rhs, char *buffer,
                             size_t buffer_size, int *handled) {
  int written = 0;
  (void)dest;
  (void)lhs;
  (void)rhs;
  *handled = 1;
  switch (instruction->op) {
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT: {
    size_t offset = 0;
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "%s = %s(",
                               dest, instruction->text ? instruction->text
                                                       : "<callee>");
    for (size_t arg_i = 0; arg_i < instruction->argument_count; arg_i++) {
      char arg_buffer[128];
      ir_format_operand(&instruction->arguments[arg_i], arg_buffer,
                        sizeof(arg_buffer));
      offset +=
          (size_t)snprintf(buffer + offset, buffer_size - offset, "%s%s",
                           arg_i == 0 ? "" : ", ", arg_buffer);
      if (offset >= buffer_size) {
        break;
      }
    }
    if (ir_intrinsic_is_atomic(instruction->intrinsic)) {
      if (ir_intrinsic_is_compare_exchange(instruction->intrinsic)) {
        written = (int)snprintf(
            buffer + offset, buffer_size - offset,
            ") [%s success=%s failure=%s %s]",
            ir_address_space_name(instruction->address_space),
            ir_memory_order_name(instruction->memory_order),
            ir_memory_order_name(instruction->failure_memory_order),
            ir_memory_scope_name(instruction->memory_scope));
      } else {
        written = (int)snprintf(
            buffer + offset, buffer_size - offset, ") [%s %s %s]",
            ir_address_space_name(instruction->address_space),
            ir_memory_order_name(instruction->memory_order),
            ir_memory_scope_name(instruction->memory_scope));
      }
    } else {
      written = (int)snprintf(buffer + offset, buffer_size - offset, ")");
    }
    if (written >= 0) {
      written += (int)offset;
    }
    break;
  }
  case IR_OP_GPU_LAUNCH: {
    size_t offset = 0;
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset,
                               "gpu_launch %s [", lhs);
    for (size_t arg_i = 0; arg_i < instruction->argument_count; arg_i++) {
      char arg_buffer[128];
      ir_format_operand(&instruction->arguments[arg_i], arg_buffer,
                        sizeof(arg_buffer));
      offset += (size_t)snprintf(buffer + offset, buffer_size - offset,
                                "%s%s", arg_i == 0 ? "" : ", ",
                                arg_buffer);
      if (offset >= buffer_size) {
        break;
      }
    }
    written = (int)snprintf(buffer + offset, buffer_size - offset, "]");
    if (written >= 0) {
      written += (int)offset;
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return written;
}

static int ir_format_gpu_line(const IRInstruction *instruction,
                             const char *dest, const char *lhs,
                             const char *rhs, char *buffer,
                             size_t buffer_size, int *handled) {
  int written = 0;
  (void)dest;
  (void)lhs;
  (void)rhs;
  *handled = 1;
  switch (instruction->op) {
  case IR_OP_ADDRESS_SPACE_ALLOC:
    if (instruction->rhs.kind == IR_OPERAND_INT &&
        instruction->rhs.int_value == 0) {
      written = snprintf(buffer, buffer_size,
                         "%s <- view.%s.dynamic %s[*]", dest,
                         ir_address_space_name(instruction->address_space),
                         instruction->text ? instruction->text : "<unknown>");
    } else {
      written = snprintf(buffer, buffer_size, "%s <- alloc.%s %s x %s", dest,
                         ir_address_space_name(instruction->address_space),
                         instruction->text ? instruction->text : "<unknown>",
                         rhs);
    }
    break;
  case IR_OP_BARRIER: {
    const char *regions =
        instruction->memory_regions ==
                (MTLC_MEMORY_REGION_WORKGROUP | MTLC_MEMORY_REGION_GLOBAL)
            ? "workgroup+global"
        : (instruction->memory_regions & MTLC_MEMORY_REGION_WORKGROUP)
            ? "workgroup"
        : (instruction->memory_regions & MTLC_MEMORY_REGION_GLOBAL)
            ? "global"
            : "none";
    written = snprintf(buffer, buffer_size, "barrier.workgroup %s %s",
                       ir_memory_order_name(instruction->memory_order), regions);
    break;
  }
  case IR_OP_ASYNC_COPY:
    ir_format_operand(instruction->argument_count > 0
                          ? &instruction->arguments[0]
                          : NULL,
                      dest, sizeof(dest));
    ir_format_operand(instruction->argument_count > 1
                          ? &instruction->arguments[1]
                          : NULL,
                      lhs, sizeof(lhs));
    written = snprintf(
        buffer, buffer_size,
        "async_copy.workgroup %s <- %s x%u transaction=%u cache.%s%s",
        dest, lhs,
        instruction->async_copy_element_count,
        instruction->async_copy_transaction_bytes,
        instruction->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL ? "global"
                                                                 : "all",
        instruction->async_copy_generated ? " generated" : "");
    break;
  case IR_OP_ASYNC_COMMIT:
    written = snprintf(buffer, buffer_size, "async_copy.commit");
    break;
  case IR_OP_ASYNC_WAIT:
    written = snprintf(buffer, buffer_size, "async_copy.wait pending=%u",
                       instruction->async_copy_pending_groups);
    break;
  case IR_OP_TENSOR_TRANSFER: {
    const MtlcTensorTransferDesc *desc = &IR_TENSOR_TRANSFER(instruction);
    written = snprintf(
        buffer, buffer_size,
        "tensor_transfer.workgroup %s rank=%u element=%d tile=(%u,%u,%u,%u,%u) view=%s",
        desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP
            ? "global->workgroup"
            : "workgroup->global",
        (unsigned)desc->rank, (int)desc->element,
        (unsigned)desc->tile_extent[0], (unsigned)desc->tile_extent[1],
        (unsigned)desc->tile_extent[2], (unsigned)desc->tile_extent[3],
        (unsigned)desc->tile_extent[4],
        instruction->tensor_transfer_has_prepared_view ? "prepared"
                                                       : "none");
    break;
  }
  case IR_OP_TENSOR_MMA:
    {
    char residency[64] = {0};
    const char *scope =
        instruction->tensor_residency_scope ==
                IR_TENSOR_RESIDENCY_SCOPE_PIPELINE
            ? "pipeline."
            : instruction->tensor_residency_scope ==
                      IR_TENSOR_RESIDENCY_SCOPE_LOOP
                  ? "loop."
                  : "";
    if (instruction->tensor_residency_role == IR_TENSOR_RESIDENCY_START) {
      snprintf(residency, sizeof(residency), " residency.%sstart#%u", scope,
               instruction->tensor_residency_id);
    } else if (instruction->tensor_residency_role ==
               IR_TENSOR_RESIDENCY_UPDATE) {
      snprintf(residency, sizeof(residency), " residency.%supdate#%u", scope,
               instruction->tensor_residency_id);
    }
    written = snprintf(
        buffer, buffer_size,
        "tensor_mma x%llu%s m%un%uk%u fmt(%d,%d,%d,%d) layout(%d,%d,%d,%d) ld(%u,%u,%u,%u) packing(%d,%d) sparsity(%d) scale(%d:%d:%u,%d:%d:%u)",
        (unsigned long long)ir_tensor_mma_instruction_count(instruction),
        residency,
        (unsigned)IR_TENSOR_MMA(instruction).m,
        (unsigned)IR_TENSOR_MMA(instruction).n,
        (unsigned)IR_TENSOR_MMA(instruction).k,
        (int)IR_TENSOR_MMA(instruction).a_element,
        (int)IR_TENSOR_MMA(instruction).b_element,
        (int)IR_TENSOR_MMA(instruction).accumulator_element,
        (int)IR_TENSOR_MMA(instruction).result_element,
        (int)IR_TENSOR_MMA(instruction).a_layout,
        (int)IR_TENSOR_MMA(instruction).b_layout,
        (int)IR_TENSOR_MMA(instruction).c_layout,
        (int)IR_TENSOR_MMA(instruction).d_layout,
        (unsigned)IR_TENSOR_MMA(instruction).a_leading_dimension,
        (unsigned)IR_TENSOR_MMA(instruction).b_leading_dimension,
        (unsigned)IR_TENSOR_MMA(instruction).c_leading_dimension,
        (unsigned)IR_TENSOR_MMA(instruction).d_leading_dimension,
        (int)IR_TENSOR_MMA(instruction).a_packing,
        (int)IR_TENSOR_MMA(instruction).b_packing,
        (int)IR_TENSOR_MMA(instruction).sparsity,
        (int)IR_TENSOR_MMA(instruction).a_scale_mode,
        (int)IR_TENSOR_MMA(instruction).a_scale_element,
        (unsigned)IR_TENSOR_MMA(instruction).a_scale_leading_dimension,
        (int)IR_TENSOR_MMA(instruction).b_scale_mode,
        (int)IR_TENSOR_MMA(instruction).b_scale_element,
        (unsigned)IR_TENSOR_MMA(instruction).b_scale_leading_dimension);
    break;
    }
  case IR_OP_TENSOR_MATMUL:
    written = snprintf(
        buffer, buffer_size,
        "tensor_matmul region=m%un%u k_chunk=%u fmt(%d,%d,%d,%d) layout(%d,%d,%d,%d) ld(%u,%u,%u,%u) packing(%d,%d) sparsity=%d scope=%s",
        (unsigned)IR_TENSOR_MMA(instruction).m,
        (unsigned)IR_TENSOR_MMA(instruction).n,
        (unsigned)IR_TENSOR_MMA(instruction).k,
        (int)IR_TENSOR_MMA(instruction).a_element,
        (int)IR_TENSOR_MMA(instruction).b_element,
        (int)IR_TENSOR_MMA(instruction).accumulator_element,
        (int)IR_TENSOR_MMA(instruction).result_element,
        (int)IR_TENSOR_MMA(instruction).a_layout,
        (int)IR_TENSOR_MMA(instruction).b_layout,
        (int)IR_TENSOR_MMA(instruction).c_layout,
        (int)IR_TENSOR_MMA(instruction).d_layout,
        (unsigned)IR_TENSOR_MMA(instruction).a_leading_dimension,
        (unsigned)IR_TENSOR_MMA(instruction).b_leading_dimension,
        (unsigned)IR_TENSOR_MMA(instruction).c_leading_dimension,
        (unsigned)IR_TENSOR_MMA(instruction).d_leading_dimension,
        (int)IR_TENSOR_MMA(instruction).a_packing,
        (int)IR_TENSOR_MMA(instruction).b_packing,
        (int)IR_TENSOR_MMA(instruction).sparsity,
        IR_TENSOR_MMA(instruction).scope == MTLC_MEMORY_SCOPE_WORKGROUP
            ? "workgroup"
            : "subgroup");
    break;
  case IR_OP_TENSOR_EPILOGUE:
    written = snprintf(
        buffer, buffer_size,
        "tensor_epilogue m%un%u element=%d layout=%d ld=%u bias=%d:%d:%u activation=%d scale=(%u,%u) scope=%s",
        (unsigned)IR_TENSOR_EPILOGUE(instruction).m,
        (unsigned)IR_TENSOR_EPILOGUE(instruction).n,
        (int)IR_TENSOR_EPILOGUE(instruction).element,
        (int)IR_TENSOR_EPILOGUE(instruction).layout,
        (unsigned)IR_TENSOR_EPILOGUE(instruction).leading_dimension,
        (int)IR_TENSOR_EPILOGUE(instruction).bias_mode,
        (int)IR_TENSOR_EPILOGUE(instruction).bias_layout,
        (unsigned)IR_TENSOR_EPILOGUE(instruction).bias_leading_dimension,
        (int)IR_TENSOR_EPILOGUE(instruction).activation,
        (unsigned)IR_TENSOR_EPILOGUE(instruction).scale_output,
        (unsigned)IR_TENSOR_EPILOGUE(instruction).scale_bias,
        IR_TENSOR_EPILOGUE(instruction).scope == MTLC_MEMORY_SCOPE_WORKGROUP
            ? "workgroup"
            : "subgroup");
    break;
  case IR_OP_TENSOR_COMMIT:
    written = snprintf(buffer, buffer_size,
                       "tensor_commit residency.%scommit#%u m%un%uk%u",
                       instruction->tensor_residency_scope ==
                               IR_TENSOR_RESIDENCY_SCOPE_PIPELINE
                           ? "pipeline."
                           : instruction->tensor_residency_scope ==
                                     IR_TENSOR_RESIDENCY_SCOPE_LOOP
                                 ? "loop."
                                 : "",
                       instruction->tensor_residency_id,
                       (unsigned)IR_TENSOR_MMA(instruction).m,
                       (unsigned)IR_TENSOR_MMA(instruction).n,
                       (unsigned)IR_TENSOR_MMA(instruction).k);
    break;
  default:
    *handled = 0;
    break;
  }
  return written;
}

static int ir_format_simd_int_line(const IRInstruction *instruction,
                             const char *dest, const char *lhs,
                             const char *rhs, char *buffer,
                             size_t buffer_size, int *handled) {
  int written = 0;
  (void)dest;
  (void)lhs;
  (void)rhs;
  *handled = 1;
  switch (instruction->op) {
  case IR_OP_MEMCPY_INLINE:
    written = snprintf(buffer, buffer_size, "%s = memcpy_inline %s, %s", dest,
                       lhs, rhs);
    break;
  case IR_OP_SIMD_SUM_I32:
    written = snprintf(buffer, buffer_size, "%s += simd_sum_i32(base=%s, len=%s)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_SUM_U8:
    written = snprintf(buffer, buffer_size, "%s += simd_sum_u8(base=%s, len=%s)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_BYTE_MAP:
    written = snprintf(buffer, buffer_size,
                       "simd_byte_map(base=%s, len=%s, steps=%zu)", lhs, rhs,
                       instruction->argument_count / 2);
    break;
  case IR_OP_SIMD_FILL: {
    char value[128];
    char start[128];
    char offset[128];
    int pointer_walk = instruction->argument_count > 1 &&
                       instruction->arguments[1].int_value == 1;
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      value, sizeof(value));
    ir_format_operand(instruction->argument_count > 3 ? &instruction->arguments[3]
                                                      : NULL,
                      start, sizeof(start));
    ir_format_operand(instruction->argument_count > 4 ? &instruction->arguments[4]
                                                      : NULL,
                      offset, sizeof(offset));
    written = snprintf(
        buffer, buffer_size,
        "simd_fill(%s=%s, %s=%s, size=%lld, value=%s, start=%s, offset=%s)",
        pointer_walk ? "begin" : "base", lhs, pointer_walk ? "end" : "len", rhs,
        instruction->argument_count > 0 ? instruction->arguments[0].int_value
                                        : 0,
        value, start, offset);
    break;
  }
  case IR_OP_SIMD_MATMUL_N32:
    written = snprintf(buffer, buffer_size, "%s = matmul_n32(c=%s, a=%s, b=%s)",
                       dest, dest, lhs, rhs);
    break;
  case IR_OP_SIMD_INSERTION_SORT_I32:
    written = snprintf(buffer, buffer_size, "simd_insertion_sort_i32(base=%s, len=%s)",
                       dest, rhs);
    break;
  case IR_OP_SIMD_DOT_I32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "%s = dot_i32(a=%s, b=%s, len=%s)",
                       dest, lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_DOT_I8: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "%s = dot_i8(a=%s, b=%s, len=%s)",
                       dest, lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_SLP_MAC_I32:
    written = snprintf(buffer, buffer_size,
                       "slp_mac_i32(out=%s, a=%s, b=%s, K/n/aoff/boff/str/ooff)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_SLP_MAC_I8:
    written = snprintf(buffer, buffer_size,
                       "slp_mac_i8(out=%s, a=%s, b=%s, K/n/aoff/boff/str/ooff)",
                       dest, lhs, rhs);
    break;
  case IR_OP_SIMD_SCALE_I32: {
    char len[128], mul[128], add[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    ir_format_operand(instruction->argument_count > 1 ? &instruction->arguments[1]
                                                      : NULL,
                      mul, sizeof(mul));
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      add, sizeof(add));
    written = snprintf(buffer, buffer_size,
                       "%s = scale_i32(src=%s, dst=%s, len=%s, mul=%s, add=%s)",
                       dest, lhs, rhs, len, mul, add);
    break;
  }
  case IR_OP_SIMD_CLAMP_I32: {
    char len[128], lo[128], hi[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    ir_format_operand(instruction->argument_count > 1 ? &instruction->arguments[1]
                                                      : NULL,
                      lo, sizeof(lo));
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      hi, sizeof(hi));
    written = snprintf(buffer, buffer_size,
                       "%s = clamp_i32(src=%s, dst=%s, len=%s, lo=%s, hi=%s)",
                       dest, lhs, rhs, len, lo, hi);
    break;
  }
  case IR_OP_SIMD_REVERSE_COPY_I32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size,
                       "%s = reverse_copy_i32(src=%s, dst=%s, len=%s)", dest,
                       lhs, rhs, len);
    break;
  }
  case IR_OP_LOWER_BOUND_I32: {
    char key[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      key, sizeof(key));
    written = snprintf(buffer, buffer_size,
                       "%s = lower_bound_i32(arr=%s, n=%s, key=%s)", dest, lhs,
                       rhs, key);
    break;
  }
  case IR_OP_PREFIX_SUM_I32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size,
                       "%s = prefix_sum_i32(src=%s, dst=%s, len=%s)", dest,
                       lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_MINMAX_I32: {
    char maxv[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      maxv, sizeof(maxv));
    written = snprintf(buffer, buffer_size,
                       "%s = minmax_i32(arr=%s, n=%s, max=%s)", dest, lhs, rhs,
                       maxv);
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return written;
}

static int ir_format_simd_float_line(const IRInstruction *instruction,
                             const char *dest, const char *lhs,
                             const char *rhs, char *buffer,
                             size_t buffer_size, int *handled) {
  int written = 0;
  (void)dest;
  (void)lhs;
  (void)rhs;
  *handled = 1;
  switch (instruction->op) {
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
    written = snprintf(buffer, buffer_size, "%s += %s(base=%s, len=%s)", dest,
                       ir_opcode_name(instruction->op), lhs, rhs);
    break;
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "%s += %s(a=%s, b=%s, len=%s)", dest,
                       ir_opcode_name(instruction->op), lhs, rhs, len);
    break;
  }
  case IR_OP_SIMD_I2F_REDUCE_F64: {
    char trip[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      trip, sizeof(trip));
    written = snprintf(buffer, buffer_size, "%s += %s(n=%s, steps=%zu)", dest,
                       ir_opcode_name(instruction->op), trip,
                       instruction->argument_count > 0
                           ? (instruction->argument_count - 1) / 2
                           : 0);
    break;
  }
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32: {
    long long reduce_op = (instruction->argument_count > 0 &&
                           instruction->arguments[0].kind == IR_OPERAND_INT)
                              ? instruction->arguments[0].int_value
                              : -1;
    long long n_nodes = (instruction->argument_count > 2 &&
                         instruction->arguments[2].kind == IR_OPERAND_INT)
                            ? instruction->arguments[2].int_value
                            : -1;
    written = snprintf(buffer, buffer_size, "%s %s %s(%s nodes=%lld)", dest,
                       reduce_op >= 1 ? "+=" : "<-",
                       ir_opcode_name(instruction->op),
                       reduce_op == 1   ? "reduce"
                       : reduce_op == 2 ? "max"
                       : reduce_op == 3 ? "min"
                                        : "map",
                       n_nodes);
    break;
  }
  case IR_OP_SIMD_FIND: {
    char base[128];
    char start[128];
    long long pred = instruction->argument_count > 0
                         ? instruction->arguments[0].int_value
                         : -1;
    ir_format_operand(&instruction->rhs, base, sizeof(base));
    if (instruction->argument_count > 4) {
      ir_format_operand(&instruction->arguments[4], start, sizeof(start));
    } else {
      snprintf(start, sizeof(start), "0");
    }
    written = snprintf(buffer, buffer_size,
                       "%s <- %s(a=%s, n=%s, pred=%lld, start=%s)", dest,
                       ir_opcode_name(instruction->op), base, lhs, pred, start);
    break;
  }
  case IR_OP_SIMD_OUTER_LANE_F64:
    written = snprintf(buffer, buffer_size, "%s += %s(outerP=%s, args=%zu)", dest,
                       ir_opcode_name(instruction->op), lhs,
                       instruction->argument_count);
    break;
  case IR_OP_SIMD_EXP_F32: {
    char len[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    written = snprintf(buffer, buffer_size, "exp_f32(a=%s, len=%s)", dest, len);
    break;
  }
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32: {
    char len[128];
    char src_scale[128];
    char dst_scale[128];
    char bias[128];
    ir_format_operand(instruction->argument_count > 0 ? &instruction->arguments[0]
                                                      : NULL,
                      len, sizeof(len));
    ir_format_operand(instruction->argument_count > 1 ? &instruction->arguments[1]
                                                      : NULL,
                      src_scale, sizeof(src_scale));
    ir_format_operand(instruction->argument_count > 2 ? &instruction->arguments[2]
                                                      : NULL,
                      dst_scale, sizeof(dst_scale));
    ir_format_operand(instruction->argument_count > 3 ? &instruction->arguments[3]
                                                      : NULL,
                      bias, sizeof(bias));
    written = snprintf(buffer, buffer_size,
                       "%s = %s(src=%s, dst=%s, len=%s, a=%s, b=%s, c=%s)",
                       dest, ir_opcode_name(instruction->op), lhs, rhs, len,
                       src_scale, dst_scale, bias);
    break;
  }
  default:
    *handled = 0;
    break;
  }
  return written;
}

static int ir_format_instruction_line(const IRInstruction *instruction,
                                      char *buffer, size_t buffer_size) {
  char dest[IR_OPERAND_FMT_BUFSIZE];
  char lhs[IR_OPERAND_FMT_BUFSIZE];
  char rhs[IR_OPERAND_FMT_BUFSIZE];
  int handled = 0;
  int written;

  if (!instruction || !buffer || buffer_size == 0) {
    return 0;
  }

  ir_format_operand(&instruction->dest, dest, sizeof(dest));
  ir_format_operand(&instruction->lhs, lhs, sizeof(lhs));
  ir_format_operand(&instruction->rhs, rhs, sizeof(rhs));

  written = ir_format_control_line(instruction, dest, lhs, rhs, buffer,
                                   buffer_size, &handled);
  if (!handled) {
    written = ir_format_value_line(instruction, dest, lhs, rhs, buffer,
                                   buffer_size, &handled);
  }
  if (!handled) {
    written = ir_format_call_line(instruction, dest, lhs, rhs, buffer,
                                  buffer_size, &handled);
  }
  if (!handled) {
    written = ir_format_gpu_line(instruction, dest, lhs, rhs, buffer,
                                 buffer_size, &handled);
  }
  if (!handled) {
    written = ir_format_simd_int_line(instruction, dest, lhs, rhs, buffer,
                                     buffer_size, &handled);
  }
  if (!handled) {
    written = ir_format_simd_float_line(instruction, dest, lhs, rhs, buffer,
                                        buffer_size, &handled);
  }
  if (!handled) {
    written =
        snprintf(buffer, buffer_size, "%s", ir_opcode_name(instruction->op));
  }

  if (instruction->is_volatile && instruction->op != IR_OP_LOAD &&
      instruction->op != IR_OP_STORE && written > 0 &&
      (size_t)written + 10 < buffer_size) {
    written += snprintf(buffer + written, buffer_size - (size_t)written,
                        " volatile");
  }

  return written > 0 && (size_t)written < buffer_size;
}

int ir_instruction_dump(const IRInstruction *instruction,
                        char *buffer, size_t capacity) {
  if (!instruction || !buffer || capacity == 0) {
    return 0;
  }
  return ir_format_instruction_line(instruction, buffer, capacity);
}

static void ir_dump_block_edges(FILE *output, const char *label,
                                const size_t *items, size_t count) {
  fprintf(output, " %s:", label);
  if (count == 0) {
    fprintf(output, " -");
    return;
  }

  for (size_t i = 0; i < count; i++) {
    fprintf(output, "%s%zu", i == 0 ? " " : ",", items[i]);
  }
}

/* Replace insn->text with a copy of `name`, freeing the old text. */
static int ir_retarget_call(IRInstruction *insn, const char *name) {
  char *copy = mettle_strdup(name);
  if (!copy) {
    return 0;
  }
  mettle_free_string(insn->text);
  insn->text = copy;
  return 1;
}

int ir_program_route_to_native_heap(IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    /* Never rewrite inside the allocator shims themselves. They bottom out on
     * the OS page layer (never malloc/free/new), so no rewrite is needed
     * today; the guard makes that invariant robust against a future shim edit
     * that would otherwise turn a literal free or malloc call into
     * self-recursion. The "mettle_heap_" prefix is distinctive enough not to
     * collide with user code, and the allocator core calls no allocation
     * surface either, so it needs no guard. */
    if (fn->name && strncmp(fn->name, "mettle_heap_", 12) == 0) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *insn = &fn->instructions[i];

      if (insn->op == IR_OP_NEW) {
        /* new T  ->  mettle_heap_zeroed(sizeof T). The size is in rhs. An
         * absent or non-positive constant size means a default object (8
         * bytes), the same fallback the backend's original new lowering used.
         * Any other operand (the normal case is an INT constant; TEMP/SYMBOL
         * are copied faithfully, duplicating an owned name) is forwarded. */
        IROperand size_arg;
        memset(&size_arg, 0, sizeof(size_arg));
        if (insn->rhs.kind == IR_OPERAND_NONE ||
            (insn->rhs.kind == IR_OPERAND_INT && insn->rhs.int_value <= 0)) {
          size_arg.kind = IR_OPERAND_INT;
          size_arg.int_value = 8;
        } else {
          size_arg = insn->rhs;
          if (insn->rhs.name) {
            size_arg.name = mettle_strdup(insn->rhs.name);
            if (!size_arg.name) {
              return 0;
            }
          }
        }

        IROperand *args = (IROperand *)calloc(1, sizeof(IROperand));
        char *callee = mettle_strdup("mettle_heap_zeroed");
        if (!args || !callee) {
          free(args);
          free(callee);
          return 0;
        }
        args[0] = size_arg;
        mettle_free_string(insn->text);
        insn->text = callee;
        insn->arguments = args;
        insn->argument_count = 1;
        insn->op = IR_OP_CALL;
        /* rhs ownership moved into args[0] (TEMP name re-duplicated above), so
         * detach it from the instruction to avoid a double free. */
        memset(&insn->rhs, 0, sizeof(insn->rhs));
        insn->rhs.kind = IR_OPERAND_NONE;
        continue;
      }

      if (insn->op == IR_OP_CALL && insn->text) {
        if (strcmp(insn->text, "malloc") == 0 && insn->argument_count == 1) {
          if (!ir_retarget_call(insn, "mettle_heap_alloc")) return 0;
        } else if (strcmp(insn->text, "calloc") == 0 &&
                   insn->argument_count == 2) {
          if (!ir_retarget_call(insn, "mettle_heap_calloc")) return 0;
        } else if (strcmp(insn->text, "realloc") == 0 &&
                   insn->argument_count == 2) {
          if (!ir_retarget_call(insn, "mettle_heap_realloc")) return 0;
        } else if (strcmp(insn->text, "free") == 0 &&
                   insn->argument_count == 1) {
          if (!ir_retarget_call(insn, "mettle_heap_free")) return 0;
        }
      }
    }
    /* Calls were added/retyped; the cached CFG (if any) is unaffected in shape,
     * but mark it stale so any later consumer rebuilds rather than trusting
     * per-op metadata. */
    ir_function_clear_cfg(fn);
  }
  return 1;
}

static void ir_destroy_instruction_array(IRInstruction *instructions,
                                         size_t count) {
  if (!instructions) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    ir_instruction_destroy(&instructions[i]);
  }
  free(instructions);
}

static const char *ir_gpu_launch_type_name(const MtlcType *type) {
  if (!type) {
    return NULL;
  }
  if (type->name) {
    return type->name;
  }
  /* Keep the core IR object self-contained.  Some diagnostic harnesses link
   * ir.c without the public type factory, and launch lowering needs only the
   * spelling already carried by scalar descriptors. */
  switch (type->kind) {
  case MTLC_TYPE_VOID: return "void";
  case MTLC_TYPE_INT8: return "int8";
  case MTLC_TYPE_INT16: return "int16";
  case MTLC_TYPE_INT32: return "int32";
  case MTLC_TYPE_INT64: return "int64";
  case MTLC_TYPE_UINT8: return "uint8";
  case MTLC_TYPE_UINT16: return "uint16";
  case MTLC_TYPE_UINT32: return "uint32";
  case MTLC_TYPE_UINT64: return "uint64";
  case MTLC_TYPE_BOOL: return "bool";
  case MTLC_TYPE_FLOAT32: return "float32";
  case MTLC_TYPE_FLOAT64: return "float64";
  case MTLC_TYPE_FLOAT16: return "float16";
  case MTLC_TYPE_BFLOAT16: return "bfloat16";
  case MTLC_TYPE_STRING: return "string";
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_FUNCTION_POINTER:
  case MTLC_TYPE_STRUCT:
  case MTLC_TYPE_ENUM:
  case MTLC_TYPE_TAGGED_ENUM:
    return NULL;
  }
  return NULL;
}

static MtlcType *ir_gpu_launch_params_type(IRProgram *program, size_t count) {
  char name[40];
  MtlcType *existing;
  MtlcType *base;
  MtlcType *type;
  snprintf(name, sizeof(name), "int64[%zu]", count);
  existing = ir_program_lookup_type(program, name);
  if (existing) {
    return existing;
  }
  base = ir_program_lookup_type(program, "int64");
  if (!base) {
    return NULL;
  }
  if (program->owned_type_count == program->owned_type_capacity) {
    size_t next = program->owned_type_capacity
                      ? program->owned_type_capacity * 2
                      : 4;
    MtlcType **grown =
        realloc(program->owned_types, next * sizeof(*program->owned_types));
    if (!grown) {
      return NULL;
    }
    program->owned_types = grown;
    program->owned_type_capacity = next;
  }
  type = calloc(1, sizeof(*type));
  if (!type) {
    return NULL;
  }
  type->name = mettle_strdup(name);
  if (!type->name) {
    free(type);
    return NULL;
  }
  type->kind = MTLC_TYPE_ARRAY;
  type->base_type = base;
  type->array_size = count;
  type->size = base->size * count;
  type->alignment = base->alignment;
  if (!ir_program_register_type(program, name, type)) {
    free((char *)type->name);
    free(type);
    return NULL;
  }
  program->owned_types[program->owned_type_count++] = type;
  return type;
}

/* Register `T*` for each scalar T, so a pass that needs to name a pointer type
 * has one whether or not the source ever spelled it. Optimizer passes see the
 * program only through its functions, and a local they declare must carry a
 * type string the backend can resolve to a stack slot. */
int ir_program_register_scalar_pointer_types(IRProgram *program) {
  static const char *kElements[] = {"int8",    "uint8",   "int16",  "uint16",
                                    "int32",   "uint32",  "int64",  "uint64",
                                    "float32", "float64", "float16", "bfloat16"};
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(kElements) / sizeof(kElements[0]); i++) {
    char name[32];
    MtlcType *base = NULL;
    MtlcType *type = NULL;
    snprintf(name, sizeof(name), "%s*", kElements[i]);
    if (ir_program_lookup_type(program, name)) {
      continue;
    }
    base = ir_program_lookup_type(program, kElements[i]);
    if (!base) {
      continue;
    }
    if (program->owned_type_count == program->owned_type_capacity) {
      size_t next = program->owned_type_capacity
                        ? program->owned_type_capacity * 2
                        : 8;
      MtlcType **grown =
          realloc(program->owned_types, next * sizeof(*program->owned_types));
      if (!grown) {
        return 0;
      }
      program->owned_types = grown;
      program->owned_type_capacity = next;
    }
    type = calloc(1, sizeof(*type));
    if (!type) {
      return 0;
    }
    type->name = mettle_strdup(name);
    if (!type->name) {
      free(type);
      return 0;
    }
    type->kind = MTLC_TYPE_POINTER;
    type->base_type = base;
    type->size = 8;
    type->alignment = 8;
    if (!ir_program_register_type(program, name, type)) {
      free((char *)type->name);
      free(type);
      return 0;
    }
    program->owned_types[program->owned_type_count++] = type;
  }
  return 1;
}

static int ir_gpu_launch_append_local(IRFunction *out, const char *name,
                                      MtlcType *type,
                                      const IROperand *value,
                                      SourceLocation location) {
  IRInstruction declaration = {0};
  IRInstruction assign = {0};
  int ok;
  declaration.op = IR_OP_DECLARE_LOCAL;
  declaration.location = location;
  declaration.dest = ir_operand_symbol(name);
  declaration.text = (char *)ir_gpu_launch_type_name(type);
  declaration.value_type = type;
  if (!declaration.dest.name || !declaration.text) {
    ir_operand_destroy(&declaration.dest);
    return 0;
  }
  ok = ir_function_append_instruction(out, &declaration);
  ir_operand_destroy(&declaration.dest);
  if (!ok) {
    return 0;
  }

  assign.op = IR_OP_ASSIGN;
  assign.location = location;
  assign.dest = ir_operand_symbol(name);
  assign.lhs = *value;
  if (type && (type->kind == MTLC_TYPE_FLOAT32 ||
               type->kind == MTLC_TYPE_FLOAT64 ||
               type->kind == MTLC_TYPE_FLOAT16 ||
               type->kind == MTLC_TYPE_BFLOAT16)) {
    assign.is_float = 1;
    assign.float_bits = 32;
  }
  if (!assign.dest.name) {
    return 0;
  }
  ok = ir_function_append_instruction(out, &assign);
  ir_operand_destroy(&assign.dest);
  return ok;
}

static int ir_gpu_launch_append_expansion(IRProgram *program, IRFunction *out,
                                          const IRInstruction *launch,
                                          size_t launch_id) {
  const size_t controls = IR_GPU_LAUNCH_CONTROL_ARGS;
  const size_t nargs = launch->argument_count >= controls
                           ? launch->argument_count - controls
                           : 0;
  char params_name[80];
  char params_type[40];
  char params_base_name[80];
  MtlcType *params_array_type = NULL;

  if (!out || !launch || launch->op != IR_OP_GPU_LAUNCH ||
      launch->lhs.kind == IR_OPERAND_NONE ||
      launch->argument_count < controls || !launch->arguments ||
      (nargs > 0 && !launch->argument_types)) {
    return 0;
  }
  for (size_t i = 0; i < nargs; i++) {
    if (!launch->argument_types[controls + i]) {
      return 0;
    }
  }

  snprintf(params_name, sizeof(params_name), ".__mtlc_gpu%zu_params",
           launch_id);
  snprintf(params_base_name, sizeof(params_base_name),
           ".__mtlc_gpu%zu_params_base", launch_id);

  /* Materialize every kernel argument in exact typed storage. The runtime ABI
   * receives pointers to these cells, so narrow integers and float32 retain
   * their natural widths on both x86-64 and AArch64. */
  for (size_t i = 0; i < nargs; i++) {
    char arg_name[80];
    snprintf(arg_name, sizeof(arg_name), ".__mtlc_gpu%zu_arg%zu", launch_id,
             i);
    if (!ir_gpu_launch_append_local(
            out, arg_name, launch->argument_types[controls + i],
            &launch->arguments[controls + i], launch->location)) {
      return 0;
    }
  }

  if (nargs > 0) {
    IRInstruction params_decl = {0};
    IRInstruction params_base = {0};
    snprintf(params_type, sizeof(params_type), "int64[%zu]", nargs);
    params_array_type = ir_gpu_launch_params_type(program, nargs);
    if (!params_array_type) {
      return 0;
    }
    params_decl.op = IR_OP_DECLARE_LOCAL;
    params_decl.location = launch->location;
    params_decl.dest = ir_operand_symbol(params_name);
    params_decl.text = params_type;
    params_decl.value_type = params_array_type;
    if (!params_decl.dest.name ||
        !ir_function_append_instruction(out, &params_decl)) {
      ir_operand_destroy(&params_decl.dest);
      return 0;
    }
    ir_operand_destroy(&params_decl.dest);

    params_base.op = IR_OP_ADDRESS_OF;
    params_base.location = launch->location;
    params_base.dest = ir_operand_temp(params_base_name);
    params_base.lhs = ir_operand_symbol(params_name);
    if (!params_base.dest.name || !params_base.lhs.name ||
        !ir_function_append_instruction(out, &params_base)) {
      ir_operand_destroy(&params_base.dest);
      ir_operand_destroy(&params_base.lhs);
      return 0;
    }
    ir_operand_destroy(&params_base.dest);
    ir_operand_destroy(&params_base.lhs);

    for (size_t i = 0; i < nargs; i++) {
      char arg_name[80];
      char arg_addr_name[80];
      char slot_name[80];
      IRInstruction arg_addr = {0};
      IRInstruction slot = {0};
      IRInstruction store = {0};
      snprintf(arg_name, sizeof(arg_name), ".__mtlc_gpu%zu_arg%zu",
               launch_id, i);
      snprintf(arg_addr_name, sizeof(arg_addr_name),
               ".__mtlc_gpu%zu_arg%zu_addr", launch_id, i);
      snprintf(slot_name, sizeof(slot_name), ".__mtlc_gpu%zu_slot%zu",
               launch_id, i);

      arg_addr.op = IR_OP_ADDRESS_OF;
      arg_addr.location = launch->location;
      arg_addr.dest = ir_operand_temp(arg_addr_name);
      arg_addr.lhs = ir_operand_symbol(arg_name);
      if (!arg_addr.dest.name || !arg_addr.lhs.name ||
          !ir_function_append_instruction(out, &arg_addr)) {
        ir_operand_destroy(&arg_addr.dest);
        ir_operand_destroy(&arg_addr.lhs);
        return 0;
      }
      ir_operand_destroy(&arg_addr.dest);
      ir_operand_destroy(&arg_addr.lhs);

      slot.op = IR_OP_BINARY;
      slot.location = launch->location;
      slot.dest = ir_operand_temp(slot_name);
      slot.lhs = ir_operand_temp(params_base_name);
      slot.rhs = ir_operand_int((long long)(i * 8u));
      slot.text = "+";
      if (!slot.dest.name || !slot.lhs.name ||
          !ir_function_append_instruction(out, &slot)) {
        ir_operand_destroy(&slot.dest);
        ir_operand_destroy(&slot.lhs);
        return 0;
      }
      ir_operand_destroy(&slot.dest);
      ir_operand_destroy(&slot.lhs);

      store.op = IR_OP_STORE;
      store.location = launch->location;
      store.dest = ir_operand_temp(slot_name);
      store.lhs = ir_operand_temp(arg_addr_name);
      store.rhs = ir_operand_int(8);
      if (!store.dest.name || !store.lhs.name ||
          !ir_function_append_instruction(out, &store)) {
        ir_operand_destroy(&store.dest);
        ir_operand_destroy(&store.lhs);
        return 0;
      }
      ir_operand_destroy(&store.dest);
      ir_operand_destroy(&store.lhs);
    }
  }

  {
    IRInstruction call = {0};
    IROperand call_args[11] = {0};
    call_args[0] = launch->lhs;
    for (size_t i = 0; i < controls; i++) {
      call_args[1 + i] = launch->arguments[i];
    }
    call_args[9] = nargs > 0 ? ir_operand_temp(params_base_name)
                             : ir_operand_int(0);
    call_args[10] = ir_operand_int((long long)nargs);
    call.op = IR_OP_CALL;
    call.location = launch->location;
    call.text = "mtlc_gpu_launch_checked";
    call.arguments = call_args;
    call.argument_count = sizeof(call_args) / sizeof(call_args[0]);
    if ((nargs > 0 && !call_args[9].name) ||
        !ir_function_append_instruction(out, &call)) {
      ir_operand_destroy(&call_args[9]);
      return 0;
    }
    ir_operand_destroy(&call_args[9]);
  }
  return 1;
}

int ir_program_lower_gpu_launches(IRProgram *program) {
  size_t launch_id = 0;
  if (!program) {
    return 0;
  }
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    int has_launch = 0;
    if (!fn) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      if (fn->instructions[i].op == IR_OP_GPU_LAUNCH) {
        has_launch = 1;
        break;
      }
    }
    if (!has_launch) {
      continue;
    }

    IRFunction expanded = {0};
    for (size_t i = 0; i < fn->instruction_count; i++) {
      IRInstruction *instruction = &fn->instructions[i];
      int ok = instruction->op == IR_OP_GPU_LAUNCH
                   ? ir_gpu_launch_append_expansion(program, &expanded,
                                                    instruction, launch_id++)
                   : ir_function_append_instruction(&expanded, instruction);
      if (!ok) {
        ir_destroy_instruction_array(expanded.instructions,
                                     expanded.instruction_count);
        ir_function_clear_cfg(&expanded);
        return 0;
      }
    }

    ir_function_clear_cfg(fn);
    ir_destroy_instruction_array(fn->instructions, fn->instruction_count);
    fn->instructions = expanded.instructions;
    fn->instruction_count = expanded.instruction_count;
    fn->instruction_capacity = expanded.instruction_capacity;
    fn->cfg_valid = 0;
  }
  return 1;
}

typedef struct {
  const IRProgram *program;
  IRGpuCallGraph *graph;
  unsigned char *state; /* 0 unseen, 1 active, 2 complete */
  char **error;
} IRGpuGraphBuilder;

static int ir_gpu_graph_fail(IRGpuGraphBuilder *builder, const char *fmt, ...) {
  char buffer[512];
  va_list ap;
  if (builder && builder->error && !*builder->error) {
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    *builder->error = mettle_strdup(buffer);
  }
  return 0;
}

static long ir_gpu_graph_function_index(const IRProgram *program,
                                        const char *name) {
  if (!program || !name) return -1;
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->name &&
        strcmp(program->functions[i]->name, name) == 0) {
      return (long)i;
    }
  }
  return -1;
}

static int ir_gpu_subgroup_signature_matches(const IRProgram *program,
                                             const IRInstruction *instruction) {
  const IRModuleSymbol *symbol =
      ir_program_lookup_symbol(program, instruction->text);
  MtlcTypeKind result_kind =
      ir_intrinsic_subgroup_result_kind(instruction->intrinsic);
  const MtlcType *result_type = instruction->value_type;
  if (!result_type && symbol && symbol->kind == IR_MODSYM_FUNCTION) {
    result_type = symbol->return_type;
  }
  if (!result_type || result_type->kind != result_kind) return 0;
  if (!symbol || symbol->kind != IR_MODSYM_FUNCTION) return 1;
  if (symbol->param_count != instruction->argument_count) return 0;
  switch (instruction->intrinsic) {
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SIZE:
    return symbol->param_count == 0;
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32:
    return symbol->param_types && symbol->param_count == 2 &&
           symbol->param_types[0] &&
           symbol->param_types[0]->kind == MTLC_TYPE_UINT32 &&
           symbol->param_types[1] &&
           symbol->param_types[1]->kind == MTLC_TYPE_UINT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32:
    return symbol->param_types && symbol->param_count == 2 &&
           symbol->param_types[0] &&
           symbol->param_types[0]->kind == MTLC_TYPE_FLOAT32 &&
           symbol->param_types[1] &&
           symbol->param_types[1]->kind == MTLC_TYPE_UINT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32:
    return symbol->param_types && symbol->param_count == 1 &&
           symbol->param_types[0] &&
           symbol->param_types[0]->kind == MTLC_TYPE_UINT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
    return symbol->param_types && symbol->param_count == 1 &&
           symbol->param_types[0] &&
           symbol->param_types[0]->kind == MTLC_TYPE_FLOAT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD:
    return symbol->param_types && symbol->param_count == 2 &&
           symbol->param_types[0] &&
           symbol->param_types[0]->kind == MTLC_TYPE_BOOL &&
           symbol->param_types[1] &&
           symbol->param_types[1]->kind == MTLC_TYPE_UINT32;
  case MTLC_INTRINSIC_GPU_SUBGROUP_ANY:
  case MTLC_INTRINSIC_GPU_SUBGROUP_ALL:
    return symbol->param_types && symbol->param_count == 1 &&
           symbol->param_types[0] &&
           symbol->param_types[0]->kind == MTLC_TYPE_BOOL;
  default:
    return 0;
  }
}

static const MtlcType *ir_gpu_operand_type(const IRProgram *program,
                                           const IRFunction *function,
                                           const IRInstruction *instruction,
                                           size_t argument_index) {
  const IROperand *operand;
  const IRModuleSymbol *function_symbol;
  if (!program || !function || !instruction ||
      argument_index >= instruction->argument_count) {
    return NULL;
  }
  if (instruction->argument_types &&
      instruction->argument_types[argument_index]) {
    return instruction->argument_types[argument_index];
  }
  operand = &instruction->arguments[argument_index];
  if ((operand->kind != IR_OPERAND_SYMBOL &&
       operand->kind != IR_OPERAND_TEMP) ||
      !operand->name) {
    return NULL;
  }
  function_symbol = ir_program_lookup_symbol(program, function->name);
  if (operand->kind == IR_OPERAND_SYMBOL && function_symbol &&
      function_symbol->kind == IR_MODSYM_FUNCTION) {
    for (size_t p = 0; p < function->parameter_count &&
                       p < function_symbol->param_count;
         p++) {
      if (function->parameter_names && function->parameter_names[p] &&
          strcmp(function->parameter_names[p], operand->name) == 0) {
        return function_symbol->param_types
                   ? function_symbol->param_types[p]
                   : NULL;
      }
    }
  }
  for (size_t i = function->instruction_count; i > 0; i--) {
    const IRInstruction *producer = &function->instructions[i - 1];
    if (producer->dest.name &&
        strcmp(producer->dest.name, operand->name) == 0 &&
        producer->value_type) {
      return producer->value_type;
    }
  }
  if (operand->kind == IR_OPERAND_SYMBOL) {
    const IRModuleSymbol *symbol = ir_program_lookup_symbol(program, operand->name);
    if (symbol && symbol->kind == IR_MODSYM_VARIABLE) return symbol->type;
  }
  return NULL;
}

static int ir_gpu_tensor_pointer_matches(const MtlcType *type,
                                         MtlcTypeKind storage_kind) {
  return type && type->kind == MTLC_TYPE_POINTER && type->base_type &&
         type->base_type->kind == storage_kind;
}

static int ir_gpu_instruction_defines_dest(
    const IRInstruction *instruction);

static MtlcAddressSpace ir_gpu_operand_address_space_impl(
    const IRProgram *program, const IRFunction *function,
    const IROperand *operand, unsigned depth) {
  if (!program || !function || !operand || depth > 32 ||
      (operand->kind != IR_OPERAND_SYMBOL &&
       operand->kind != IR_OPERAND_TEMP) ||
      !operand->name)
    return MTLC_ADDRESS_SPACE_DEFAULT;

  const IRModuleSymbol *function_symbol =
      ir_program_lookup_symbol(program, function->name);
  if (operand->kind == IR_OPERAND_SYMBOL && function_symbol &&
      function_symbol->kind == IR_MODSYM_FUNCTION) {
    for (size_t p = 0; p < function->parameter_count &&
                       p < function_symbol->param_count;
         p++) {
      if (!function->parameter_names || !function->parameter_names[p] ||
          strcmp(function->parameter_names[p], operand->name) != 0)
        continue;
      const MtlcType *type = function_symbol->param_types
                                 ? function_symbol->param_types[p]
                                 : NULL;
      if (type && type->kind == MTLC_TYPE_POINTER &&
          type->address_space != MTLC_ADDRESS_SPACE_DEFAULT)
        return type->address_space;
      return function->is_kernel ? MTLC_ADDRESS_SPACE_GLOBAL
                                 : MTLC_ADDRESS_SPACE_DEFAULT;
    }
  }

  for (size_t i = function->instruction_count; i > 0; i--) {
    const IRInstruction *producer = &function->instructions[i - 1];
    if (!producer->dest.name ||
        strcmp(producer->dest.name, operand->name) != 0)
      continue;
    /* STORE.dest and several fused-op dest fields are address/value uses.
     * They must not shadow the allocation or pointer expression that actually
     * defines this operand's provenance. */
    if (!ir_gpu_instruction_defines_dest(producer)) continue;
    if (producer->op == IR_OP_ADDRESS_SPACE_ALLOC)
      return producer->address_space;
    if (producer->value_type &&
        producer->value_type->kind == MTLC_TYPE_POINTER &&
        producer->value_type->address_space != MTLC_ADDRESS_SPACE_DEFAULT &&
        producer->value_type->address_space != MTLC_ADDRESS_SPACE_GENERIC)
      return producer->value_type->address_space;
    if (producer->op == IR_OP_BINARY) {
      MtlcAddressSpace lhs = ir_gpu_operand_address_space_impl(
          program, function, &producer->lhs, depth + 1);
      if (lhs != MTLC_ADDRESS_SPACE_DEFAULT) return lhs;
      return ir_gpu_operand_address_space_impl(program, function,
                                               &producer->rhs, depth + 1);
    }
    if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST)
      return ir_gpu_operand_address_space_impl(program, function,
                                               &producer->lhs, depth + 1);
    break;
  }
  const IRModuleSymbol *symbol =
      ir_program_lookup_symbol(program, operand->name);
  if (symbol && symbol->kind == IR_MODSYM_VARIABLE && symbol->type &&
      symbol->type->kind == MTLC_TYPE_POINTER)
    return symbol->type->address_space;
  return MTLC_ADDRESS_SPACE_DEFAULT;
}

static MtlcAddressSpace ir_gpu_operand_address_space(
    const IRProgram *program, const IRFunction *function,
    const IROperand *operand) {
  return ir_gpu_operand_address_space_impl(program, function, operand, 0);
}

static int ir_gpu_async_copy_signature_matches(
    const IRProgram *program, const IRFunction *function,
    const IRInstruction *instruction) {
  if (!program || !function || !instruction) return 0;
  if (instruction->op == IR_OP_ASYNC_COMMIT) {
    return instruction->argument_count == 0 &&
           instruction->async_copy_element_count == 0 &&
           instruction->async_copy_transaction_bytes == 0 &&
           instruction->async_copy_pending_groups == 0 &&
           instruction->async_copy_cache == MTLC_ASYNC_CACHE_DEFAULT &&
           instruction->async_copy_generated == 0;
  }
  if (instruction->op == IR_OP_ASYNC_WAIT) {
    return instruction->argument_count == 0 &&
           instruction->async_copy_element_count == 0 &&
           instruction->async_copy_transaction_bytes == 0 &&
           instruction->async_copy_pending_groups <= 7 &&
           instruction->async_copy_cache == MTLC_ASYNC_CACHE_DEFAULT &&
           instruction->async_copy_generated == 0;
  }
  if (instruction->op != IR_OP_ASYNC_COPY ||
      instruction->argument_count != 2 ||
      instruction->async_copy_element_count == 0 ||
      instruction->async_copy_element_count > 4096 ||
      (instruction->async_copy_transaction_bytes != 4 &&
       instruction->async_copy_transaction_bytes != 8 &&
       instruction->async_copy_transaction_bytes != 16) ||
      (instruction->async_copy_cache != MTLC_ASYNC_CACHE_ALL &&
       instruction->async_copy_cache != MTLC_ASYNC_CACHE_GLOBAL) ||
      (instruction->async_copy_generated != 0 &&
       instruction->async_copy_generated != 1))
    return 0;
  const MtlcType *destination =
      ir_gpu_operand_type(program, function, instruction, 0);
  const MtlcType *source =
      ir_gpu_operand_type(program, function, instruction, 1);
  MtlcTypeKind element_kind =
      destination && destination->base_type ? destination->base_type->kind
                                            : MTLC_TYPE_VOID;
  int scalar_element =
      element_kind == MTLC_TYPE_INT8 || element_kind == MTLC_TYPE_INT16 ||
      element_kind == MTLC_TYPE_INT32 || element_kind == MTLC_TYPE_INT64 ||
      element_kind == MTLC_TYPE_UINT8 || element_kind == MTLC_TYPE_UINT16 ||
      element_kind == MTLC_TYPE_UINT32 || element_kind == MTLC_TYPE_UINT64 ||
      element_kind == MTLC_TYPE_BOOL || element_kind == MTLC_TYPE_FLOAT32 ||
      element_kind == MTLC_TYPE_FLOAT64 || element_kind == MTLC_TYPE_FLOAT16 ||
      element_kind == MTLC_TYPE_BFLOAT16;
  if (!destination || !source || destination->kind != MTLC_TYPE_POINTER ||
      source->kind != MTLC_TYPE_POINTER || !destination->base_type ||
      !source->base_type || !scalar_element ||
      destination->base_type->kind != source->base_type->kind ||
      destination->base_type->size == 0 ||
      destination->base_type->size != source->base_type->size ||
      destination->base_type->size > 8 ||
      (size_t)instruction->async_copy_element_count >
          65536u / destination->base_type->size)
    return 0;
  size_t bytes = destination->base_type->size *
                 (size_t)instruction->async_copy_element_count;
  if ((bytes & 3u) != 0 ||
      bytes % instruction->async_copy_transaction_bytes != 0 ||
      (instruction->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL &&
       instruction->async_copy_transaction_bytes != 16))
    return 0;
  MtlcAddressSpace destination_space = ir_gpu_operand_address_space(
      program, function, &instruction->arguments[0]);
  MtlcAddressSpace source_space = ir_gpu_operand_address_space(
      program, function, &instruction->arguments[1]);
  return destination_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
         (source_space == MTLC_ADDRESS_SPACE_GLOBAL ||
          source_space == MTLC_ADDRESS_SPACE_GENERIC);
}

static int ir_gpu_tensor_transfer_signature_matches(
    const IRProgram *program, const IRFunction *function,
    const IRInstruction *instruction) {
  if (!program || !function || !instruction ||
      instruction->op != IR_OP_TENSOR_TRANSFER)
    return 0;
  const MtlcTensorTransferDesc *desc = &IR_TENSOR_TRANSFER(instruction);
  int has_view = instruction->tensor_transfer_has_prepared_view;
  size_t expected = ir_tensor_transfer_operand_count(desc, has_view);
  if (!expected || instruction->argument_count != expected ||
      instruction->dest.kind != IR_OPERAND_NONE)
    return 0;
  MtlcTypeKind storage_kind = ir_tensor_element_storage_kind(desc->element);
  const MtlcType *destination =
      ir_gpu_operand_type(program, function, instruction, 0);
  const MtlcType *source =
      ir_gpu_operand_type(program, function, instruction, 1);
  if (!ir_gpu_tensor_pointer_matches(destination, storage_kind) ||
      !ir_gpu_tensor_pointer_matches(source, storage_kind))
    return 0;
  MtlcAddressSpace destination_space = ir_gpu_operand_address_space(
      program, function, &instruction->arguments[0]);
  MtlcAddressSpace source_space = ir_gpu_operand_address_space(
      program, function, &instruction->arguments[1]);
  if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
    if (destination_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        (source_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         source_space != MTLC_ADDRESS_SPACE_GENERIC))
      return 0;
  } else if ((destination_space != MTLC_ADDRESS_SPACE_GLOBAL &&
              destination_space != MTLC_ADDRESS_SPACE_GENERIC) ||
             source_space != MTLC_ADDRESS_SPACE_WORKGROUP) {
    return 0;
  }
  size_t coordinate_base = 2;
  if (has_view) {
    const MtlcType *view =
        ir_gpu_operand_type(program, function, instruction, 2);
    MtlcAddressSpace view_space = ir_gpu_operand_address_space(
        program, function, &instruction->arguments[2]);
    if (!view || view->kind != MTLC_TYPE_POINTER || !view->base_type ||
        view->base_type->kind != MTLC_TYPE_UINT8 ||
        (view_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         view_space != MTLC_ADDRESS_SPACE_GENERIC))
      return 0;
    coordinate_base++;
  }
  for (size_t dimension = 0; dimension < desc->rank; dimension++) {
    const MtlcType *coordinate = ir_gpu_operand_type(
        program, function, instruction, coordinate_base + dimension);
    if (!coordinate || coordinate->kind != MTLC_TYPE_INT32) return 0;
  }
  return 1;
}

/* Validate per-work-item async groups over arbitrary neutral CFGs. A state is
 * (committed pending groups 0..8, has-uncommitted-copy). Union at CFG merges
 * preserves every possible path; an unmatched loop keeps growing until it
 * exceeds the architectural-neutral eight-group bound and is rejected. */
static int ir_gpu_async_copy_groups_valid(const IRProgram *program,
                                          IRFunction *function) {
  int have_async = 0;
  if (!program || !function) return 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_ASYNC_COPY &&
        instruction->op != IR_OP_ASYNC_COMMIT &&
        instruction->op != IR_OP_ASYNC_WAIT)
      continue;
    have_async = 1;
    if (!ir_gpu_async_copy_signature_matches(program, function, instruction))
      return 0;
  }
  if (!have_async) return 1;
  if (!ir_function_rebuild_cfg(function)) return 0;
  size_t count = function->block_count;
  uint32_t *inputs = calloc(count ? count : 1, sizeof(*inputs));
  uint32_t *outputs = calloc(count ? count : 1, sizeof(*outputs));
  if (!inputs || !outputs) {
    free(inputs);
    free(outputs);
    return 0;
  }
  inputs[function->entry_block] = 1u; /* pending=0, uncommitted=false */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t block_index = 0; block_index < count; block_index++) {
      const IRBasicBlock *block = &function->blocks[block_index];
      uint32_t incoming =
          block_index == function->entry_block ? 1u : 0u;
      for (size_t p = 0; p < block->predecessor_count; p++) {
        size_t predecessor = block->predecessors[p];
        if (predecessor < count) incoming |= outputs[predecessor];
      }
      if ((incoming | inputs[block_index]) != inputs[block_index]) {
        inputs[block_index] |= incoming;
        changed = 1;
      }
      uint32_t states = inputs[block_index];
      if (!states) continue;
      for (size_t offset = 0; offset < block->instruction_count; offset++) {
        const IRInstruction *instruction = &block->instructions[offset];
        if (instruction->op != IR_OP_ASYNC_COPY &&
            instruction->op != IR_OP_ASYNC_COMMIT &&
            instruction->op != IR_OP_ASYNC_WAIT)
          continue;
        uint32_t next = 0;
        for (unsigned pending = 0; pending <= 8; pending++) {
          for (unsigned uncommitted = 0; uncommitted <= 1; uncommitted++) {
            unsigned state = pending * 2u + uncommitted;
            if (!(states & (1u << state))) continue;
            unsigned next_pending = pending;
            unsigned next_uncommitted = uncommitted;
            if (instruction->op == IR_OP_ASYNC_COPY) {
              next_uncommitted = 1;
            } else if (instruction->op == IR_OP_ASYNC_COMMIT) {
              if (pending == 8) {
                free(inputs);
                free(outputs);
                return 0;
              }
              next_pending = pending + 1;
              next_uncommitted = 0;
            } else {
              uint32_t allowed = instruction->async_copy_pending_groups;
              if (next_pending > allowed) next_pending = allowed;
            }
            next |= 1u << (next_pending * 2u + next_uncommitted);
          }
        }
        states = next;
      }
      if ((states | outputs[block_index]) != outputs[block_index]) {
        outputs[block_index] |= states;
        changed = 1;
      }
    }
  }
  int valid = 1;
  for (size_t block = 0; block < count; block++) {
    if (function->blocks[block].successor_count == 0 && outputs[block] != 0 &&
        outputs[block] != 1u) {
      valid = 0;
      break;
    }
  }
  free(inputs);
  free(outputs);
  return valid;
}

static int ir_gpu_tensor_stride_type(const MtlcType *type) {
  if (!type) return 0;
  switch (type->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

static int ir_gpu_tensor_unsigned_control_type(const MtlcType *type) {
  if (!type) return 0;
  return type->kind == MTLC_TYPE_UINT8 || type->kind == MTLC_TYPE_UINT16 ||
         type->kind == MTLC_TYPE_UINT32 || type->kind == MTLC_TYPE_UINT64;
}

static size_t ir_tensor_runtime_stride_operand_index(
    const MtlcTensorMmaDesc *desc, unsigned requested_bit) {
  size_t index = 4u +
                 (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
                 (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
                 (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  unsigned mask = ir_tensor_mma_runtime_stride_mask(desc);
  for (unsigned bit = MTLC_TENSOR_RUNTIME_STRIDE_A;
       bit <= MTLC_TENSOR_RUNTIME_STRIDE_D; bit <<= 1) {
    if (!(mask & bit)) continue;
    if (bit == requested_bit) return index;
    index++;
  }
  return SIZE_MAX;
}

static int ir_gpu_tensor_signature_matches(const IRProgram *program,
                                            const IRFunction *function,
                                            const IRInstruction *instruction) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(instruction);
  size_t per_tile = ir_tensor_mma_operand_count(desc);
  size_t tile_count = ir_tensor_mma_instruction_count(instruction);
  if (!per_tile || tile_count > SIZE_MAX / per_tile ||
      instruction->argument_count != per_tile * tile_count ||
      instruction->dest.kind != IR_OPERAND_NONE) {
    return 0;
  }
  MtlcTypeKind base_kinds[4] = {
      ir_tensor_element_storage_kind(desc->a_element),
      ir_tensor_element_storage_kind(desc->b_element),
      ir_tensor_element_storage_kind(desc->accumulator_element),
      ir_tensor_element_storage_kind(desc->result_element)};
  for (size_t tile = 0; tile < tile_count; tile++) {
    size_t base = tile * per_tile;
    size_t index = 0;
    for (; index < 4; index++) {
      if (!ir_gpu_tensor_pointer_matches(
              ir_gpu_operand_type(program, function, instruction,
                                  base + index),
              base_kinds[index])) {
        return 0;
      }
    }
    if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
      const MtlcType *metadata = ir_gpu_operand_type(
          program, function, instruction, base + index++);
      if (!ir_gpu_tensor_pointer_matches(metadata, MTLC_TYPE_UINT8)) return 0;
    }
    if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE) {
      const MtlcType *scale = ir_gpu_operand_type(
          program, function, instruction, base + index++);
      if (!ir_gpu_tensor_pointer_matches(
              scale, ir_tensor_element_storage_kind(desc->a_scale_element)))
        return 0;
    }
    if (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE) {
      const MtlcType *scale = ir_gpu_operand_type(
          program, function, instruction, base + index++);
      if (!ir_gpu_tensor_pointer_matches(
              scale, ir_tensor_element_storage_kind(desc->b_scale_element)))
        return 0;
    }
    unsigned runtime_strides = ir_tensor_mma_runtime_stride_mask(desc);
    for (unsigned bit = MTLC_TENSOR_RUNTIME_STRIDE_A;
         bit <= MTLC_TENSOR_RUNTIME_STRIDE_D; bit <<= 1) {
      if (!(runtime_strides & bit)) continue;
      if (!ir_gpu_tensor_stride_type(ir_gpu_operand_type(
              program, function, instruction, base + index++)))
        return 0;
    }
    if (index != per_tile) return 0;
  }

  if (tile_count > 1) {
    if (desc->accumulator_element != desc->result_element ||
        desc->c_layout != desc->d_layout ||
        ((desc->c_leading_dimension == 0) !=
         (desc->d_leading_dimension == 0)) ||
        (desc->c_leading_dimension != 0 &&
         desc->c_leading_dimension != desc->d_leading_dimension))
      return 0;
    const IROperand *output = &instruction->arguments[3];
    size_t c_stride = ir_tensor_runtime_stride_operand_index(
        desc, MTLC_TENSOR_RUNTIME_STRIDE_C);
    size_t d_stride = ir_tensor_runtime_stride_operand_index(
        desc, MTLC_TENSOR_RUNTIME_STRIDE_D);
    const IROperand *output_stride =
        d_stride == SIZE_MAX ? NULL : &instruction->arguments[d_stride];
    for (size_t tile = 0; tile < tile_count; tile++) {
      size_t base = tile * per_tile;
      if (!ir_operand_same(&instruction->arguments[base + 3], output) ||
          (tile > 0 &&
           !ir_operand_same(&instruction->arguments[base + 2], output)))
        return 0;
      if (output_stride &&
          (!ir_operand_same(&instruction->arguments[base + d_stride],
                            output_stride) ||
           (tile > 0 &&
            !ir_operand_same(&instruction->arguments[base + c_stride],
                             output_stride))))
        return 0;
    }
  }
  return 1;
}

static int ir_gpu_tensor_matmul_signature_matches(
    const IRProgram *program, const IRFunction *function,
    const IRInstruction *instruction) {
  size_t mma_operands = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(instruction));
  size_t expected = ir_tensor_matmul_operand_count(&IR_TENSOR_MMA(instruction));
  if (!mma_operands || expected != mma_operands + 5u ||
      instruction->argument_count != expected ||
      instruction->dest.kind != IR_OPERAND_NONE ||
      ir_tensor_mma_instruction_count(instruction) != 1u)
    return 0;

  /* The first bundle is deliberately identical to one exact neutral MMA.
   * Validate it through the same descriptor/type contract, then validate the
   * five whole-problem controls independently. */
  IRInstruction tile = *instruction;
  tile.op = IR_OP_TENSOR_MMA;
  tile.argument_count = mma_operands;
  tile.tensor_mma_count = 1;
  if (!ir_gpu_tensor_signature_matches(program, function, &tile)) return 0;
  unsigned runtime_strides =
      ir_tensor_mma_runtime_stride_mask(&IR_TENSOR_MMA(instruction));
  for (unsigned bit = MTLC_TENSOR_RUNTIME_STRIDE_A;
       bit <= MTLC_TENSOR_RUNTIME_STRIDE_D; bit <<= 1) {
    if (!(runtime_strides & bit)) continue;
    size_t index = ir_tensor_runtime_stride_operand_index(
        &IR_TENSOR_MMA(instruction), bit);
    const MtlcType *type = index == SIZE_MAX
                               ? NULL
                               : ir_gpu_operand_type(program, function,
                                                     instruction, index);
    if (!type || (type->kind != MTLC_TYPE_UINT8 &&
                  type->kind != MTLC_TYPE_UINT16 &&
                  type->kind != MTLC_TYPE_UINT32))
      return 0;
  }
  for (size_t i = 0; i < 5; i++) {
    if (!ir_gpu_tensor_unsigned_control_type(ir_gpu_operand_type(
            program, function, instruction, mma_operands + i)))
      return 0;
  }
  return 1;
}

static int ir_gpu_tensor_epilogue_signature_matches(
    const IRProgram *program, const IRFunction *function,
    const IRInstruction *instruction) {
  const MtlcTensorEpilogueDesc *desc = &IR_TENSOR_EPILOGUE(instruction);
  size_t expected = ir_tensor_epilogue_operand_count(desc);
  size_t index = 0;
  MtlcTypeKind storage_kind = ir_tensor_element_storage_kind(desc->element);
  MtlcTypeKind compute_kind = desc->element == MTLC_TENSOR_ELEMENT_FLOAT64
                                  ? MTLC_TYPE_FLOAT64
                                  : MTLC_TYPE_FLOAT32;
  if (!expected || instruction->argument_count != expected ||
      instruction->dest.kind != IR_OPERAND_NONE ||
      !ir_gpu_tensor_pointer_matches(
          ir_gpu_operand_type(program, function, instruction, index++),
          storage_kind)) {
    return 0;
  }
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE &&
      !ir_gpu_tensor_pointer_matches(
          ir_gpu_operand_type(program, function, instruction, index++),
          storage_kind)) {
    return 0;
  }
  if (desc->scale_output) {
    const MtlcType *alpha =
        ir_gpu_operand_type(program, function, instruction, index++);
    if (!alpha || alpha->kind != compute_kind) return 0;
  }
  if (desc->scale_bias) {
    const MtlcType *beta =
        ir_gpu_operand_type(program, function, instruction, index++);
    if (!beta || beta->kind != compute_kind) return 0;
  }
  if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    const MtlcType *minimum =
        ir_gpu_operand_type(program, function, instruction, index++);
    const MtlcType *maximum =
        ir_gpu_operand_type(program, function, instruction, index++);
    if (!minimum || !maximum || minimum->kind != compute_kind ||
        maximum->kind != compute_kind)
      return 0;
  }
  if (desc->leading_dimension == 0 &&
      !ir_gpu_tensor_stride_type(
          ir_gpu_operand_type(program, function, instruction, index++))) {
    return 0;
  }
  if (desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX &&
      desc->bias_leading_dimension == 0 &&
      !ir_gpu_tensor_stride_type(
          ir_gpu_operand_type(program, function, instruction, index++))) {
    return 0;
  }
  return index == expected;
}

static int ir_gpu_tensor_residency_block_for_instruction(
    const IRFunction *function, size_t instruction_index,
    size_t *out_block) {
  if (!function || !out_block) return 0;
  for (size_t block = 0; block < function->block_count; block++) {
    const IRBasicBlock *candidate = &function->blocks[block];
    if (instruction_index >= candidate->first_instruction &&
        instruction_index <
            candidate->first_instruction + candidate->instruction_count) {
      *out_block = block;
      return 1;
    }
  }
  return 0;
}

static int ir_gpu_tensor_residency_has_edge(const IRBasicBlock *block,
                                             size_t target) {
  if (!block) return 0;
  for (size_t i = 0; i < block->successor_count; i++) {
    if (block->successors[i] == target) return 1;
  }
  return 0;
}

static int ir_gpu_tensor_residency_mentions_operand(
    const IRInstruction *instruction, const IROperand *operand) {
  if (!instruction || !operand) return 0;
  if (ir_operand_same(&instruction->dest, operand) ||
      ir_operand_same(&instruction->lhs, operand) ||
      ir_operand_same(&instruction->rhs, operand))
    return 1;
  for (size_t i = 0; i < instruction->argument_count; i++)
    if (ir_operand_same(&instruction->arguments[i], operand)) return 1;
  return 0;
}

static int ir_gpu_tensor_pipeline_scalar_instruction(
    const IRInstruction *instruction) {
  if (!instruction) return 0;
  switch (instruction->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_ASSIGN:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_SELECT:
    return 1;
  default:
    return 0;
  }
}

static int ir_gpu_tensor_pipeline_barrier(
    const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_BARRIER &&
         instruction->memory_scope == MTLC_MEMORY_SCOPE_WORKGROUP &&
         (instruction->memory_regions & MTLC_MEMORY_REGION_WORKGROUP) != 0 &&
         (instruction->memory_order == MTLC_MEMORY_ORDER_ACQ_REL ||
          instruction->memory_order == MTLC_MEMORY_ORDER_SEQ_CST);
}

static int ir_gpu_tensor_residency_groups_valid(IRFunction *function) {
  if (!function) return 0;
  int have_group = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    IRTensorResidencyRole role = instruction->tensor_residency_role;
    IRTensorResidencyScope scope = instruction->tensor_residency_scope;
    uint32_t id = instruction->tensor_residency_id;
    if (role == IR_TENSOR_RESIDENCY_NONE) {
      if (id != 0 || scope != IR_TENSOR_RESIDENCY_SCOPE_NONE ||
          instruction->op == IR_OP_TENSOR_COMMIT)
        return 0;
      continue;
    }
    if (id == 0 ||
        (role != IR_TENSOR_RESIDENCY_START &&
         role != IR_TENSOR_RESIDENCY_UPDATE &&
         role != IR_TENSOR_RESIDENCY_COMMIT) ||
        (scope != IR_TENSOR_RESIDENCY_SCOPE_LOOP &&
         scope != IR_TENSOR_RESIDENCY_SCOPE_PIPELINE) ||
        (role == IR_TENSOR_RESIDENCY_COMMIT) !=
            (instruction->op == IR_OP_TENSOR_COMMIT) ||
        (role != IR_TENSOR_RESIDENCY_COMMIT &&
         instruction->op != IR_OP_TENSOR_MMA) ||
        ir_tensor_mma_instruction_count(instruction) != 1)
      return 0;
    have_group = 1;
  }
  if (!have_group) return 1;
  if (!ir_function_rebuild_cfg(function)) return 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *start = &function->instructions[i];
    if (start->tensor_residency_role != IR_TENSOR_RESIDENCY_START) continue;
    const IRInstruction *first_update = NULL;
    const IRInstruction *commit = NULL;
    size_t first_update_index = 0, last_update_index = 0, commit_index = 0;
    size_t starts = 0, updates = 0, commits = 0;
    for (size_t j = 0; j < function->instruction_count; j++) {
      const IRInstruction *candidate = &function->instructions[j];
      if (candidate->tensor_residency_id != start->tensor_residency_id)
        continue;
      if (candidate->tensor_residency_role == IR_TENSOR_RESIDENCY_START) {
        starts++;
      } else if (candidate->tensor_residency_role ==
                 IR_TENSOR_RESIDENCY_UPDATE) {
        if (!first_update) {
          first_update = candidate;
          first_update_index = j;
        }
        last_update_index = j;
        updates++;
      } else if (candidate->tensor_residency_role ==
                 IR_TENSOR_RESIDENCY_COMMIT) {
        commit = candidate;
        commit_index = j;
        commits++;
      }
    }
    if (starts != 1 || updates < 1 || commits != 1 || !first_update ||
        !commit || !(i < first_update_index &&
                     last_update_index < commit_index) ||
        commit->tensor_residency_scope != start->tensor_residency_scope ||
        !ir_tensor_mma_desc_equal(&IR_TENSOR_MMA(start), &IR_TENSOR_MMA(commit)) ||
        start->argument_count < 4 ||
        commit->argument_count != start->argument_count)
      return 0;
    for (size_t arg = 0; arg < start->argument_count; arg++) {
      if (!ir_operand_same(&commit->arguments[arg], &start->arguments[arg]))
        return 0;
    }
    size_t c_stride = ir_tensor_runtime_stride_operand_index(
        &IR_TENSOR_MMA(start), MTLC_TENSOR_RUNTIME_STRIDE_C);
    size_t d_stride = ir_tensor_runtime_stride_operand_index(
        &IR_TENSOR_MMA(start), MTLC_TENSOR_RUNTIME_STRIDE_D);
    for (size_t j = first_update_index; j <= last_update_index; j++) {
      const IRInstruction *update = &function->instructions[j];
      if (update->tensor_residency_id != start->tensor_residency_id ||
          update->tensor_residency_role != IR_TENSOR_RESIDENCY_UPDATE)
        continue;
      if (update->tensor_residency_scope != start->tensor_residency_scope ||
          !ir_tensor_mma_desc_equal(&IR_TENSOR_MMA(start),
                                    &IR_TENSOR_MMA(update)) ||
          update->argument_count != start->argument_count ||
          !ir_operand_same(&update->arguments[2], &start->arguments[3]) ||
          !ir_operand_same(&update->arguments[3], &start->arguments[3]) ||
          (d_stride != SIZE_MAX &&
           (c_stride == SIZE_MAX ||
            !ir_operand_same(&update->arguments[c_stride],
                             &start->arguments[d_stride]) ||
            !ir_operand_same(&update->arguments[d_stride],
                             &start->arguments[d_stride]))))
        return 0;
    }

    size_t start_block = 0, update_block = 0, commit_block = 0;
    if (!ir_gpu_tensor_residency_block_for_instruction(function, i,
                                                        &start_block) ||
        !ir_gpu_tensor_residency_block_for_instruction(function,
                                                        first_update_index,
                                                        &update_block) ||
        !ir_gpu_tensor_residency_block_for_instruction(function,
                                                        commit_index,
                                                        &commit_block))
      return 0;
    if (start->tensor_residency_scope == IR_TENSOR_RESIDENCY_SCOPE_LOOP) {
      /* START falls into one loop header, UPDATE is its sole backedge body,
       * and the other header edge is the unique COMMIT predecessor. */
      if (updates != 1 || start_block == update_block ||
          start_block == commit_block ||
          update_block == commit_block)
        return 0;
      const IRBasicBlock *start_cfg = &function->blocks[start_block];
      const IRBasicBlock *update_cfg = &function->blocks[update_block];
      const IRBasicBlock *commit_cfg = &function->blocks[commit_block];
      if (start_cfg->successor_count != 1 ||
          update_cfg->successor_count != 1 ||
          start_cfg->successors[0] != update_cfg->successors[0])
        return 0;
      size_t header_block = start_cfg->successors[0];
      if (header_block >= function->block_count) return 0;
      const IRBasicBlock *header_cfg = &function->blocks[header_block];
      if (header_cfg->successor_count != 2 ||
          !ir_gpu_tensor_residency_has_edge(header_cfg, update_block) ||
          !ir_gpu_tensor_residency_has_edge(header_cfg, commit_block) ||
          update_cfg->predecessor_count != 1 ||
          update_cfg->predecessors[0] != header_block ||
          commit_cfg->predecessor_count != 1 ||
          commit_cfg->predecessors[0] != header_block ||
          commit_cfg->successor_count > 1)
        return 0;
    } else {
      /* A staged pipeline is one straight-line block. Every adjacent MMA pair
       * has its own ordered async WAIT -> publication BARRIER handoff; D stays
       * entirely unobservable until the single final commit. */
      if (start_block != update_block || start_block != commit_block)
        return 0;
      size_t previous_mma_index = i;
      size_t verified_updates = 0;
      for (size_t update_index = first_update_index;
           update_index <= last_update_index; update_index++) {
        const IRInstruction *candidate =
            &function->instructions[update_index];
        if (candidate->tensor_residency_id != start->tensor_residency_id ||
            candidate->tensor_residency_role != IR_TENSOR_RESIDENCY_UPDATE)
          continue;
        size_t candidate_block = 0;
        if (!ir_gpu_tensor_residency_block_for_instruction(
                function, update_index, &candidate_block) ||
            candidate_block != start_block)
          return 0;
        int handoff_state = 0; /* 0 issuing, 1 waited, 2 published */
        for (size_t j = previous_mma_index + 1; j < update_index; j++) {
          const IRInstruction *middle = &function->instructions[j];
          if (ir_gpu_tensor_residency_mentions_operand(
                  middle, &start->arguments[3]))
            return 0;
          if (ir_gpu_tensor_pipeline_scalar_instruction(middle)) continue;
          if (handoff_state == 0 &&
              (middle->op == IR_OP_ASYNC_COPY ||
               middle->op == IR_OP_ASYNC_COMMIT))
            continue;
          if (handoff_state == 0 && middle->op == IR_OP_ASYNC_WAIT) {
            handoff_state = 1;
            continue;
          }
          if (handoff_state == 1 &&
              ir_gpu_tensor_pipeline_barrier(middle)) {
            handoff_state = 2;
            continue;
          }
          return 0;
        }
        if (handoff_state != 2) return 0;
        previous_mma_index = update_index;
        verified_updates++;
      }
      if (verified_updates != updates) return 0;
      for (size_t j = previous_mma_index + 1; j < commit_index; j++)
        if (function->instructions[j].op != IR_OP_NOP) return 0;
    }
  }

  /* Every update/commit must belong to the unique start checked above. */
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->tensor_residency_role == IR_TENSOR_RESIDENCY_NONE ||
        instruction->tensor_residency_role == IR_TENSOR_RESIDENCY_START)
      continue;
    size_t starts = 0;
    for (size_t j = 0; j < function->instruction_count; j++) {
      const IRInstruction *candidate = &function->instructions[j];
      if (candidate->tensor_residency_role == IR_TENSOR_RESIDENCY_START &&
          candidate->tensor_residency_id == instruction->tensor_residency_id)
        starts++;
    }
    if (starts != 1) return 0;
  }
  return 1;
}

/* GPU collective legality is a shared-IR property. A frontend cannot make a
 * divergent barrier correct, and a backend must not quietly reinterpret one.
 * Track the strongest scope at which a value is known uniform:
 *
 *   WORKGROUP <= SUBGROUP <= VARYING
 *
 * The join is therefore max(). Kernel parameters are workgroup-uniform. Device
 * helper parameter ranks are propagated from every reachable call site in
 * caller-before-callee order, so helpers remain reusable without pretending
 * their arguments are always uniform. Memory loads and unknown call results
 * are conservatively varying. */
enum {
  IR_GPU_UNIFORM_WORKGROUP = 0,
  IR_GPU_UNIFORM_SUBGROUP = 1,
  IR_GPU_UNIFORM_VARYING = 2
};

enum {
  IR_GPU_COLLECTIVE_NONE = 0,
  IR_GPU_COLLECTIVE_SUBGROUP = 1,
  IR_GPU_COLLECTIVE_WORKGROUP = 2
};

typedef struct {
  const char *name; /* borrowed from IR */
  unsigned char rank;
  /* Lane decomposition: the value is W*s + l where s is subgroup-uniform,
   * W is the subgroup width, and l is the caller's lane (0 <= l < W), so
   * dividing it by W yields a subgroup-uniform value. thread.x has that shape
   * under the documented launch precondition (a one-dimensional block, or a
   * block x-extent that is a multiple of the subgroup width). */
  unsigned char lane_affine;
  /* The value is the subgroup width itself (a subgroup_size() result). */
  unsigned char subgroup_width;
} IRGpuUniformSlot;

typedef struct {
  IRGpuUniformSlot *slots;
  size_t capacity;
} IRGpuUniformMap;

static size_t ir_gpu_uniform_hash(const char *name) {
  size_t hash = (size_t)1469598103934665603ULL;
  if (!name) return 0;
  while (*name) {
    hash ^= (unsigned char)*name++;
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static IRGpuUniformSlot *ir_gpu_uniform_slot(IRGpuUniformMap *map,
                                             const char *name, int create) {
  if (!map || !map->slots || !map->capacity || !name) return NULL;
  size_t mask = map->capacity - 1;
  size_t index = ir_gpu_uniform_hash(name) & mask;
  for (size_t probe = 0; probe < map->capacity; probe++) {
    IRGpuUniformSlot *slot = &map->slots[index];
    if (!slot->name) {
      if (create) {
        slot->name = name;
        slot->rank = IR_GPU_UNIFORM_WORKGROUP;
        return slot;
      }
      return NULL;
    }
    if (strcmp(slot->name, name) == 0) return slot;
    index = (index + 1) & mask;
  }
  return NULL;
}

static int ir_gpu_instruction_defines_dest(const IRInstruction *instruction) {
  if (!instruction || instruction->dest.kind == IR_OPERAND_NONE ||
      !instruction->dest.name) {
    return 0;
  }
  switch (instruction->op) {
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_ADDRESS_SPACE_ALLOC:
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  case IR_OP_SELECT:
    return 1;
  default:
    return 0;
  }
}

static unsigned char ir_gpu_uniform_operand(const IRGpuUniformMap *map,
                                            const IROperand *operand) {
  if (!operand) return IR_GPU_UNIFORM_VARYING;
  switch (operand->kind) {
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
  case IR_OPERAND_STRING:
  case IR_OPERAND_LABEL:
  case IR_OPERAND_NONE:
    return IR_GPU_UNIFORM_WORKGROUP;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    IRGpuUniformSlot *slot = ir_gpu_uniform_slot((IRGpuUniformMap *)map,
                                                operand->name, 0);
    return slot ? slot->rank : IR_GPU_UNIFORM_VARYING;
  }
  default:
    return IR_GPU_UNIFORM_VARYING;
  }
}

static const IRGpuUniformSlot *ir_gpu_uniform_operand_slot(
    const IRGpuUniformMap *map, const IROperand *operand) {
  if (!operand ||
      (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL))
    return NULL;
  return ir_gpu_uniform_slot((IRGpuUniformMap *)map, operand->name, 0);
}

/* A cast keeps a lane decomposition only when it is value-preserving for the
 * small nonnegative id/width magnitudes involved: any 32-bit-or-wider integer
 * target qualifies; a narrowing or float cast does not (a truncated or
 * rounded id no longer floors to its subgroup index). The frontend stamps the
 * target type name in text; builder-created casts carry value_type instead. */
static int ir_gpu_cast_preserves_lane_shape(const IRInstruction *instruction) {
  if (instruction->op != IR_OP_CAST || instruction->dest.float_bits != 0)
    return 0;
  if (instruction->text) {
    return strcmp(instruction->text, "int32") == 0 ||
           strcmp(instruction->text, "uint32") == 0 ||
           strcmp(instruction->text, "int64") == 0 ||
           strcmp(instruction->text, "uint64") == 0;
  }
  if (!instruction->value_type) return 0;
  switch (instruction->value_type->kind) {
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

/* Downward fixpoint over the lane-shape flags: assume every defined name has
 * both shapes, then strip the assumption wherever some reachable definition
 * fails to produce it. DECLARE_LOCAL is a binding, not a value definition, and
 * is skipped exactly as the rank fixpoint skips it. */
static void ir_gpu_uniform_map_mark_lane_shapes(const IRFunction *function,
                                                IRGpuUniformMap *map) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!ir_gpu_instruction_defines_dest(instruction) ||
        instruction->op == IR_OP_DECLARE_LOCAL)
      continue;
    IRGpuUniformSlot *slot = ir_gpu_uniform_slot(map, instruction->dest.name, 0);
    if (!slot) continue;
    slot->lane_affine = 1;
    slot->subgroup_width = 1;
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (!ir_gpu_instruction_defines_dest(instruction) ||
          instruction->op == IR_OP_DECLARE_LOCAL)
        continue;
      IRGpuUniformSlot *slot =
          ir_gpu_uniform_slot(map, instruction->dest.name, 0);
      if (!slot || (!slot->lane_affine && !slot->subgroup_width)) continue;
      unsigned char def_lane = 0;
      unsigned char def_width = 0;
      if (instruction->op == IR_OP_CALL) {
        if (instruction->intrinsic == MTLC_INTRINSIC_GPU_LOCAL_ID_X)
          def_lane = 1;
        else if (instruction->intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SIZE)
          def_width = 1;
      } else if (instruction->op == IR_OP_ASSIGN ||
                 ir_gpu_cast_preserves_lane_shape(instruction)) {
        const IRGpuUniformSlot *source =
            ir_gpu_uniform_operand_slot(map, &instruction->lhs);
        if (source) {
          def_lane = source->lane_affine;
          def_width = source->subgroup_width;
        }
      }
      if (slot->lane_affine && !def_lane) {
        slot->lane_affine = 0;
        changed = 1;
      }
      if (slot->subgroup_width && !def_width) {
        slot->subgroup_width = 0;
        changed = 1;
      }
    }
  }
}

/* thread.x / 32, thread.x >> 5, and thread.x / subgroup_size() name the warp
 * (subgroup) a work-item belongs to: every lane of a subgroup computes the
 * same quotient, so the result ranks subgroup-uniform rather than varying.
 * The literal spellings assume the 32-lane PTX subgroup width; the
 * subgroup_size() spelling is the portable form. Integer division only --
 * float division does not floor away the lane term. */
static int ir_gpu_binary_is_subgroup_quotient(const IRGpuUniformMap *map,
                                              const IRInstruction *instruction) {
  if (instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text)
    return 0;
  int is_div = strcmp(instruction->text, "/") == 0;
  int is_shr = strcmp(instruction->text, ">>") == 0;
  if (!is_div && !is_shr) return 0;
  const IRGpuUniformSlot *lhs =
      ir_gpu_uniform_operand_slot(map, &instruction->lhs);
  if (!lhs || !lhs->lane_affine) return 0;
  if (instruction->rhs.kind == IR_OPERAND_INT) {
    long long divisor = instruction->rhs.int_value;
    return is_div ? divisor == 32 : divisor == 5;
  }
  if (is_div) {
    const IRGpuUniformSlot *rhs =
        ir_gpu_uniform_operand_slot(map, &instruction->rhs);
    return rhs && rhs->subgroup_width;
  }
  return 0;
}

static unsigned char ir_gpu_uniform_arguments(const IRGpuUniformMap *map,
                                              const IRInstruction *instruction) {
  unsigned char rank = IR_GPU_UNIFORM_WORKGROUP;
  if (!instruction) return IR_GPU_UNIFORM_VARYING;
  for (size_t i = 0; i < instruction->argument_count; i++) {
    unsigned char argument_rank =
        ir_gpu_uniform_operand(map, &instruction->arguments[i]);
    if (argument_rank > rank) rank = argument_rank;
  }
  return rank;
}

static unsigned char ir_gpu_intrinsic_result_uniformity(
    const IRGpuUniformMap *map, const IRInstruction *instruction) {
  switch (instruction->intrinsic) {
  case MTLC_INTRINSIC_GPU_LOCAL_ID_X:
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Y:
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Z:
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID:
    return IR_GPU_UNIFORM_VARYING;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_X:
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Y:
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Z:
  case MTLC_INTRINSIC_GPU_GROUP_ID_X:
  case MTLC_INTRINSIC_GPU_GROUP_ID_Y:
  case MTLC_INTRINSIC_GPU_GROUP_ID_Z:
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_X:
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Y:
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Z:
    return IR_GPU_UNIFORM_WORKGROUP;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SIZE:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_ANY:
  case MTLC_INTRINSIC_GPU_SUBGROUP_ALL:
    return IR_GPU_UNIFORM_SUBGROUP;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD:
    return IR_GPU_UNIFORM_VARYING;
  case MTLC_INTRINSIC_GPU_SQRT_F32:
  case MTLC_INTRINSIC_GPU_RSQRT_F32:
  case MTLC_INTRINSIC_GPU_ABS_F32:
  case MTLC_INTRINSIC_GPU_SIN_F32:
  case MTLC_INTRINSIC_GPU_COS_F32:
  case MTLC_INTRINSIC_GPU_LOG_F32:
  case MTLC_INTRINSIC_GPU_EXP_F32:
  case MTLC_INTRINSIC_GPU_F16_BITS_TO_F32:
  case MTLC_INTRINSIC_GPU_F32_TO_F16_BITS:
  case MTLC_INTRINSIC_GPU_F32_FROM_BITS:
  case MTLC_INTRINSIC_GPU_F32_TO_BITS:
  case MTLC_INTRINSIC_GPU_DP4A_U32:
  case MTLC_INTRINSIC_GPU_DP4A_S32:
  case MTLC_INTRINSIC_GPU_DP2A_LO_U32:
  case MTLC_INTRINSIC_GPU_DP2A_LO_S32:
  case MTLC_INTRINSIC_GPU_DP2A_HI_U32:
  case MTLC_INTRINSIC_GPU_DP2A_HI_S32:
  case MTLC_INTRINSIC_GPU_PRMT_B32:
  case MTLC_INTRINSIC_GPU_HADD2:
  case MTLC_INTRINSIC_GPU_HMUL2:
  case MTLC_INTRINSIC_GPU_HFMA2:
  case MTLC_INTRINSIC_GPU_H2F_LO:
  case MTLC_INTRINSIC_GPU_H2F_HI:
  case MTLC_INTRINSIC_GPU_F2H2:
  case MTLC_INTRINSIC_GPU_BF2F:
  case MTLC_INTRINSIC_GPU_F2BF:
    return ir_gpu_uniform_arguments(map, instruction);
  case MTLC_INTRINSIC_GPU_LOAD4_F32:
  case MTLC_INTRINSIC_GPU_LOAD4_U32:
  case MTLC_INTRINSIC_GPU_STORE4_F32:
  case MTLC_INTRINSIC_GPU_STORE4_U32:
    /* Memory movers: the values they land are as varying as any load. */
    return IR_GPU_UNIFORM_VARYING;
  case MTLC_INTRINSIC_GPU_PRINT:
  case MTLC_INTRINSIC_GPU_PRINT_I32:
  case MTLC_INTRINSIC_GPU_PRINT_F32:
  case MTLC_INTRINSIC_GPU_PRINT_2I32:
  case MTLC_INTRINSIC_GPU_ASSERT:
    /* Diagnostics return nothing and constrain nothing: a per-lane print or
     * trap is deliberately allowed in divergent control flow, which is where
     * it is most wanted. */
    return IR_GPU_UNIFORM_WORKGROUP;
  case MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER:
    return IR_GPU_UNIFORM_WORKGROUP;
  case MTLC_INTRINSIC_NONE:
  default:
    if (ir_intrinsic_is_atomic(instruction->intrinsic))
      return IR_GPU_UNIFORM_VARYING;
    return IR_GPU_UNIFORM_VARYING;
  }
}

static unsigned char ir_gpu_instruction_result_uniformity(
    const IRGpuUniformMap *map, const IRInstruction *instruction) {
  unsigned char lhs;
  unsigned char rhs;
  if (!instruction) return IR_GPU_UNIFORM_VARYING;
  lhs = ir_gpu_uniform_operand(map, &instruction->lhs);
  rhs = ir_gpu_uniform_operand(map, &instruction->rhs);
  switch (instruction->op) {
  case IR_OP_DECLARE_LOCAL:
    return IR_GPU_UNIFORM_WORKGROUP;
  case IR_OP_ADDRESS_SPACE_ALLOC:
    return instruction->address_space == MTLC_ADDRESS_SPACE_WORKGROUP
               ? IR_GPU_UNIFORM_WORKGROUP
               : IR_GPU_UNIFORM_VARYING;
  case IR_OP_ASSIGN:
  case IR_OP_UNARY:
  case IR_OP_CAST:
    return lhs;
  case IR_OP_BINARY:
    if (ir_gpu_binary_is_subgroup_quotient(map, instruction)) {
      return IR_GPU_UNIFORM_SUBGROUP;
    }
    return lhs > rhs ? lhs : rhs;
  case IR_OP_SELECT:
    return lhs > rhs ? lhs : rhs;
  case IR_OP_LOAD:
  case IR_OP_ADDRESS_OF:
  case IR_OP_NEW:
  case IR_OP_CALL_INDIRECT:
    return IR_GPU_UNIFORM_VARYING;
  case IR_OP_CALL:
    return instruction->intrinsic == MTLC_INTRINSIC_NONE
               ? IR_GPU_UNIFORM_VARYING
               : ir_gpu_intrinsic_result_uniformity(map, instruction);
  case IR_OP_ROTATE_ADD:
    return lhs > rhs ? lhs : rhs;
  default:
    return IR_GPU_UNIFORM_VARYING;
  }
}

static int ir_gpu_uniform_map_build(const IRFunction *function,
                                    const unsigned char *parameter_ranks,
                                    IRGpuUniformMap *map) {
  size_t names = function ? function->parameter_count : 0;
  if (!function || !map) return 0;
  memset(map, 0, sizeof(*map));
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (ir_gpu_instruction_defines_dest(&function->instructions[i])) names++;
  }
  size_t capacity = 8;
  while (capacity < (names + 1) * 2) {
    if (capacity > SIZE_MAX / 2) return 0;
    capacity *= 2;
  }
  map->slots = calloc(capacity, sizeof(*map->slots));
  if (!map->slots) return 0;
  map->capacity = capacity;
  for (size_t i = 0; i < function->parameter_count; i++) {
    IRGpuUniformSlot *slot = ir_gpu_uniform_slot(
        map, function->parameter_names ? function->parameter_names[i] : NULL, 1);
    if (!slot) goto fail;
    slot->rank = parameter_ranks ? parameter_ranks[i]
                                 : IR_GPU_UNIFORM_VARYING;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (ir_gpu_instruction_defines_dest(instruction) &&
        !ir_gpu_uniform_slot(map, instruction->dest.name, 1)) {
      goto fail;
    }
  }
  ir_gpu_uniform_map_mark_lane_shapes(function, map);

  /* Mutable locals and loops need a small monotone fixed point. Each name can
   * rise at most twice, so instruction_count+1 rounds is a hard upper bound. */
  for (size_t round = 0; round <= function->instruction_count; round++) {
    int changed = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (!ir_gpu_instruction_defines_dest(instruction) ||
          instruction->op == IR_OP_DECLARE_LOCAL) {
        continue;
      }
      IRGpuUniformSlot *slot =
          ir_gpu_uniform_slot(map, instruction->dest.name, 0);
      unsigned char rank =
          ir_gpu_instruction_result_uniformity(map, instruction);
      if (slot && rank > slot->rank) {
        slot->rank = rank;
        changed = 1;
      }
    }
    if (!changed) return 1;
  }
  return 1;

fail:
  free(map->slots);
  memset(map, 0, sizeof(*map));
  return 0;
}

static void ir_gpu_uniform_map_destroy(IRGpuUniformMap *map) {
  if (!map) return;
  free(map->slots);
  memset(map, 0, sizeof(*map));
}

static int ir_gpu_subgroup_collective(MtlcIntrinsic intrinsic) {
  return ir_intrinsic_is_subgroup(intrinsic) &&
         intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID &&
         intrinsic != MTLC_INTRINSIC_GPU_SUBGROUP_SIZE;
}

static int ir_gpu_instruction_collective_requirement(
    const IRProgram *program, const IRInstruction *instruction,
    const unsigned char *function_requirements) {
  if (!instruction) return IR_GPU_COLLECTIVE_NONE;
  if (instruction->op == IR_OP_BARRIER ||
      instruction->op == IR_OP_TENSOR_TRANSFER ||
      (instruction->op == IR_OP_CALL &&
       instruction->intrinsic == MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER)) {
    return IR_GPU_COLLECTIVE_WORKGROUP;
  }
  if (instruction->op == IR_OP_TENSOR_MMA ||
      instruction->op == IR_OP_TENSOR_MATMUL ||
      instruction->op == IR_OP_TENSOR_COMMIT) {
    return IR_TENSOR_MMA(instruction).scope == MTLC_MEMORY_SCOPE_WORKGROUP
               ? IR_GPU_COLLECTIVE_WORKGROUP
               : IR_GPU_COLLECTIVE_SUBGROUP;
  }
  if (instruction->op == IR_OP_TENSOR_EPILOGUE) {
    return IR_TENSOR_EPILOGUE(instruction).scope == MTLC_MEMORY_SCOPE_WORKGROUP
               ? IR_GPU_COLLECTIVE_WORKGROUP
               : IR_GPU_COLLECTIVE_SUBGROUP;
  }
  if (instruction->op == IR_OP_CALL &&
      ir_gpu_subgroup_collective(instruction->intrinsic)) {
    return IR_GPU_COLLECTIVE_SUBGROUP;
  }
  if (instruction->op == IR_OP_CALL &&
      instruction->intrinsic == MTLC_INTRINSIC_NONE) {
    long callee = ir_gpu_graph_function_index(program, instruction->text);
    if (callee >= 0 && function_requirements) {
      return function_requirements[(size_t)callee];
    }
  }
  return IR_GPU_COLLECTIVE_NONE;
}

static int ir_gpu_cfg_reachable(const IRFunction *function, size_t start,
                                unsigned char *reachable, size_t *queue) {
  if (!function || !reachable || !queue || start >= function->block_count)
    return 0;
  memset(reachable, 0, function->block_count);
  size_t head = 0;
  size_t tail = 0;
  reachable[start] = 1;
  queue[tail++] = start;
  while (head < tail) {
    size_t block_index = queue[head++];
    const IRBasicBlock *block = &function->blocks[block_index];
    for (size_t i = 0; i < block->successor_count; i++) {
      size_t successor = block->successors[i];
      if (successor < function->block_count && !reachable[successor]) {
        reachable[successor] = 1;
        queue[tail++] = successor;
      }
    }
  }
  return 1;
}

static const char *ir_gpu_uniformity_rank_name(unsigned char rank) {
  return rank == IR_GPU_UNIFORM_SUBGROUP
             ? "subgroup-uniform but not workgroup-uniform"
             : "work-item-varying";
}

static const char *ir_gpu_collective_name(const IRInstruction *instruction,
                                          int requirement) {
  if (instruction && instruction->op == IR_OP_TENSOR_MMA) return "tensor MMA";
  if (instruction && instruction->op == IR_OP_TENSOR_MATMUL)
    return "bounded tensor matrix operation";
  if (instruction && instruction->op == IR_OP_TENSOR_EPILOGUE)
    return "tensor epilogue";
  if (instruction && instruction->op == IR_OP_TENSOR_COMMIT)
    return "tensor accumulator commit";
  if (instruction && instruction->op == IR_OP_CALL &&
      instruction->intrinsic == MTLC_INTRINSIC_NONE) {
    return "call to collective device function";
  }
  return requirement == IR_GPU_COLLECTIVE_WORKGROUP ? "workgroup barrier"
                                                    : "subgroup collective";
}

static int ir_gpu_validate_function_uniformity(
    IRGpuGraphBuilder *builder, size_t function_index,
    const unsigned char *parameter_ranks,
    const unsigned char *function_requirements,
    unsigned char *out_requirement) {
  IRFunction *function = builder->program->functions[function_index];
  IRGpuUniformMap uniformity;
  unsigned char requirement = IR_GPU_COLLECTIVE_NONE;
  unsigned char *entry_reachable = NULL;
  unsigned char *from_first = NULL;
  unsigned char *from_second = NULL;
  unsigned char *block_divergence = NULL;
  size_t *queue = NULL;
  int ok = 0;
  if (!function || !out_requirement ||
      !ir_gpu_uniform_map_build(function, parameter_ranks, &uniformity)) {
    return ir_gpu_graph_fail(builder,
                             "out of memory analyzing GPU collective uniformity");
  }
  if (!ir_function_rebuild_cfg(function)) {
    ir_gpu_uniform_map_destroy(&uniformity);
    return ir_gpu_graph_fail(builder,
                             "could not build CFG for GPU device function '%s'",
                             function->name ? function->name : "?");
  }
  size_t blocks = function->block_count;
  size_t storage = blocks ? blocks : 1;
  entry_reachable = calloc(storage, 1);
  from_first = calloc(storage, 1);
  from_second = calloc(storage, 1);
  block_divergence = calloc(storage, 1);
  queue = malloc(storage * sizeof(*queue));
  if (!entry_reachable || !from_first || !from_second ||
      !block_divergence || !queue) {
    ir_gpu_graph_fail(builder,
                      "out of memory analyzing GPU collective control flow");
    goto done;
  }
  if (blocks &&
      !ir_gpu_cfg_reachable(function, function->entry_block, entry_reachable,
                            queue)) {
    ir_gpu_graph_fail(builder, "invalid GPU device CFG in '%s'",
                      function->name ? function->name : "?");
    goto done;
  }

  /* For a two-way branch, blocks reachable from exactly one successor are
   * control-dependent on that decision. Blocks reachable from both successors
   * are reconverged. This also catches varying-trip loops: the header/body is
   * reachable through the backedge from only the continuing successor. */
  for (size_t b = 0; b < blocks; b++) {
    IRBasicBlock *block = &function->blocks[b];
    size_t last_offset = 0;
    if (!entry_reachable[b] || block->successor_count != 2 ||
        !ir_cfg_block_last_non_nop(block, &last_offset)) {
      continue;
    }
    const IRInstruction *branch = &block->instructions[last_offset];
    if (branch->op != IR_OP_BRANCH_ZERO && branch->op != IR_OP_BRANCH_EQ)
      continue;
    unsigned char branch_rank =
        ir_gpu_uniform_operand(&uniformity, &branch->lhs);
    if (branch->op == IR_OP_BRANCH_EQ) {
      unsigned char rhs_rank =
          ir_gpu_uniform_operand(&uniformity, &branch->rhs);
      if (rhs_rank > branch_rank) branch_rank = rhs_rank;
    }
    if (branch_rank == IR_GPU_UNIFORM_WORKGROUP) continue;
    ir_gpu_cfg_reachable(function, block->successors[0], from_first, queue);
    ir_gpu_cfg_reachable(function, block->successors[1], from_second, queue);
    for (size_t controlled = 0; controlled < blocks; controlled++) {
      if (entry_reachable[controlled] &&
          (from_first[controlled] != from_second[controlled]) &&
          branch_rank > block_divergence[controlled]) {
        block_divergence[controlled] = branch_rank;
      }
    }
  }

  for (size_t b = 0; b < blocks; b++) {
    if (!entry_reachable[b]) continue;
    IRBasicBlock *block = &function->blocks[b];
    for (size_t offset = 0; offset < block->instruction_count; offset++) {
      const IRInstruction *instruction = &block->instructions[offset];
      char location[64] = {0};
      if (instruction->location.line) {
        snprintf(location, sizeof(location), " at line %llu",
                 (unsigned long long)instruction->location.line);
      }
      int needed = ir_gpu_instruction_collective_requirement(
          builder->program, instruction, function_requirements);
      if (needed > requirement) requirement = (unsigned char)needed;
      if (!needed) continue;
      unsigned char allowed = needed == IR_GPU_COLLECTIVE_WORKGROUP
                                  ? IR_GPU_UNIFORM_WORKGROUP
                                  : IR_GPU_UNIFORM_SUBGROUP;
      if (block_divergence[b] > allowed) {
        ir_gpu_graph_fail(
            builder,
            "GPU collective uniformity violation in '%s'%s: %s is control-dependent on a %s condition",
            function->name ? function->name : "?",
            location,
            ir_gpu_collective_name(instruction, needed),
            ir_gpu_uniformity_rank_name(block_divergence[b]));
        goto done;
      }
      if (instruction->op == IR_OP_TENSOR_MMA ||
          instruction->op == IR_OP_TENSOR_MATMUL ||
          instruction->op == IR_OP_TENSOR_COMMIT) {
        size_t pointer_operands =
            4u +
            (IR_TENSOR_MMA(instruction).sparsity != MTLC_TENSOR_SPARSITY_DENSE
                 ? 1u
                 : 0u) +
            (IR_TENSOR_MMA(instruction).a_scale_mode != MTLC_TENSOR_SCALE_NONE
                 ? 1u
                 : 0u) +
            (IR_TENSOR_MMA(instruction).b_scale_mode != MTLC_TENSOR_SCALE_NONE
                 ? 1u
                 : 0u);
        size_t matmul_control_base =
            instruction->op == IR_OP_TENSOR_MATMUL
                ? ir_tensor_mma_operand_count(&IR_TENSOR_MMA(instruction))
                : SIZE_MAX;
        static const char *matmul_control_names[5] = {
            "row-origin control", "column-origin control",
            "problem-M control", "problem-N control", "problem-K control"};
        for (size_t arg = 0; arg < instruction->argument_count; arg++) {
          unsigned char operand_rank = ir_gpu_uniform_operand(
              &uniformity, &instruction->arguments[arg]);
          if (operand_rank > allowed) {
            const char *operand_kind =
                arg < pointer_operands
                    ? "pointer"
                : arg >= matmul_control_base &&
                          arg - matmul_control_base < 5u
                    ? matmul_control_names[arg - matmul_control_base]
                    : "runtime stride";
            const char *tensor_name =
                instruction->op == IR_OP_TENSOR_MATMUL ? "tensor matrix"
                                                       : "tensor MMA";
            ir_gpu_graph_fail(
                builder,
                "GPU collective uniformity violation in '%s'%s: %s %s operand %llu is not %s-uniform",
                function->name ? function->name : "?",
                location,
                tensor_name,
                operand_kind,
                (unsigned long long)arg,
                needed == IR_GPU_COLLECTIVE_WORKGROUP ? "workgroup"
                                                      : "subgroup");
            goto done;
          }
        }
      }
      if (instruction->op == IR_OP_TENSOR_EPILOGUE) {
        for (size_t arg = 0; arg < instruction->argument_count; arg++) {
          unsigned char operand_rank = ir_gpu_uniform_operand(
              &uniformity, &instruction->arguments[arg]);
          if (operand_rank > allowed) {
            ir_gpu_graph_fail(
                builder,
                "GPU collective uniformity violation in '%s'%s: tensor epilogue operand %llu is not %s-uniform",
                function->name ? function->name : "?", location,
                (unsigned long long)arg,
                needed == IR_GPU_COLLECTIVE_WORKGROUP ? "workgroup"
                                                      : "subgroup");
            goto done;
          }
        }
      }
      if (instruction->op == IR_OP_TENSOR_TRANSFER) {
        for (size_t arg = 0; arg < instruction->argument_count; arg++) {
          if (ir_gpu_uniform_operand(&uniformity,
                                     &instruction->arguments[arg]) >
              IR_GPU_UNIFORM_WORKGROUP) {
            ir_gpu_graph_fail(
                builder,
                "GPU collective uniformity violation in '%s'%s: tensor transfer operand %llu is not workgroup-uniform",
                function->name ? function->name : "?", location,
                (unsigned long long)arg);
            goto done;
          }
        }
      }
      if (instruction->op == IR_OP_CALL &&
          (instruction->intrinsic ==
               MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32 ||
           instruction->intrinsic ==
               MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32) &&
          instruction->argument_count == 2 &&
          ir_gpu_uniform_operand(&uniformity, &instruction->arguments[1]) >
              IR_GPU_UNIFORM_SUBGROUP) {
        ir_gpu_graph_fail(
            builder,
            "GPU collective uniformity violation in '%s'%s: subgroup broadcast source lane is work-item-varying",
            function->name ? function->name : "?",
            location);
        goto done;
      }
    }
  }
  *out_requirement = requirement;
  ok = 1;

done:
  free(entry_reachable);
  free(from_first);
  free(from_second);
  free(block_divergence);
  free(queue);
  ir_gpu_uniform_map_destroy(&uniformity);
  return ok;
}

static void ir_gpu_free_parameter_ranks(const IRProgram *program,
                                        unsigned char **parameter_ranks) {
  if (!parameter_ranks) return;
  for (size_t i = 0; program && i < program->function_count; i++)
    free(parameter_ranks[i]);
  free(parameter_ranks);
}

static int ir_gpu_validate_collective_uniformity(IRGpuGraphBuilder *builder) {
  const IRProgram *program = builder->program;
  IRGpuCallGraph *graph = builder->graph;
  unsigned char **parameter_ranks =
      calloc(program->function_count ? program->function_count : 1,
             sizeof(*parameter_ranks));
  unsigned char *function_requirements =
      calloc(program->function_count ? program->function_count : 1, 1);
  if (!parameter_ranks || !function_requirements) {
    free(function_requirements);
    ir_gpu_free_parameter_ranks(program, parameter_ranks);
    return ir_gpu_graph_fail(builder,
                             "out of memory analyzing GPU collective contracts");
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function || !graph->reachable[i] || !function->parameter_count)
      continue;
    parameter_ranks[i] = calloc(function->parameter_count, 1);
    if (!parameter_ranks[i]) {
      free(function_requirements);
      ir_gpu_free_parameter_ranks(program, parameter_ranks);
      return ir_gpu_graph_fail(
          builder, "out of memory propagating GPU parameter uniformity");
    }
  }

  /* graph->order is callee-before-caller. Reverse it so every caller has
   * contributed its argument ranks before a helper is analyzed. */
  for (size_t remaining = graph->count; remaining > 0; remaining--) {
    size_t index = graph->order[remaining - 1];
    IRFunction *function = program->functions[index];
    IRGpuUniformMap uniformity;
    if (!ir_gpu_uniform_map_build(function, parameter_ranks[index],
                                  &uniformity)) {
      free(function_requirements);
      ir_gpu_free_parameter_ranks(program, parameter_ranks);
      return ir_gpu_graph_fail(
          builder, "out of memory propagating GPU argument uniformity");
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (instruction->op != IR_OP_CALL ||
          instruction->intrinsic != MTLC_INTRINSIC_NONE)
        continue;
      long callee_index =
          ir_gpu_graph_function_index(program, instruction->text);
      if (callee_index < 0 || !graph->reachable[(size_t)callee_index]) continue;
      IRFunction *callee = program->functions[(size_t)callee_index];
      size_t count = instruction->argument_count < callee->parameter_count
                         ? instruction->argument_count
                         : callee->parameter_count;
      for (size_t arg = 0; arg < count; arg++) {
        unsigned char rank = ir_gpu_uniform_operand(
            &uniformity, &instruction->arguments[arg]);
        if (rank > parameter_ranks[(size_t)callee_index][arg])
          parameter_ranks[(size_t)callee_index][arg] = rank;
      }
    }
    ir_gpu_uniform_map_destroy(&uniformity);
  }

  for (size_t order = 0; order < graph->count; order++) {
    size_t index = graph->order[order];
    if (!ir_gpu_validate_function_uniformity(
            builder, index, parameter_ranks[index], function_requirements,
            &function_requirements[index])) {
      free(function_requirements);
      ir_gpu_free_parameter_ranks(program, parameter_ranks);
      return 0;
    }
  }
  free(function_requirements);
  ir_gpu_free_parameter_ranks(program, parameter_ranks);
  return 1;
}

static int ir_gpu_graph_visit(IRGpuGraphBuilder *builder, size_t index) {
  const IRProgram *program = builder->program;
  IRFunction *function = program->functions[index];
  if (builder->state[index] == 2) return 1;
  if (builder->state[index] == 1) {
    return ir_gpu_graph_fail(builder,
                             "GPU device call graph is recursive at '%s'",
                             function && function->name ? function->name : "?");
  }
  if (!function) {
    return ir_gpu_graph_fail(builder, "GPU device call graph has a null body");
  }
  if (!ir_gpu_async_copy_groups_valid(program, function)) {
    return ir_gpu_graph_fail(
        builder,
        "GPU device function '%s' has an invalid asynchronous-copy contract or unbalanced group",
        function->name ? function->name : "?");
  }
  if (!ir_gpu_tensor_residency_groups_valid(function)) {
    return ir_gpu_graph_fail(
        builder,
        "GPU device function '%s' has an invalid tensor residency group",
        function->name ? function->name : "?");
  }
  builder->state[index] = 1;
  builder->graph->reachable[index] = 1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_GPU_LAUNCH) {
      return ir_gpu_graph_fail(
          builder, "GPU device function '%s' contains a host launch",
          function->name ? function->name : "?");
    }
    if (instruction->op == IR_OP_CALL_INDIRECT) {
      return ir_gpu_graph_fail(
          builder, "GPU device function '%s' contains an indirect call",
          function->name ? function->name : "?");
    }
    if (instruction->op == IR_OP_TENSOR_MMA ||
        instruction->op == IR_OP_TENSOR_COMMIT) {
      if (!ir_gpu_tensor_signature_matches(program, function, instruction)) {
        return ir_gpu_graph_fail(
            builder,
            "GPU device function '%s' has an invalid tensor MMA descriptor or operand signature",
            function->name ? function->name : "?");
      }
      continue;
    }
    if (instruction->op == IR_OP_TENSOR_MATMUL) {
      if (!ir_gpu_tensor_matmul_signature_matches(program, function,
                                                   instruction)) {
        return ir_gpu_graph_fail(
            builder,
            "GPU device function '%s' has an invalid tensor_matmul descriptor or operand signature",
            function->name ? function->name : "?");
      }
      continue;
    }
    if (instruction->op == IR_OP_TENSOR_EPILOGUE) {
      if (!ir_gpu_tensor_epilogue_signature_matches(program, function,
                                                     instruction)) {
        return ir_gpu_graph_fail(
            builder,
            "GPU device function '%s' has an invalid tensor epilogue descriptor or operand signature",
            function->name ? function->name : "?");
      }
      continue;
    }
    if (instruction->op == IR_OP_TENSOR_TRANSFER) {
      if (!ir_gpu_tensor_transfer_signature_matches(program, function,
                                                    instruction)) {
        return ir_gpu_graph_fail(
            builder,
            "GPU device function '%s' has an invalid tensor transfer descriptor or operand signature",
            function->name ? function->name : "?");
      }
      continue;
    }
    if (instruction->op == IR_OP_CALL &&
        ir_intrinsic_is_subgroup(instruction->intrinsic)) {
      if ((size_t)ir_intrinsic_arity(instruction->intrinsic) !=
              instruction->argument_count ||
          !ir_gpu_subgroup_signature_matches(program, instruction)) {
        return ir_gpu_graph_fail(
            builder,
            "GPU device function '%s' has an invalid subgroup intrinsic signature for '%s'",
            function->name ? function->name : "?",
            instruction->text ? instruction->text : "?");
      }
      continue;
    }
    if (instruction->op != IR_OP_CALL ||
        instruction->intrinsic != MTLC_INTRINSIC_NONE) {
      continue;
    }
    if (instruction->text &&
        strcmp(instruction->text, IR_SYSCALL_CALL_NAME) == 0) {
      return ir_gpu_graph_fail(
          builder,
          "GPU device function '%s' makes a system call, and a device runs "
          "where there is no operating system",
          function->name ? function->name : "?");
    }
    long callee_index =
        ir_gpu_graph_function_index(program, instruction->text);
    if (callee_index < 0) {
      return ir_gpu_graph_fail(
          builder, "GPU device function '%s' calls external or missing '%s'",
          function->name ? function->name : "?",
          instruction->text ? instruction->text : "?");
    }
    IRFunction *callee = program->functions[(size_t)callee_index];
    if (callee->is_kernel) {
      return ir_gpu_graph_fail(
          builder, "GPU device function '%s' directly calls kernel '%s'",
          function->name ? function->name : "?",
          callee->name ? callee->name : "?");
    }
    if (!ir_gpu_graph_visit(builder, (size_t)callee_index)) return 0;
  }
  builder->state[index] = 2;
  builder->graph->order[builder->graph->count++] = index;
  return 1;
}

int ir_program_build_gpu_call_graph(const IRProgram *program,
                                    IRGpuCallGraph *graph, char **error) {
  IRGpuGraphBuilder builder;
  size_t kernels = 0;
  if (error) *error = NULL;
  if (!program || !graph) return 0;
  memset(graph, 0, sizeof(*graph));
  graph->function_count = program->function_count;
  graph->order = calloc(program->function_count ? program->function_count : 1,
                        sizeof(*graph->order));
  graph->reachable =
      calloc(program->function_count ? program->function_count : 1,
             sizeof(*graph->reachable));
  unsigned char *state =
      calloc(program->function_count ? program->function_count : 1,
             sizeof(*state));
  if (!graph->order || !graph->reachable || !state) {
    free(state);
    ir_gpu_call_graph_destroy(graph);
    if (error) *error = mettle_strdup("out of memory building GPU call graph");
    return 0;
  }
  memset(&builder, 0, sizeof(builder));
  builder.program = program;
  builder.graph = graph;
  builder.state = state;
  builder.error = error;
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->is_kernel) {
      kernels++;
      if (!ir_gpu_graph_visit(&builder, i)) {
        free(state);
        ir_gpu_call_graph_destroy(graph);
        return 0;
      }
    }
  }
  free(state);
  if (kernels == 0) {
    ir_gpu_call_graph_destroy(graph);
    if (error) *error = mettle_strdup("GPU module has no kernel entry points");
    return 0;
  }
  if (!ir_gpu_validate_collective_uniformity(&builder)) {
    ir_gpu_call_graph_destroy(graph);
    return 0;
  }
  return 1;
}

void ir_gpu_call_graph_destroy(IRGpuCallGraph *graph) {
  if (!graph) return;
  free(graph->order);
  free(graph->reachable);
  memset(graph, 0, sizeof(*graph));
}

/* Open-addressed name -> function-index table for the dead-function sweep.
 * Lookups happen once per operand of every instruction, so a linear name scan
 * would go quadratic on large multi-function programs (the same trap as the
 * historical per-element strcmp compile-speed bugs). */
typedef struct {
  const char **names;
  size_t *indices;
  size_t capacity; /* power of two */
} IrFnNameTable;

static size_t ir_fn_name_hash(const char *name) {
  /* FNV-1a. */
  size_t hash = 1469598103934665603ull;
  while (*name) {
    hash ^= (unsigned char)*name++;
    hash *= 1099511628211ull;
  }
  return hash;
}

static int ir_fn_table_init(IrFnNameTable *table, size_t function_count) {
  size_t capacity = 16;
  while (capacity < function_count * 2) {
    capacity *= 2;
  }
  table->names = (const char **)calloc(capacity, sizeof(*table->names));
  table->indices = (size_t *)calloc(capacity, sizeof(*table->indices));
  table->capacity = capacity;
  if (!table->names || !table->indices) {
    free(table->names);
    free(table->indices);
    return 0;
  }
  return 1;
}

static void ir_fn_table_insert(IrFnNameTable *table, const char *name,
                               size_t index) {
  size_t slot = ir_fn_name_hash(name) & (table->capacity - 1);
  while (table->names[slot]) {
    if (strcmp(table->names[slot], name) == 0) {
      return; /* First definition wins; duplicates would be a sema error. */
    }
    slot = (slot + 1) & (table->capacity - 1);
  }
  table->names[slot] = name;
  table->indices[slot] = index;
}

/* Returns the function index for `name`, or (size_t)-1. */
static size_t ir_fn_table_lookup(const IrFnNameTable *table, const char *name) {
  size_t slot = ir_fn_name_hash(name) & (table->capacity - 1);
  while (table->names[slot]) {
    if (strcmp(table->names[slot], name) == 0) {
      return table->indices[slot];
    }
    slot = (slot + 1) & (table->capacity - 1);
  }
  return (size_t)-1;
}

static void ir_dead_fn_mark(const IrFnNameTable *table, const char *name,
                            unsigned char *live, size_t *worklist,
                            size_t *worklist_count) {
  size_t index;

  if (!name) {
    return;
  }
  index = ir_fn_table_lookup(table, name);
  if (index != (size_t)-1 && !live[index]) {
    live[index] = 1;
    worklist[(*worklist_count)++] = index;
  }
}

static void ir_dead_fn_mark_assembly(const IrFnNameTable *table,
                                     const char *assembly, unsigned char *live,
                                     size_t *worklist,
                                     size_t *worklist_count) {
  size_t i = 0;

  if (!assembly) {
    return;
  }

  while (assembly[i] != '\0') {
    if (assembly[i] == '"' || assembly[i] == '\'') {
      char quote = assembly[i];
      i++;
      while (assembly[i] != '\0' && assembly[i] != quote) {
        if (assembly[i] == '\\' && assembly[i + 1] != '\0') {
          i++;
        }
        i++;
      }
      if (assembly[i] != '\0') {
        i++;
      }
      continue;
    }

    if (isalpha((unsigned char)assembly[i]) || assembly[i] == '_') {
      size_t start = i;
      size_t length = 1;
      char *token = NULL;

      while (isalnum((unsigned char)assembly[start + length]) ||
             assembly[start + length] == '_') {
        length++;
      }

      token = (char *)malloc(length + 1);
      if (token) {
        memcpy(token, assembly + start, length);
        token[length] = '\0';
        ir_dead_fn_mark(table, token, live, worklist, worklist_count);
        free(token);
      }
      i = start + length;
      continue;
    }

    i++;
  }
}

int ir_init_image_is_all_zero(const IRModuleSymbol *symbol) {
  if (!symbol || !symbol->init_bytes || symbol->init_bytes_size == 0 ||
      symbol->init_reloc_count > 0) {
    return 0;
  }
  for (size_t i = 0; i < symbol->init_bytes_size; i++) {
    if (symbol->init_bytes[i] != 0) {
      return 0;
    }
  }
  return 1;
}

#define IR_DECLARATION_HINT_SLOTS 8192u

static struct {
  const IRFunction *function;
  size_t index;
} g_ir_declaration_hints[IR_DECLARATION_HINT_SLOTS];

/* Keyed on what the name says, not where it is stored: an operand's name is
 * its own copy, so the same symbol arrives through a different pointer at
 * almost every call and a pointer key never hits. */
static size_t ir_declaration_hint_slot(const IRFunction *function,
                                       const char *symbol_name,
                                       int symbols_only) {
  size_t length = strlen(symbol_name);
  size_t edge = length < 4 ? length : 4;
  unsigned head = 0;
  unsigned tail = 0;
  size_t mixed;

  memcpy(&head, symbol_name, edge);
  memcpy(&tail, symbol_name + length - edge, edge);
  mixed = (size_t)(const void *)function * 0x9e3779b97f4a7c15ull;
  mixed ^= ((size_t)head + ((size_t)tail << 16) + length) *
           0xff51afd7ed558ccdull;
  mixed ^= (size_t)symbols_only * 0xd6e8feb86659fd93ull;
  mixed ^= mixed >> 31;
  return (mixed >> 17) & (IR_DECLARATION_HINT_SLOTS - 1u);
}

static int ir_declaration_names(const IRInstruction *instruction,
                                const char *symbol_name, int symbols_only) {
  return instruction->op == IR_OP_DECLARE_LOCAL && instruction->text &&
         instruction->dest.name &&
         (!symbols_only || instruction->dest.kind == IR_OPERAND_SYMBOL) &&
         instruction->dest.name[0] == symbol_name[0] &&
         strcmp(instruction->dest.name, symbol_name) == 0;
}

/* One scan of a body indexes every declaration in it, which is what the
 * per-name hint below cannot do: its first lookup for each name still had to
 * walk the function, and on a body that has absorbed a thousand inlined calls
 * that was most of the compile. The index is only ever trusted through
 * ir_declaration_names against the live instruction, so a body that moved
 * under it costs a miss and never a wrong answer. */
#define IR_DECLARATION_MAP_SLOTS 4
#define IR_DECLARATION_MAP_MIN_BODY 64

typedef struct {
  unsigned any;    /* index + 1 of the first declaration of this name */
  unsigned symbol; /* index + 1 of the first with a SYMBOL destination */
} IRDeclarationBucket;

static struct {
  const IRFunction *function;
  const IRInstruction *instructions;
  size_t instruction_count;
  IRDeclarationBucket *buckets;
  size_t bucket_count;
} g_ir_declaration_maps[IR_DECLARATION_MAP_SLOTS];
static size_t g_ir_declaration_map_next;

static size_t ir_declaration_name_hash(const char *name) {
  size_t hash = METTLE_FNV1A_OFFSET_BASIS;

  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    hash ^= (size_t)*p;
    hash *= METTLE_FNV1A_PRIME;
  }
  hash ^= hash >> 31;
  return hash * 0xff51afd7ed558ccdull;
}

static size_t ir_declaration_map_build(const IRFunction *function) {
  size_t slot = g_ir_declaration_map_next % IR_DECLARATION_MAP_SLOTS;
  size_t declarations = 0;
  size_t want = 64;

  g_ir_declaration_map_next++;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_DECLARE_LOCAL) {
      declarations++;
    }
  }
  while (want < (declarations + 1) * 4) {
    want *= 2;
  }
  if (g_ir_declaration_maps[slot].bucket_count < want) {
    IRDeclarationBucket *grown =
        realloc(g_ir_declaration_maps[slot].buckets,
                want * sizeof(IRDeclarationBucket));

    if (!grown) {
      return IR_DECLARATION_MAP_SLOTS;
    }
    g_ir_declaration_maps[slot].buckets = grown;
    g_ir_declaration_maps[slot].bucket_count = want;
  }
  want = g_ir_declaration_maps[slot].bucket_count;
  memset(g_ir_declaration_maps[slot].buckets, 0,
         want * sizeof(IRDeclarationBucket));

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    size_t bucket;

    if (instruction->op != IR_OP_DECLARE_LOCAL || !instruction->dest.name) {
      continue;
    }
    bucket = ir_declaration_name_hash(instruction->dest.name) & (want - 1);
    while (g_ir_declaration_maps[slot].buckets[bucket].any) {
      const IRInstruction *held =
          &function->instructions
               [g_ir_declaration_maps[slot].buckets[bucket].any - 1];

      if (held->dest.name &&
          strcmp(held->dest.name, instruction->dest.name) == 0) {
        break;
      }
      bucket = (bucket + 1) & (want - 1);
    }
    if (!g_ir_declaration_maps[slot].buckets[bucket].any) {
      g_ir_declaration_maps[slot].buckets[bucket].any = (unsigned)(i + 1);
    }
    if (!g_ir_declaration_maps[slot].buckets[bucket].symbol &&
        instruction->dest.kind == IR_OPERAND_SYMBOL) {
      g_ir_declaration_maps[slot].buckets[bucket].symbol = (unsigned)(i + 1);
    }
  }

  g_ir_declaration_maps[slot].function = function;
  g_ir_declaration_maps[slot].instructions = function->instructions;
  g_ir_declaration_maps[slot].instruction_count = function->instruction_count;
  return slot;
}

static size_t ir_declaration_map_slot(const IRFunction *function) {
  for (size_t i = 0; i < IR_DECLARATION_MAP_SLOTS; i++) {
    if (g_ir_declaration_maps[i].function == function &&
        g_ir_declaration_maps[i].instructions == function->instructions &&
        g_ir_declaration_maps[i].instruction_count ==
            function->instruction_count) {
      return i;
    }
  }
  return ir_declaration_map_build(function);
}

static const IRInstruction *ir_declaration_map_find(const IRFunction *function,
                                                    const char *symbol_name,
                                                    int symbols_only) {
  size_t slot = ir_declaration_map_slot(function);
  size_t mask;
  size_t bucket;

  if (slot >= IR_DECLARATION_MAP_SLOTS ||
      !g_ir_declaration_maps[slot].buckets) {
    return NULL;
  }
  mask = g_ir_declaration_maps[slot].bucket_count - 1;
  bucket = ir_declaration_name_hash(symbol_name) & mask;
  while (g_ir_declaration_maps[slot].buckets[bucket].any) {
    unsigned found = symbols_only
                         ? g_ir_declaration_maps[slot].buckets[bucket].symbol
                         : g_ir_declaration_maps[slot].buckets[bucket].any;
    const IRInstruction *held =
        &function->instructions
             [g_ir_declaration_maps[slot].buckets[bucket].any - 1];

    if (held->dest.name && strcmp(held->dest.name, symbol_name) == 0) {
      if (!found || found > function->instruction_count) {
        return NULL;
      }
      held = &function->instructions[found - 1];
      return ir_declaration_names(held, symbol_name, symbols_only) ? held
                                                                   : NULL;
    }
    bucket = (bucket + 1) & mask;
  }
  return NULL;
}

/* Dead instructions are retired by turning them into NOPs, so a function that
 * has been through the pipeline is close to half empty and every later pass
 * still walks the holes. Drop the ones that carry nothing: a NOP with text is a
 * marker (`@@simd:`, `@@unroll:`) a later pass reads, and one with an origin
 * token still names a source line for debug info. Positions move, so the caller
 * owns invalidating anything that recorded one. */
size_t ir_function_drop_dead_nops(IRFunction *function) {
  size_t write = 0;
  size_t dropped;

  if (!function || !function->instructions) {
    return 0;
  }

  for (size_t read = 0; read < function->instruction_count; read++) {
    IRInstruction *instruction = &function->instructions[read];

    if (instruction->op == IR_OP_NOP && !instruction->text &&
        !instruction->ast_ref) {
      ir_instruction_destroy(instruction);
      continue;
    }
    if (write != read) {
      function->instructions[write] = *instruction;
    }
    write++;
  }

  dropped = function->instruction_count - write;
  function->instruction_count = write;
  if (dropped > 0) {
    function->cfg_valid = 0;
  }
  return dropped;
}

const IRInstruction *ir_function_find_declaration(const IRFunction *function,
                                                  const char *symbol_name,
                                                  int symbols_only) {
  size_t slot;

  if (!function || !symbol_name) {
    return NULL;
  }

  slot = ir_declaration_hint_slot(function, symbol_name, symbols_only);
  if (g_ir_declaration_hints[slot].function == function &&
      g_ir_declaration_hints[slot].index < function->instruction_count) {
    const IRInstruction *hinted =
        &function->instructions[g_ir_declaration_hints[slot].index];
    if (ir_declaration_names(hinted, symbol_name, symbols_only)) {
      return hinted;
    }
  }

  if (function->instruction_count >= IR_DECLARATION_MAP_MIN_BODY) {
    const IRInstruction *found =
        ir_declaration_map_find(function, symbol_name, symbols_only);

    if (found) {
      g_ir_declaration_hints[slot].function = function;
      g_ir_declaration_hints[slot].index =
          (size_t)(found - function->instructions);
      return found;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (ir_declaration_names(instruction, symbol_name, symbols_only)) {
      g_ir_declaration_hints[slot].function = function;
      g_ir_declaration_hints[slot].index = i;
      return instruction;
    }
  }

  return NULL;
}

int ir_program_eliminate_dead_functions(IRProgram *program, int keep_exports) {
  IrFnNameTable table;
  unsigned char *live = NULL;
  size_t *worklist = NULL;
  size_t worklist_count = 0;
  size_t n;
  size_t kept = 0;
  int found_main = 0;

  if (!program || program->function_count == 0) {
    return 1;
  }
  n = program->function_count;

  if (!ir_fn_table_init(&table, n)) {
    return 0;
  }
  live = (unsigned char *)calloc(n, 1);
  worklist = (size_t *)malloc(n * sizeof(*worklist));
  if (!live || !worklist) {
    free(table.names);
    free(table.indices);
    free(live);
    free(worklist);
    return 0;
  }

  for (size_t i = 0; i < n; i++) {
    if (program->functions[i] && program->functions[i]->name) {
      ir_fn_table_insert(&table, program->functions[i]->name, i);
      if (strcmp(program->functions[i]->name, "main") == 0) {
        live[i] = 1;
        worklist[worklist_count++] = i;
        found_main = 1;
      } else if (program->functions[i]->is_kernel ||
                 program->functions[i]->rewrite_role ||
                 (keep_exports && program->functions[i]->is_exported)) {
        /* A GPU entry point is launched by the driver against the emitted
         * module, so no instruction in this program has to name it. `export fn`
         * roots only when something outside this program is joining the link. */
        live[i] = 1;
        worklist[worklist_count++] = i;
      }
    }
  }

  /* No main means this is not a normal executable image; touch nothing. */
  if (!found_main) {
    free(table.names);
    free(table.indices);
    free(live);
    free(worklist);
    return 1;
  }

  /* A global holding a function's address is a root even though no instruction
   * names it: `var handler: fn(int32) -> int32 = &on_event;` lowers to a
   * relocation against the function, and sweeping the body away would leave
   * that relocation pointing at nothing. The traversal below only walks
   * instructions, so seed these here. */
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    ir_dead_fn_mark(&table, program->module_symbols[i].init_symbol_ref, live,
                    worklist, &worklist_count);
    /* Same for a dispatch table built as an aggregate constant: every entry is
     * a relocation against a function nothing else may mention. */
    for (size_t r = 0; r < program->module_symbols[i].init_reloc_count; r++) {
      ir_dead_fn_mark(&table, program->module_symbols[i].init_relocs[r].symbol,
                      live, worklist, &worklist_count);
    }
  }

  /* A function is referenced when any instruction of a live function carries
   * its name: in `text` (direct calls, defer captures, rewritten intrinsics),
   * in a SYMBOL operand (function-pointer uses like `run(mix)`), or in a
   * STRING operand (e.g. `dispatch` kernel-name launches). Local variables
   * shadowing a function name over-approximate to "live", which only costs
   * bytes, never correctness. */
  while (worklist_count > 0) {
    IRFunction *fn = program->functions[worklist[--worklist_count]];
    if (!fn) {
      continue;
    }
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *insn = &fn->instructions[i];
      if (insn->op == IR_OP_INLINE_ASM) {
        ir_dead_fn_mark_assembly(&table, insn->text, live, worklist,
                                 &worklist_count);
        continue;
      }
      ir_dead_fn_mark(&table, insn->text, live, worklist, &worklist_count);
      if (insn->dest.kind == IR_OPERAND_SYMBOL) {
        ir_dead_fn_mark(&table, insn->dest.name, live, worklist,
                        &worklist_count);
      }
      if (insn->lhs.kind == IR_OPERAND_SYMBOL ||
          insn->lhs.kind == IR_OPERAND_STRING) {
        ir_dead_fn_mark(&table, insn->lhs.name, live, worklist,
                        &worklist_count);
      }
      if (insn->rhs.kind == IR_OPERAND_SYMBOL ||
          insn->rhs.kind == IR_OPERAND_STRING) {
        ir_dead_fn_mark(&table, insn->rhs.name, live, worklist,
                        &worklist_count);
      }
      for (size_t a = 0; a < insn->argument_count; a++) {
        const IROperand *arg = &insn->arguments[a];
        if (arg->kind == IR_OPERAND_SYMBOL || arg->kind == IR_OPERAND_STRING) {
          ir_dead_fn_mark(&table, arg->name, live, worklist, &worklist_count);
        }
      }
    }
  }

  /* Freeing thousands of dead post-inline bodies instruction-by-instruction
   * costs real time on large programs and the process reclaims it at exit;
   * only pay for it when deep teardown is explicitly requested. */
  static int full_cleanup = -1;
  if (full_cleanup < 0) {
    full_cleanup = getenv("METTLE_FULL_CLEANUP") ? 1 : 0;
  }
  for (size_t i = 0; i < n; i++) {
    if (live[i]) {
      program->functions[kept++] = program->functions[i];
    } else if (program->functions[i]) {
      if (full_cleanup) {
        ir_function_destroy(program->functions[i]);
      }
    }
  }
  program->function_count = kept;
  program->dead_functions_eliminated = 1;

  free(table.names);
  free(table.indices);
  free(live);
  free(worklist);
  return 1;
}

int ir_program_dump(IRProgram *program, FILE *output) {
  if (!program || !output) {
    return 0;
  }

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }

    fprintf(output, "function %s {\n",
            function->name ? function->name : "<anonymous>");

    if (!ir_function_rebuild_cfg(function)) {
      return 0;
    }

    for (size_t block_i = 0; block_i < function->block_count; block_i++) {
      IRBasicBlock *block = &function->blocks[block_i];
      const char *label = block->label ? block->label
                          : block_i == function->entry_block ? "<entry>"
                                                              : "<anon>";
      size_t last_instruction =
          block->instruction_count == 0
              ? block->first_instruction
              : block->first_instruction + block->instruction_count - 1;

      fprintf(output, "  block %zu %s [%zu..%zu]", block_i, label,
              block->first_instruction, last_instruction);
      ir_dump_block_edges(output, "preds", block->predecessors,
                          block->predecessor_count);
      ir_dump_block_edges(output, "succs", block->successors,
                          block->successor_count);
      fprintf(output, "\n");

      for (size_t j = 0; j < block->instruction_count; j++) {
        size_t instruction_index = block->first_instruction + j;
        IRInstruction *instruction = &block->instructions[j];
        char buffer[1024];
        ir_format_instruction_line(instruction, buffer, sizeof(buffer));
        fprintf(output, "    %4zu: %s\n", instruction_index, buffer);
      }
    }

    fprintf(output, "}\n\n");
  }

  return 1;
}

int ir_inline_asm_binds_symbol(const char *assembly_text, const char *name) {
  size_t name_length;
  const char *at;

  if (!assembly_text || !name || !*name) {
    return 0;
  }
  name_length = strlen(name);
  for (at = strchr(assembly_text, '{'); at; at = strchr(at + 1, '{')) {
    const char *cursor = at + 1;
    while (*cursor == ' ' || *cursor == '	') {
      cursor++;
    }
    if (strncmp(cursor, name, name_length) != 0) {
      continue;
    }
    cursor += name_length;
    while (*cursor == ' ' || *cursor == '	') {
      cursor++;
    }
    if (*cursor == '}') {
      return 1;
    }
  }
  return 0;
}
