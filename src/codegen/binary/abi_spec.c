/* Calling-convention descriptor selection and argument-layout computation for
 * the binary backend. Centralizes every place the MS-x64 and SysV AMD64
 * conventions differ so the rest of the backend is convention-agnostic. */

#include "codegen/binary/internal.h"

#include <stddef.h>
#include <string.h>

/* MS-x64: first four args by position in RCX/RDX/R8/R9 (or XMM0..3), 32-byte
 * shadow space, INDIRECT out-pointer in RCX. */
static const BinaryGpRegister MS_X64_INT_PARAMS[] = {
    BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8, BINARY_GP_R9};
static const BinaryXmmRegister MS_X64_FLOAT_PARAMS[] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3};

static const BinaryAbi MS_X64_ABI = {
    MS_X64_INT_PARAMS,        4,
    MS_X64_FLOAT_PARAMS,      4,
    32,                       /* shadow space */
    BINARY_GP_RCX,            /* INDIRECT return out-pointer */
    0,                        /* shared positional slot for int+float */
};

/* SysV AMD64: up to six integer args in RDI/RSI/RDX/RCX/R8/R9 and up to eight
 * float args in XMM0..7, counted independently; no shadow space; INDIRECT
 * out-pointer in RDI. */
static const BinaryGpRegister SYSV_INT_PARAMS[] = {
    BINARY_GP_RDI, BINARY_GP_RSI, BINARY_GP_RDX,
    BINARY_GP_RCX, BINARY_GP_R8,  BINARY_GP_R9};
static const BinaryXmmRegister SYSV_FLOAT_PARAMS[] = {
    BINARY_XMM0, BINARY_XMM1, BINARY_XMM2, BINARY_XMM3,
    BINARY_XMM4, BINARY_XMM5, BINARY_XMM6, BINARY_XMM7};

static const BinaryAbi SYSV_ABI = {
    SYSV_INT_PARAMS,          6,
    SYSV_FLOAT_PARAMS,        8,
    0,                        /* no shadow space */
    BINARY_GP_RDI,            /* INDIRECT return out-pointer */
    1,                        /* separate int/float register sequences */
};

static const BinaryAbi *g_active_abi = &MS_X64_ABI;

static BinaryGpRegister g_described_int[16];
static BinaryXmmRegister g_described_float[16];
static BinaryAbi g_described_abi;
static int g_described_valid;

void code_generator_binary_describe_abi(const BinaryGpRegister *int_regs,
                                        size_t int_count,
                                        const BinaryXmmRegister *float_regs,
                                        size_t float_count, int shadow_space,
                                        BinaryGpRegister indirect_return,
                                        int separate_classes) {
  size_t i;
  if (!int_regs || !float_regs || int_count == 0 || int_count > 16 ||
      float_count == 0 || float_count > 16) {
    g_described_valid = 0;
    return;
  }
  for (i = 0; i < int_count; i++) {
    g_described_int[i] = int_regs[i];
  }
  for (i = 0; i < float_count; i++) {
    g_described_float[i] = float_regs[i];
  }
  g_described_abi.int_param_registers = g_described_int;
  g_described_abi.int_param_count = int_count;
  g_described_abi.float_param_registers = g_described_float;
  g_described_abi.float_param_count = float_count;
  g_described_abi.shadow_space_size = shadow_space;
  g_described_abi.indirect_return_register = indirect_return;
  g_described_abi.counts_classes_separately = separate_classes ? 1 : 0;
  g_described_valid = 1;
}

void code_generator_binary_select_abi(BinaryTargetFormat format) {
  if (g_described_valid) {
    g_active_abi = &g_described_abi;
    return;
  }
  switch (format) {
  case BINARY_TARGET_FORMAT_ELF_X64:
    g_active_abi = &SYSV_ABI;
    break;
  case BINARY_TARGET_FORMAT_COFF_WIN64:
  default:
    g_active_abi = &MS_X64_ABI;
    break;
  }
}

const BinaryAbi *code_generator_binary_active_abi(void) { return g_active_abi; }

int code_generator_binary_function_is_abi_public(CodeGenerator *generator,
                                                 const char *name) {
  const CgSym *symbol = NULL;
  IRFunction *function = NULL;

  if (!name) {
    return 0;
  }
  /* The startup object calls main, and a C caller may too. */
  if (strcmp(name, "main") == 0) {
    return 1;
  }

  symbol = code_generator_lookup_symbol(generator, name);
  if (symbol && symbol->is_extern) {
    return 1;
  }

  /* No body in this compilation means the definition is on the other side of
   * a link, whether it was spelled `extern` or synthesized for a runtime
   * helper. Either way the platform's rule is the only one both sides can
   * agree on. */
  function = code_generator_find_ir_function_binary(generator, name);
  if (!function) {
    return 1;
  }
  return function->is_exported;
}

/* SysV merges two classes for the same eightbyte by taking the stronger one.
 * INTEGER beats SSE, which is why a struct holding an int and a float in the
 * same 8 bytes travels in a general register. */
static BinaryEightbyteClass binary_merge_eightbyte(BinaryEightbyteClass a,
                                                   BinaryEightbyteClass b) {
  if (a == b) {
    return a;
  }
  if (a == BINARY_EIGHTBYTE_NONE) {
    return b;
  }
  if (b == BINARY_EIGHTBYTE_NONE) {
    return a;
  }
  return BINARY_EIGHTBYTE_INTEGER;
}

/* Walks every scalar leaf of `type` at `base` and folds its class into the
 * eightbyte it lands in. Arrays and nested structs recurse; a leaf wider than
 * its own eightbyte (nothing the frontend builds today) still marks both. */
static void binary_classify_fields(MtlcType *type, size_t base,
                                   BinaryEightbyteClass *classes,
                                   size_t eightbyte_count) {
  size_t i = 0;

  if (!type) {
    return;
  }

  if (type->kind == MTLC_TYPE_STRUCT && type->field_count > 0 &&
      type->field_types && type->field_offsets) {
    for (i = 0; i < type->field_count; i++) {
      binary_classify_fields(type->field_types[i],
                             base + type->field_offsets[i], classes,
                             eightbyte_count);
    }
    return;
  }

  if (type->kind == MTLC_TYPE_ARRAY && type->base_type &&
      type->base_type->size > 0) {
    for (i = 0; i < type->array_size; i++) {
      binary_classify_fields(type->base_type, base + i * type->base_type->size,
                             classes, eightbyte_count);
    }
    return;
  }

  {
    /* A scalar leaf. Floats classify SSE, everything else INTEGER. A tagged
     * enum carries a discriminant, so it is INTEGER whatever the payload is. */
    BinaryEightbyteClass leaf =
        (type->kind == MTLC_TYPE_FLOAT32 || type->kind == MTLC_TYPE_FLOAT64)
            ? BINARY_EIGHTBYTE_SSE
            : BINARY_EIGHTBYTE_INTEGER;
    size_t first = base / 8u;
    size_t last = type->size > 0 ? (base + type->size - 1u) / 8u : first;

    for (i = first; i <= last && i < eightbyte_count; i++) {
      classes[i] = binary_merge_eightbyte(classes[i], leaf);
    }
  }
}

int code_generator_binary_classify_sysv_aggregate(MtlcType *type,
                                                  BinarySysvAggregate *out) {
  size_t size = 0;
  size_t i = 0;

  if (!out) {
    return 0;
  }
  out->in_memory = 0;
  out->size = 0;
  out->eightbyte_count = 0;
  out->classes[0] = BINARY_EIGHTBYTE_NONE;
  out->classes[1] = BINARY_EIGHTBYTE_NONE;

  if (!type || !code_generator_type_is_aggregate(type)) {
    return 0;
  }

  size = code_generator_abi_type_size(type);
  out->size = size;
  if (size == 0) {
    return 0;
  }
  if (size > 16u) {
    out->in_memory = 1;
    return 1;
  }

  out->eightbyte_count = (size + 7u) / 8u;
  binary_classify_fields(type, 0u, out->classes, out->eightbyte_count);

  /* An eightbyte no field reached is padding. Nothing reads it, so INTEGER is
   * the cheaper carrier. */
  for (i = 0; i < out->eightbyte_count; i++) {
    if (out->classes[i] == BINARY_EIGHTBYTE_NONE) {
      out->classes[i] = BINARY_EIGHTBYTE_INTEGER;
    }
  }
  return 1;
}

int code_generator_binary_compute_arg_layout(const BinaryAbi *abi,
                                             const int *is_float, size_t count,
                                             BinaryArgLocation *locations_out,
                                             int *stack_bytes_out) {
  return code_generator_binary_compute_arg_layout_ex(
      abi, is_float, NULL, NULL, count, locations_out, stack_bytes_out);
}

int code_generator_binary_compute_arg_layout_ex(
    const BinaryAbi *abi, const int *is_float, const int *force_stack,
    const size_t *stack_slots, size_t count,
    BinaryArgLocation *locations_out, int *stack_bytes_out) {
  if (!abi || (!is_float && count > 0) || (!locations_out && count > 0)) {
    return 0;
  }

  size_t int_used = 0;
  size_t float_used = 0;
  /* Positional slot index for the MS-x64 shared sequence. */
  size_t positional = 0;
  int stack_cursor = 0;

  for (size_t i = 0; i < count; i++) {
    int wants_float = is_float[i] ? 1 : 0;
    int wants_stack = force_stack && force_stack[i];
    size_t slots = (stack_slots && stack_slots[i] > 0) ? stack_slots[i] : 1u;
    BinaryArgLocation *loc = &locations_out[i];

    if (wants_stack) {
      /* MEMORY class: on the stack by value, whatever registers are left. */
      loc->kind = BINARY_ARG_ON_STACK;
      loc->stack_offset = stack_cursor;
      stack_cursor += (int)(slots * BINARY_FUNCTION_STACK_SLOT_SIZE);
      positional++;
      continue;
    }

    if (abi->counts_classes_separately) {
      /* SysV: each class draws from its own register pool; overflow spills to
       * the stack in argument order. */
      if (wants_float) {
        if (float_used < abi->float_param_count) {
          loc->kind = BINARY_ARG_IN_XMM_REGISTER;
          loc->xmm_register = abi->float_param_registers[float_used++];
          continue;
        }
      } else {
        if (int_used < abi->int_param_count) {
          loc->kind = BINARY_ARG_IN_GP_REGISTER;
          loc->gp_register = abi->int_param_registers[int_used++];
          continue;
        }
      }
      loc->kind = BINARY_ARG_ON_STACK;
      loc->stack_offset = stack_cursor;
      stack_cursor += (int)(slots * BINARY_FUNCTION_STACK_SLOT_SIZE);
    } else {
      /* MS-x64: one positional slot indexes both register files; slots beyond
       * the register count go on the stack at (slot - regcount) * 8. The int
       * and float register tables have the same length here. */
      size_t reg_count = abi->int_param_count;
      if (positional < reg_count) {
        if (wants_float) {
          loc->kind = BINARY_ARG_IN_XMM_REGISTER;
          loc->xmm_register = abi->float_param_registers[positional];
        } else {
          loc->kind = BINARY_ARG_IN_GP_REGISTER;
          loc->gp_register = abi->int_param_registers[positional];
        }
      } else {
        loc->kind = BINARY_ARG_ON_STACK;
        loc->stack_offset =
            (int)((positional - reg_count) * BINARY_FUNCTION_STACK_SLOT_SIZE);
        if (loc->stack_offset + BINARY_FUNCTION_STACK_SLOT_SIZE > stack_cursor) {
          stack_cursor = loc->stack_offset + BINARY_FUNCTION_STACK_SLOT_SIZE;
        }
      }
      positional++;
    }
  }

  if (stack_bytes_out) {
    *stack_bytes_out = stack_cursor;
  }
  return 1;
}
