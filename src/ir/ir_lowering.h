/* ir_lowering.h - the AST->IR lowering boundary (a FRONTEND concern).
 *
 * Lowering is the one backend-adjacent stage that must see the frontend's AST and
 * type system, so its entry points live here rather than in the frontend-free
 * ir.h. The reference Mettle frontend (and its lowering TUs, ir_lower_*.c) include
 * this header; the backend proper (optimizer, codegen, linker) does not.
 *
 * These prototypes were previously declared in ir.h; they moved here when the IR
 * core was made frontend-independent for libmtlc. */
#ifndef IR_LOWERING_H
#define IR_LOWERING_H

#include "ir.h"
#include "../parser/ast.h"
#include "../semantic/symbol_table.h"
#include "../semantic/type_checker.h"

/* Lower a type-checked AST program into a backend IR program. Returns NULL on
 * failure with *error_message set. emit_runtime_checks selects whether bounds/
 * null/overflow checks are lowered (off for --release and GPU/arm64 targets).
 * emit_safety_checks is --safe, which marks every memory access for
 * ir_safety_resolve_program() and is independent of optimization level. */
IRProgram *ir_lower_program(ASTNode *program, TypeChecker *type_checker,
                            SymbolTable *symbol_table, char **error_message,
                            int emit_runtime_checks, int emit_safety_checks);

/* --explain: when enabled, lowering brackets EVERY loop (not just `@simd` ones)
 * with report-only markers (SIMD_ATTR_REPORT) so the optimizer can report what
 * became of each one. Set by the driver before ir_lower_program; only meaningful
 * when optimization will run. */
void ir_lowering_set_explain(int enabled);

void ir_lowering_set_refinement_checks(int enabled);

#endif /* IR_LOWERING_H */
