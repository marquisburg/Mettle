#include "register_allocator.h"

#include <stdlib.h>
#include <string.h>

RegisterAllocator *register_allocator_create(void) {
  RegisterAllocator *allocator = malloc(sizeof(RegisterAllocator));
  if (!allocator) {
    return NULL;
  }

  allocator->calling_convention = NULL;

#ifdef _WIN32
  register_allocator_set_calling_convention(allocator, CALLING_CONV_MS_X64);
#else
  register_allocator_set_calling_convention(allocator, CALLING_CONV_SYSV);
#endif

  return allocator;
}

void register_allocator_destroy(RegisterAllocator *allocator) {
  if (!allocator) {
    return;
  }

  if (allocator->calling_convention) {
    register_allocator_destroy_calling_convention_spec(
        allocator->calling_convention);
  }
  free(allocator);
}

void register_allocator_set_calling_convention(RegisterAllocator *allocator,
                                               CallingConvention convention) {
  if (!allocator) {
    return;
  }

  if (allocator->calling_convention) {
    register_allocator_destroy_calling_convention_spec(
        allocator->calling_convention);
  }

  allocator->calling_convention =
      register_allocator_get_calling_convention_spec(convention);
}

CallingConventionSpec *
register_allocator_get_calling_convention_spec(CallingConvention convention) {
  CallingConventionSpec *spec = malloc(sizeof(CallingConventionSpec));
  if (!spec) {
    return NULL;
  }

  spec->convention = convention;

  if (convention == CALLING_CONV_SYSV) {
    spec->int_param_count = 6;
    spec->int_param_registers =
        malloc(spec->int_param_count * sizeof(x86Register));
    spec->int_param_registers[0] = REG_RDI;
    spec->int_param_registers[1] = REG_RSI;
    spec->int_param_registers[2] = REG_RDX;
    spec->int_param_registers[3] = REG_RCX;
    spec->int_param_registers[4] = REG_R8;
    spec->int_param_registers[5] = REG_R9;

    spec->float_param_count = 8;
    spec->float_param_registers =
        malloc(spec->float_param_count * sizeof(x86Register));
    for (int i = 0; i < 8; i++) {
      spec->float_param_registers[i] = REG_XMM0 + i;
    }

    spec->int_return_register = REG_RAX;
    spec->float_return_register = REG_XMM0;

    spec->caller_saved_count = 19;
    spec->caller_saved_registers =
        malloc(spec->caller_saved_count * sizeof(x86Register));
    spec->caller_saved_registers[0] = REG_RAX;
    spec->caller_saved_registers[1] = REG_RCX;
    spec->caller_saved_registers[2] = REG_RDX;
    spec->caller_saved_registers[3] = REG_RSI;
    spec->caller_saved_registers[4] = REG_RDI;
    spec->caller_saved_registers[5] = REG_R8;
    spec->caller_saved_registers[6] = REG_R9;
    spec->caller_saved_registers[7] = REG_R10;
    spec->caller_saved_registers[8] = REG_R11;
    for (int i = 0; i < 10; i++) {
      spec->caller_saved_registers[9 + i] = REG_XMM0 + i;
    }

    spec->callee_saved_count = 6;
    spec->callee_saved_registers =
        malloc(spec->callee_saved_count * sizeof(x86Register));
    spec->callee_saved_registers[0] = REG_RBX;
    spec->callee_saved_registers[1] = REG_RBP;
    spec->callee_saved_registers[2] = REG_R12;
    spec->callee_saved_registers[3] = REG_R13;
    spec->callee_saved_registers[4] = REG_R14;
    spec->callee_saved_registers[5] = REG_R15;

    spec->stack_alignment = 16;
    spec->shadow_space_size = 0;
  } else if (convention == CALLING_CONV_MS_X64) {
    spec->int_param_count = 4;
    spec->int_param_registers =
        malloc(spec->int_param_count * sizeof(x86Register));
    spec->int_param_registers[0] = REG_RCX;
    spec->int_param_registers[1] = REG_RDX;
    spec->int_param_registers[2] = REG_R8;
    spec->int_param_registers[3] = REG_R9;

    spec->float_param_count = 4;
    spec->float_param_registers =
        malloc(spec->float_param_count * sizeof(x86Register));
    spec->float_param_registers[0] = REG_XMM0;
    spec->float_param_registers[1] = REG_XMM1;
    spec->float_param_registers[2] = REG_XMM2;
    spec->float_param_registers[3] = REG_XMM3;

    spec->int_return_register = REG_RAX;
    spec->float_return_register = REG_XMM0;

    spec->caller_saved_count = 13;
    spec->caller_saved_registers =
        malloc(spec->caller_saved_count * sizeof(x86Register));
    spec->caller_saved_registers[0] = REG_RAX;
    spec->caller_saved_registers[1] = REG_RCX;
    spec->caller_saved_registers[2] = REG_RDX;
    spec->caller_saved_registers[3] = REG_R8;
    spec->caller_saved_registers[4] = REG_R9;
    spec->caller_saved_registers[5] = REG_R10;
    spec->caller_saved_registers[6] = REG_R11;
    for (int i = 0; i < 6; i++) {
      spec->caller_saved_registers[7 + i] = REG_XMM0 + i;
    }

    spec->callee_saved_count = 16;
    spec->callee_saved_registers =
        malloc(spec->callee_saved_count * sizeof(x86Register));
    spec->callee_saved_registers[0] = REG_RBX;
    spec->callee_saved_registers[1] = REG_RBP;
    spec->callee_saved_registers[2] = REG_RDI;
    spec->callee_saved_registers[3] = REG_RSI;
    spec->callee_saved_registers[4] = REG_R12;
    spec->callee_saved_registers[5] = REG_R13;
    spec->callee_saved_registers[6] = REG_R14;
    spec->callee_saved_registers[7] = REG_R15;
    for (int i = 0; i < 8; i++) {
      spec->callee_saved_registers[8 + i] = REG_XMM6 + i;
    }

    spec->stack_alignment = 16;
    spec->shadow_space_size = 32;
  } else {
    free(spec);
    return NULL;
  }

  return spec;
}

void register_allocator_destroy_calling_convention_spec(
    CallingConventionSpec *spec) {
  if (!spec) {
    return;
  }

  free(spec->int_param_registers);
  free(spec->float_param_registers);
  free(spec->caller_saved_registers);
  free(spec->callee_saved_registers);
  free(spec);
}

RegisterClass register_allocator_get_register_class(CallingConventionSpec *spec,
                                                    x86Register reg) {
  if (!spec || reg == REG_NONE) {
    return REG_CLASS_RESERVED;
  }

  if (reg == REG_RSP || reg == REG_RBP) {
    return REG_CLASS_RESERVED;
  }

  for (size_t i = 0; i < spec->int_param_count; i++) {
    if (spec->int_param_registers[i] == reg) {
      return REG_CLASS_PARAMETER;
    }
  }
  for (size_t i = 0; i < spec->float_param_count; i++) {
    if (spec->float_param_registers[i] == reg) {
      return REG_CLASS_PARAMETER;
    }
  }

  if (reg == spec->int_return_register || reg == spec->float_return_register) {
    return REG_CLASS_RETURN;
  }

  for (size_t i = 0; i < spec->caller_saved_count; i++) {
    if (spec->caller_saved_registers[i] == reg) {
      return REG_CLASS_CALLER_SAVED;
    }
  }

  for (size_t i = 0; i < spec->callee_saved_count; i++) {
    if (spec->callee_saved_registers[i] == reg) {
      return REG_CLASS_CALLEE_SAVED;
    }
  }

  return REG_CLASS_RESERVED;
}

int register_allocator_is_caller_saved(CallingConventionSpec *spec,
                                       x86Register reg) {
  if (!spec) {
    return 0;
  }

  for (size_t i = 0; i < spec->caller_saved_count; i++) {
    if (spec->caller_saved_registers[i] == reg) {
      return 1;
    }
  }
  return 0;
}

int register_allocator_is_callee_saved(CallingConventionSpec *spec,
                                       x86Register reg) {
  if (!spec) {
    return 0;
  }

  for (size_t i = 0; i < spec->callee_saved_count; i++) {
    if (spec->callee_saved_registers[i] == reg) {
      return 1;
    }
  }
  return 0;
}

int register_allocator_is_floating_point_type(Type *type) {
  if (!type) {
    return 0;
  }

  return type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64 ||
         type->kind == TYPE_FLOAT16 || type->kind == TYPE_BFLOAT16;
}

const char *register_allocator_register_name(x86Register reg) {
  static const char *register_names[] = {
      "rax",  "rbx",  "rcx",  "rdx",  "rsi",  "rdi",  "rsp",  "rbp",
      "r8",   "r9",   "r10",  "r11",  "r12",  "r13",  "r14",  "r15",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
      "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
      "none"};

  if (reg >= 0 && reg <= REG_NONE) {
    return register_names[reg];
  }
  return "unknown";
}
