/* The semantic decisions both backends have to make the same way.
 *
 * `mir_lower.c` covers 29 IR opcodes and `emit.c` covers 53, and every one of
 * MIR's 29 is also implemented in the fallback, so correctness depends on the
 * two agreeing. That agreement was maintained by hand, and it drifted: `-x`
 * lowered as `0 - x` in both, which is right for every float except zero,
 * where IEEE 754 asks for -0.0 and a subtract yields +0.0. One decision,
 * written twice, wrong twice, and fixed twice.
 *
 * What belongs here is a decision about what the language means, not about how
 * to encode it. The condition code a comparison takes is a decision: `<` on
 * signed operands means SETL and on unsigned means SETB, and a backend that
 * disagrees miscompiles silently. Which register the result lands in is not.
 *
 * A new decision that both backends need goes here, so a fix cannot land in
 * one and miss the other.
 */

#include "internal.h"

#include <string.h>

/* Negation is a sign-bit flip, not a subtract from zero.
 *
 * `0 - x` is right for every float except zero: IEEE 754 says -(+0.0) is -0.0
 * while 0.0 - 0.0 is +0.0. It cannot flip the sign of a NaN either, because
 * an arithmetic operation returns its NaN operand with the sign it arrived
 * with. Flipping the bit is also one instruction rather than two and needs no
 * zero in the constant pool.
 *
 * The mask is the bit pattern of negative zero at that width, which is the
 * sign bit and nothing else. */
uint64_t binary_semantics_float_sign_mask(int float_bits) {
  if (float_bits == 32) {
    return 0x80000000ull;
  }
  return 0x8000000000000000ull;
}

/* The SETcc opcode byte a comparison takes.
 *
 * Signedness is the whole question: `<` is SETL on signed operands and SETB on
 * unsigned ones, and picking the wrong one is a wrong answer rather than a
 * crash. Returns zero when `op` is not a comparison, which is also how callers
 * ask whether it is one. */
int binary_semantics_condition_code(const char *op, int is_unsigned,
                                    unsigned char *out) {
  unsigned char code;
  if (!op || !out) {
    return 0;
  }
  if (strcmp(op, "==") == 0) {
    code = 0x94; /* sete */
  } else if (strcmp(op, "!=") == 0) {
    code = 0x95; /* setne */
  } else if (strcmp(op, "<") == 0) {
    code = is_unsigned ? 0x92 : 0x9C; /* setb : setl */
  } else if (strcmp(op, "<=") == 0) {
    code = is_unsigned ? 0x96 : 0x9E; /* setbe : setle */
  } else if (strcmp(op, ">") == 0) {
    code = is_unsigned ? 0x97 : 0x9F; /* seta : setg */
  } else if (strcmp(op, ">=") == 0) {
    code = is_unsigned ? 0x93 : 0x9D; /* setae : setge */
  } else {
    return 0;
  }
  *out = code;
  return 1;
}

int binary_semantics_is_comparison(const char *op) {
  unsigned char ignored;
  return binary_semantics_condition_code(op, 0, &ignored);
}
