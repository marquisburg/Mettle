#include "codegen/binary/arm64.h"

#include <stddef.h>

static const Arm64Reg GP_ARG_REGS[] = {
    ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3,
    ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7};

static const Arm64Reg VEC_ARG_REGS[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const Arm64Reg GP_CALLEE_SAVED[] = {
    ARM64_X19, ARM64_X20, ARM64_X21, ARM64_X22, ARM64_X23,
    ARM64_X24, ARM64_X25, ARM64_X26, ARM64_X27, ARM64_X28};


static const Arm64Reg GP_TEMPS[] = {
    ARM64_X9,  ARM64_X10, ARM64_X11, ARM64_X12,
    ARM64_X13, ARM64_X14, ARM64_X15};

static const Arm64Reg VEC_VOLATILE[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const Arm64Reg VEC_CALLEE_SAVED[] = {8, 9, 10, 11, 12, 13, 14, 15};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const Arm64Abi AAPCS64 = {
    GP_ARG_REGS,        COUNT(GP_ARG_REGS),
    VEC_ARG_REGS,       COUNT(VEC_ARG_REGS),
    ARM64_X8,           /* indirect result location register (XR) */
    8,                  /* stack slot bytes */
    GP_CALLEE_SAVED,    COUNT(GP_CALLEE_SAVED),
    GP_TEMPS,           COUNT(GP_TEMPS),
    VEC_VOLATILE,       COUNT(VEC_VOLATILE),
    VEC_CALLEE_SAVED,   COUNT(VEC_CALLEE_SAVED),
    ARM64_X29,          /* fp */
    ARM64_X30,          /* lr */
    ARM64_SP,           /* sp (== 31) */
    ARM64_X16,          /* scratch0 / IP0 */
    ARM64_X17,          /* scratch1 / IP1 */
    ARM64_X18,          /* platform (reserved) */
};

static Arm64Reg g_gp_args[8] = {ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3,
                                ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7};
static Arm64Abi g_abi;
static int g_abi_ready;

static const Arm64Abi *arm64_abi(void) {
  if (!g_abi_ready) {
    g_abi = AAPCS64;
    g_abi.gp_arg_regs = g_gp_args;
    g_abi.gp_arg_count = COUNT(g_gp_args);
    g_abi_ready = 1;
  }
  return &g_abi;
}

const Arm64Abi *arm64_aapcs64(void) { return arm64_abi(); }

int arm64_abi_set_gp_args(const Arm64Reg *regs, int count) {
  int i;
  int j;
  if (!regs || count < 1 || count > COUNT(g_gp_args)) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    if (regs[i] > ARM64_X7) {
      return 0;
    }
    for (j = 0; j < i; j++) {
      if (regs[j] == regs[i]) {
        return 0;
      }
    }
  }
  (void)arm64_abi();
  for (i = 0; i < count; i++) {
    g_gp_args[i] = regs[i];
  }
  g_abi.gp_arg_count = count;
  return 1;
}

void arm64_abi_reset(void) {
  int i;
  (void)arm64_abi();
  for (i = 0; i < COUNT(g_gp_args); i++) {
    g_gp_args[i] = (Arm64Reg)i;
  }
  g_abi.gp_arg_count = COUNT(g_gp_args);
}

int arm64_reg_is_callee_saved(Arm64Reg r) {
  return (r >= ARM64_X19 && r <= ARM64_X28) || r == ARM64_X29;
}

int arm64_reg_is_volatile(Arm64Reg r) {
  if (r >= ARM64_X0 && r <= ARM64_X18) {
    return 1;
  }
  return r == ARM64_X30;
}

int arm64_reg_arg_index(Arm64Reg r) {
  const Arm64Abi *abi = arm64_abi();
  int i;
  for (i = 0; i < abi->gp_arg_count; i++) {
    if (abi->gp_arg_regs[i] == r) {
      return i;
    }
  }
  return -1;
}

int arm64_reg_is_allocatable(Arm64Reg r) {
  if (r == ARM64_SP || r == ARM64_X29 || r == ARM64_X30) {
    return 0;
  }
  if (r == ARM64_X16 || r == ARM64_X17 || r == ARM64_X18) {
    return 0;
  }
  return r <= ARM64_X28;
}

int arm64_compute_arg_layout(const int *is_float, int count,
                             Arm64ArgLocation *out, int *stack_bytes_out) {
  if (count < 0 || (count > 0 && (!is_float || !out))) {
    return 0;
  }

  const Arm64Abi *abi = arm64_abi();
  int gp_used = 0;
  int vec_used = 0;
  int stack_cursor = 0;

  for (int i = 0; i < count; i++) {
    Arm64ArgLocation *loc = &out[i];
    if (is_float[i]) {
      if (vec_used < abi->vec_arg_count) {
        loc->kind = ARM64_ARG_IN_VEC_REGISTER;
        loc->reg = abi->vec_arg_regs[vec_used++];
        loc->stack_offset = 0;
        continue;
      }
    } else {
      if (gp_used < abi->gp_arg_count) {
        loc->kind = ARM64_ARG_IN_GP_REGISTER;
        loc->reg = abi->gp_arg_regs[gp_used++];
        loc->stack_offset = 0;
        continue;
      }
    }
    loc->kind = ARM64_ARG_ON_STACK;
    loc->reg = ARM64_SP;
    loc->stack_offset = stack_cursor;
    stack_cursor += abi->stack_slot_bytes;
  }

  if (stack_bytes_out) {
    *stack_bytes_out = stack_cursor;
  }
  return 1;
}
