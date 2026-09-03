#ifndef CODEGEN_BINARY_ARM64_H
#define CODEGEN_BINARY_ARM64_H

/* AArch64 (A64) instruction encoder + decoder + AAPCS64 register file. Each
 * encoder is a pure function returning the 32-bit instruction word. `is64`
 * selects the 64-bit (sf=1) vs 32-bit form. */

#include <stdint.h>

/* Register 31 is context-dependent: XZR (zero) in data-processing slots, SP in
 * load/store and add/sub-immediate base slots. The instruction form decides. */
typedef enum {
  ARM64_X0 = 0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5,
  ARM64_X6, ARM64_X7, ARM64_X8, ARM64_X9, ARM64_X10, ARM64_X11,
  ARM64_X12, ARM64_X13, ARM64_X14, ARM64_X15, ARM64_X16, ARM64_X17,
  ARM64_X18, ARM64_X19, ARM64_X20, ARM64_X21, ARM64_X22, ARM64_X23,
  ARM64_X24, ARM64_X25, ARM64_X26, ARM64_X27, ARM64_X28,
  ARM64_X29 = 29, /* FP */
  ARM64_X30 = 30, /* LR */
  ARM64_XZR = 31,
  ARM64_SP = 31
} Arm64Reg;

typedef enum {
  ARM64_EQ = 0, ARM64_NE = 1, ARM64_CS = 2, ARM64_CC = 3,
  ARM64_MI = 4, ARM64_PL = 5, ARM64_VS = 6, ARM64_VC = 7,
  ARM64_HI = 8, ARM64_LS = 9, ARM64_GE = 10, ARM64_LT = 11,
  ARM64_GT = 12, ARM64_LE = 13, ARM64_AL = 14, ARM64_NV = 15
} Arm64Cond;

Arm64Cond arm64_invert_cond(Arm64Cond c);

uint32_t arm64_nop(void);
/* Register-register move (ORR Xd, XZR, Xm). Reg 31 is XZR here, so this CANNOT
 * move to/from SP -- use arm64_mov_sp (or arm64_emit_mov) when SP is involved. */
uint32_t arm64_mov_reg(int is64, Arm64Reg rd, Arm64Reg rm);
/* Move to/from SP (ADD rd, rn, #0). The only correct "mov" when either operand
 * is SP, since reg 31 reads as SP in the add-immediate slot. Always 64-bit. */
uint32_t arm64_mov_sp(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_movz(int is64, Arm64Reg rd, uint16_t imm16, int hw);
uint32_t arm64_movn(int is64, Arm64Reg rd, uint16_t imm16, int hw);
uint32_t arm64_movk(int is64, Arm64Reg rd, uint16_t imm16, int hw);

uint32_t arm64_add_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_sub_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_and_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_orr_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_eor_reg(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_neg(int is64, Arm64Reg rd, Arm64Reg rm);
uint32_t arm64_mvn(int is64, Arm64Reg rd, Arm64Reg rm);

/* imm12 in 0..4095; shift12 applies LSL #12. Base slot is SP-capable. */
uint32_t arm64_add_imm(int is64, Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                       int shift12);
uint32_t arm64_sub_imm(int is64, Arm64Reg rd, Arm64Reg rn, uint32_t imm12,
                       int shift12);

uint32_t arm64_cmp_reg(int is64, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_cmp_imm(int is64, Arm64Reg rn, uint32_t imm12, int shift12);
/* TST rn, rm == ANDS XZR, rn, rm (flags = rn & rm). */
uint32_t arm64_tst(int is64, Arm64Reg rn, Arm64Reg rm);

/* Sign/zero extend a narrow value (UXTB/UXTH zero-extend into Wd; SXTB/SXTH/
 * SXTW sign-extend into Xd). */
uint32_t arm64_uxtb(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_uxth(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxtb(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxth(Arm64Reg rd, Arm64Reg rn);
uint32_t arm64_sxtw(Arm64Reg rd, Arm64Reg rn);

/* MADD: rd = ra + rn*rm ; MSUB: rd = ra - rn*rm. */
uint32_t arm64_madd(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Reg ra);
uint32_t arm64_msub(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Reg ra);
uint32_t arm64_mul(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_sdiv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_udiv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);

uint32_t arm64_lslv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsrv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_asrv(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm);
uint32_t arm64_lsl_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_lsr_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift);
uint32_t arm64_asr_imm(int is64, Arm64Reg rd, Arm64Reg rn, int shift);

uint32_t arm64_csel(int is64, Arm64Reg rd, Arm64Reg rn, Arm64Reg rm,
                    Arm64Cond cond);
uint32_t arm64_cset(int is64, Arm64Reg rd, Arm64Cond cond);

/* offset_bytes: multiple of the access size (8/4), packed into the 12-bit
 * scaled field. Base slot is SP-capable. */
uint32_t arm64_ldr_imm(int is64, Arm64Reg rt, Arm64Reg rn, int offset_bytes);
uint32_t arm64_str_imm(int is64, Arm64Reg rt, Arm64Reg rn, int offset_bytes);
/* Store/load the low byte of rt at [rn, #offset] (STRB/LDRB zero-extending,
 * unscaled 0..4095). */
uint32_t arm64_strb_imm(Arm64Reg rt, Arm64Reg rn, int offset);
uint32_t arm64_ldrb_imm(Arm64Reg rt, Arm64Reg rn, int offset);
/* Halfword (16-bit) store/load (zero-extending); offset is bytes, scaled by 2. */
uint32_t arm64_strh_imm(Arm64Reg rt, Arm64Reg rn, int offset_bytes);
uint32_t arm64_ldrh_imm(Arm64Reg rt, Arm64Reg rn, int offset_bytes);

/* ---- scalar floating point (d = 64-bit double, s = 32-bit single) -------- *
 * FP registers reuse the 0..31 numbering; `is_double` selects the d/s form. */
uint32_t arm64_fadd(int is_double, int fd, int fn, int fm);
uint32_t arm64_fsub(int is_double, int fd, int fn, int fm);
uint32_t arm64_fmul(int is_double, int fd, int fn, int fm);
uint32_t arm64_fdiv(int is_double, int fd, int fn, int fm);
uint32_t arm64_fneg(int is_double, int fd, int fn);
uint32_t arm64_fcmp(int is_double, int fn, int fm);            /* -> NZCV */
uint32_t arm64_scvtf(int is_double, int fd, Arm64Reg xn);      /* int64 -> fp */
uint32_t arm64_fcvtzs(int is_double, Arm64Reg xd, int fn);     /* fp -> int64 trunc */
uint32_t arm64_ucvtf(int is_double, int fd, Arm64Reg xn);     /* uint64 -> fp */
uint32_t arm64_fcvtzu(int is_double, Arm64Reg xd, int fn);    /* fp -> uint64 trunc */
uint32_t arm64_fcvt(int to_double, int fd, int fn);
uint32_t arm64_fcvt_h2s(int sd, int hn);
uint32_t arm64_fcvt_s2h(int hd, int sn);            /* single<->double */
uint32_t arm64_fmov_gp(int is_double, int fd, Arm64Reg xn);    /* bits GP -> FP */
uint32_t arm64_fmov_to_gp(int is_double, Arm64Reg xd, int fn); /* bits FP -> GP */
uint32_t arm64_fmov_reg(int is_double, int fd, int fn);        /* FP -> FP copy */
/* FP load/store of a d/s register; offset is bytes, scaled by 8 (d) / 4 (s). */
uint32_t arm64_ldr_fp(int is_double, int ft, Arm64Reg rn, int offset_bytes);
uint32_t arm64_str_fp(int is_double, int ft, Arm64Reg rn, int offset_bytes);
uint32_t arm64_stp_pre(int is64, Arm64Reg rt, Arm64Reg rt2, Arm64Reg rn,
                       int offset_bytes);
uint32_t arm64_ldp_post(int is64, Arm64Reg rt, Arm64Reg rt2, Arm64Reg rn,
                        int offset_bytes);

/* offset_bytes: signed displacement from this instruction (multiple of 4). */
uint32_t arm64_b(int offset_bytes);
uint32_t arm64_bl(int offset_bytes);
uint32_t arm64_bcond(Arm64Cond cond, int offset_bytes);
uint32_t arm64_cbz(int is64, Arm64Reg rt, int offset_bytes);
uint32_t arm64_cbnz(int is64, Arm64Reg rt, int offset_bytes);
uint32_t arm64_br(Arm64Reg rn);
uint32_t arm64_blr(Arm64Reg rn);
uint32_t arm64_ret(Arm64Reg rn);

/* Decoder: a structural decode (not text) so round-trip tests can compare the
 * decoded fields back to the encoder inputs. */
typedef enum {
  ARM64_DIS_UNKNOWN = 0,
  ARM64_DIS_NOP, ARM64_DIS_MOV, ARM64_DIS_MOVZ, ARM64_DIS_MOVN, ARM64_DIS_MOVK,
  ARM64_DIS_ADD_REG, ARM64_DIS_SUB_REG, ARM64_DIS_AND_REG, ARM64_DIS_ORR_REG,
  ARM64_DIS_EOR_REG, ARM64_DIS_ADD_IMM, ARM64_DIS_SUB_IMM, ARM64_DIS_SUBS_REG,
  ARM64_DIS_SUBS_IMM, ARM64_DIS_MADD, ARM64_DIS_MSUB, ARM64_DIS_SDIV,
  ARM64_DIS_UDIV, ARM64_DIS_LSLV, ARM64_DIS_LSRV, ARM64_DIS_ASRV,
  ARM64_DIS_UBFM, ARM64_DIS_SBFM, ARM64_DIS_CSEL, ARM64_DIS_CSINC,
  ARM64_DIS_LDR_IMM, ARM64_DIS_STR_IMM, ARM64_DIS_STP, ARM64_DIS_LDP,
  ARM64_DIS_B, ARM64_DIS_BL, ARM64_DIS_BCOND, ARM64_DIS_CBZ, ARM64_DIS_CBNZ,
  ARM64_DIS_BR, ARM64_DIS_BLR, ARM64_DIS_RET
} Arm64DisOp;

typedef struct {
  Arm64DisOp op;
  int is64;
  int rd, rn, rm, rt, rt2, ra;
  long long imm;
  int hw;
  int cond;
  int immr, imms;
} Arm64Inst;

Arm64Inst arm64_decode(uint32_t word);

/* AAPCS64 register file for the MIR allocator. x8 is the dedicated indirect-
 * result register (no arg-slot shift, unlike Win64/SysV). x16/x17 are encoder
 * scratch; x18 is platform-reserved. Vector pools cover scalar float/double;
 * v8..v15 preserve only their low 64 bits. NEON allocation is a later brick. */
typedef enum {
  ARM64_ARG_IN_GP_REGISTER = 0,
  ARM64_ARG_IN_VEC_REGISTER = 1,
  ARM64_ARG_ON_STACK = 2
} Arm64ArgKind;

typedef struct {
  Arm64ArgKind kind;
  Arm64Reg reg;
  int stack_offset;
} Arm64ArgLocation;

typedef struct {
  const Arm64Reg *gp_arg_regs; /* x0..x7 */
  int gp_arg_count;
  const Arm64Reg *vec_arg_regs; /* v0..v7 */
  int vec_arg_count;
  Arm64Reg indirect_result_reg; /* x8 */
  int stack_slot_bytes;

  const Arm64Reg *gp_callee_saved; /* x19..x28, also the cross-call pool */
  int gp_callee_saved_count;
  const Arm64Reg *gp_temps; /* x9..x15 */
  int gp_temps_count;
  const Arm64Reg *vec_volatile; /* v0..v7 */
  int vec_volatile_count;
  const Arm64Reg *vec_callee_saved; /* v8..v15 */
  int vec_callee_saved_count;

  Arm64Reg fp, lr, sp;
  Arm64Reg scratch0, scratch1; /* x16/x17 */
  Arm64Reg platform;           /* x18 */
} Arm64Abi;

const Arm64Abi *arm64_aapcs64(void);

int arm64_reg_is_callee_saved(Arm64Reg r);
int arm64_reg_is_volatile(Arm64Reg r);
int arm64_reg_arg_index(Arm64Reg r); /* 0..7 for x0..x7, else -1 */
int arm64_reg_is_allocatable(Arm64Reg r);

/* GP args fill x0..x7, FP args fill v0..v7 (counted independently); overflow
 * spills to 8-byte stack slots. Returns 0 on bad input. */
int arm64_compute_arg_layout(const int *is_float, int count,
                             Arm64ArgLocation *out, int *stack_bytes_out);

#endif /* CODEGEN_BINARY_ARM64_H */
