#ifndef MAIN_H
#define MAIN_H

#include "parser/parser.h"
#include "codegen/code_generator.h"
#include "debug/debug_info.h"
#include "error/error_explain.h"
#include "error/error_reporter.h"
#include "ir/ir_comptime.h"
#include "ir/ir_explain_safety.h"
#include "ir/ir_pgo.h"
#include "ir/ir_safety.h"
#include "ir/ir_verify.h"
#include "lexer/lexer.h"
#include "semantic/register_allocator.h"
#include "semantic/symbol_table.h"
#include "semantic/monomorphize.h"
#include "semantic/type_checker.h"
#include <stddef.h>

typedef enum {
  LINKER_MODE_AUTO = 0,
  LINKER_MODE_INTERNAL,
  LINKER_MODE_GCC,
  LINKER_MODE_MSVC,
} LinkerMode;

typedef struct {
  const char *input_filename;
  const char *output_filename;
  int debug_mode;
  int dump_ast;
  int dump_ir;
  int ml_opt; /* --ml-opt: run the learned ML optimizer pass on the IR */
  int emit_ptx; /* --emit-ptx: lower declared kernels to PTX .entry, no object */
  /* --emit-kernel-decls[=path]: alongside the PTX, write the host-side
     `extern kernel` declaration for every kernel, so a host that imports it
     cannot disagree with the module it launches. */
  const char *emit_kernel_decls;
  const char *ptx_target; /* backend-only target such as sm_121a */
  int ptx_isa_major;
  int ptx_isa_minor;
  int ptx_tensor_tuple_budget; /* 0=arch default; backend-only residency knob */
  int report_occupancy; /* --report-occupancy: with --emit-ptx, run ptxas -v
                           and print per-kernel registers plus the resulting
                           occupancy ceiling */
  int report_sms; /* --sms=N: SM count for the occupancy report's whole-card
                     fill thresholds; 0 = ask the local driver */
  int gpu_checks; /* --gpu-checks: emit kernel-side assertion traps */
  int report_launches; /* --report-launches: list every dispatch site */
  int report_gpu_types; /* --report-gpu-types: what the device type
                         * analyses concluded, and what they cost */
  int emit_spirv; /* --emit-spirv: lower declared kernels to SPIR-V, no object */
  int emit_arm64; /* --emit-arm64: lower scalar functions to an AArch64 ELF */
  /* --emit-arm64-obj: emit an AArch64 relocatable object instead of a host
   * one. This is the path an ARM host takes by default; the flag makes it
   * reachable (and testable) from an x86-64 host too. */
  int emit_arm64_obj;
  int optimize;
  int release;
  /* --safe: check every memory access that cannot be proved in bounds, at any
   * optimization level. Unlike the debug-build null and bounds traps, this is
   * not dropped by --release; what --release changes is how many of the checks
   * survive the proving step. */
  int safe;
  int simd_report; /* --simd-report: note what each `@simd` loop became */
  int explain;     /* --explain: report optimization decisions (vectorization,
                      inlining) for the main input file, with reasons */
  int explain_all; /* --explain-all: drop the focus filter (whole program) */
  int explain_json; /* --explain-json: machine-readable .explain.json sidecar */
  const char *explain_filter; /* --explain=SELECTOR: narrow the prose report */
  int annotate_asm; /* --annotate-asm: emit asm annotated with codegen decisions
                       (a listing on stdout + <stem>.annot.json sidecar) */
  int asm_syntax;   /* 0=intel, 1=att, 2=both (matches MirAnnotSyntax) */
  /* LLM-facing focused codegen queries (imply --annotate-asm). When a line
   * range is set the annotator prints a compact report for just those source
   * lines (asm + cost + covering loops + live registers + decisions) instead of
   * the full listing; the hot query prints the program's top hotspots. */
  int annotate_q_lo; /* --annotate-lines=A-B: first source line (0 = unset) */
  int annotate_q_hi; /* last source line of the range */
  const char *annotate_q_fn; /* --annotate-fn=NAME: restrict to one function */
  int annotate_hot; /* --annotate-hot[=N]: top-N hotspots (0 = unset) */
  /* --pgo: zero-run profile-guided optimization - interpret main() at
   * compile time and feed measured call frequencies to the optimizer. */
  int pgo;
  /* `mettle test`: run every @test function in the compile-time interpreter
   * instead of generating code. */
  int test_mode;
  const char *test_filter; /* --filter=SUBSTR: run matching tests only */
  /* `mettle trace <file> <fn> [args...]`: interpret one function on the given
   * arguments and print a line-by-line value trace. */
  const char *trace_function;
  const char *const *trace_args;
  size_t trace_arg_count;
  /* `mettle expand <file>`: print the program as source after compile-time
   * expansion, instead of generating code. */
  int expand_mode;
  /* `mettle swap-check <file> --old F --new G`: run the differential harness
   * over two functions instead of over one function before and after a pass,
   * answering whether G could replace F at a call boundary. */
  int swap_check_mode;
  const char *swap_old_name;
  const char *swap_new_name;
  /* --report-expansion: print what each `comptime for` site cost. */
  int report_expansion;
  /* --expansion-budget=N: fail the build if expansion generates more than N
   * nodes. `expansion_budget_set` distinguishes "no budget" from a budget of
   * zero, which is a meaningful thing to require. */
  size_t expansion_budget;
  int expansion_budget_set;
  int report_rules;
  long long rule_budget;
  int rule_budget_set;
  /* The checker that owns std/rule's records, handed to the trace-rule hook
     so it can lay out a Trace while the tests are running. */
  void *trace_rule_checker;
  int report_twins;
  int apply_rule_fixes;
  int why_mode;
  const char *why_subject;
  const char *why_what;
  int report_proofs;
  long long proof_budget;
  int proof_budget_set;
  int report_effects;
  long long effect_budget;
  int effect_budget_set;
  const char *target_desc_path;
  int report_target;
  int check_proofs;
  int check_tasks;
  int check_deadlines;
  int check_overflow;
  int record_trace;
  const char *check_trace_path;
  int machine_mode;
  int emulate_mode;
  int report_deadlines;
  int check_effects;
  int check_purity_fault;
  int emit_object;
  int generate_debug_symbols;
  int generate_line_mapping;
  int generate_stack_trace_support;
  /* On by default: a program that faults names the fault, the address, and the
   * function it happened in, rather than dying silently. It costs a table of
   * one record per function and the installed handler, and it leaves the
   * register-allocating backend alone -- unlike -s / -d, which additionally
   * record a location per instruction and so must use the baseline emitter.
   * --no-crash-report turns it off. */
  int generate_crash_report;
  const char *debug_format; // "dwarf", "stabs", or "map"
  const char **import_directories;
  size_t import_directory_count;
  const char **link_arguments;
  size_t link_argument_count;
  /* -l<name> / --library <name>: shared objects the ELF link binds against.
     A value naming a path is taken as that file; a bare name is looked up as
     lib<name>.so along --library-path and then the platform directories. */
  const char **shared_libraries;
  size_t shared_library_count;
  /* -L<dir> / --library-path <dir> */
  const char **library_search_paths;
  size_t library_search_path_count;
  /* --rpath <dir>: written to DT_RUNPATH, colon joined. */
  const char **runpaths;
  size_t runpath_count;
  /* --shared: emit ET_DYN with a DT_SONAME instead of a program. */
  int shared_output;
  /* --export-dynamic: publish the program's globals for libraries to bind. */
  int export_dynamic;
  const char *soname;
  /* --dynamic-linker <path>: what PT_INTERP names. */
  const char *dynamic_linker;
  const char *stdlib_directory;
  int prelude;
  int profile;
  int profile_runtime;
  int profile_runtime_ops;
  int profile_blocks; /* --profile-blocks: per-basic-block execution counters
                         dumped to a .mprof sidecar for the VTune-style codegen
                         view; implies --profile-runtime. */
  int debug_hooks; /* --debug-hooks: interactive debugger instrumentation */
  int native_heap;
  int tracy;
  int static_link;
  int musl_link;
  const char *tracy_directory;
  int debug_compiler;
  int main_wants_argc_argv;
  /* Set when this compile feeds a --build executable link (as opposed to a
   * bare --emit-obj library object). Gates whole-program transforms that are
   * only sound when `main` is the single entry point, e.g. dead-function
   * elimination. */
  int building_executable;
  /* --subsystem=windows: mark the PE as a GUI image so Windows does not create
   * a console window for it. Ignored on non-PE targets. */
  int windows_subsystem;
  /* --target <triple>: the machine the output runs on, which may not be this
   * one. Selects the object format, the calling convention and the width the
   * code generators and the inline assembler emit for. */
  const char *target_triple;
  /* --image-base <addr>: where the linked image is loaded, replacing the
   * format's default. A freestanding image is placed where its loader puts
   * it, not where a hosted operating system would. */
  unsigned long long image_base;
  int image_base_set;
  /* --emit-flat <file>: a raw image with no object or executable container,
   * laid out at --image-base. This is what a boot sector or a ROM is. */
  const char *flat_output;
  LinkerMode linker_mode;
} CompilerOptions;

int compile_file(const char *input_filename, const char *output_filename,
                 CompilerOptions *options);
void print_usage(const char *program_name);
char *read_file(const char *filename);

#endif // MAIN_H
