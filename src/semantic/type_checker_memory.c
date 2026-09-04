// Compile-time memory diagnostics: the analyses that catch the bugs people
// otherwise find at 2am with a hex dump.
//
// Two phases share one walker:
//
// PHASE 1 (per function, during declaration checking, scope still live):
//   M0101  use of a pointer after a direct free(p)              warning
//   M0102  double free (both frees direct)                     warning
//   M0103  returning the address of a stack local               ERROR
//   M0104  storing the address of a stack local in a global     warning
//   M0105  constant array index out of bounds (backstop)        ERROR
//   M0106  constant-size memory op overflowing a stack array    ERROR
//
// PHASE 2 (whole program, after every declaration type-checks):
//   Ownership summaries are INFERRED per function and iterated to fixpoint
//   over the call graph (imports are flattened, so the compiler sees the
//   whole program): which parameters a function definitely frees, which it
//   keeps a reference to, and whether its return value is a fresh
//   allocation the caller owns. The walker then re-runs with summaries:
//
//   M0108  use after a CALL freed the pointer (`consume(p); p[0]`) warning
//   M0109  double free where a call did one of the frees          warning
//   M0107  leak: an allocation that never escapes and is never
//          freed -- now seen THROUGH borrowing helpers, and fed by
//          wrapper allocators (`make() -> malloc(...)`)           warning
//
// The borrow-lifetime and constant-fault findings share the walker:
//   M0110  borrowed interior pointer outlives its scope           warning
//   M0111  borrow invalidated by realloc                          warning
//   M0112  borrow invalidated by free                             warning
//   M0113  dereference of a provably null pointer                 warning
//   M0114  dereference of an unmapped constant address            warning
//   M0115  shift count at or past the operand width               warning
//   M0116  division or modulo by a constant zero                   ERROR
//   M0117  loop index runs past the end of the array               ERROR
//   M0121  a task is handed a pointer into the frame that spawns it ERROR
//   M0122  a message written after it was handed to a task          ERROR
//
// Every finding reports under its own code, not the generic E0003, so
// `mettle explain M0107` works on the diagnostic the reader is looking at.
//
// The analysis is deliberately conservative: definite-bug states are only
// set on the function's straight-line spine, anything inside a branch or
// loop demotes to "maybe" and stays silent, and only DEFINITE summaries
// propagate across calls. A diagnostic from this file is meant to be
// trusted, so the false-positive budget is zero.

#include "type_checker_internal.h"
#include "../ir/ir_explain_memory.h"
#include "codegen/target.h"

#define MEM_MAX_LOCALS 256
#define MEM_MAX_PARAMS 32
#define MEM_SUMMARY_MAX_ITER 8

typedef enum {
  MEM_FREED_NO = 0,
  MEM_FREED_DEFINITE = 1, /* freed on the spine: later use IS a bug */
  MEM_FREED_MAYBE = 2     /* freed inside a branch/loop: stay silent */
} MemFreedState;

/* Why a borrow's referent is gone, set on the spine and reported at the next
 * use of the borrowing pointer (the borrow-lifetime checker). */
typedef enum {
  BORROW_OK = 0,
  BORROW_SCOPE,   /* the stack referent's lexical block exited        M0110 */
  BORROW_REALLOC, /* the heap buffer was realloc'd (may have moved)   M0111 */
  BORROW_FREE     /* the heap buffer was freed (interior pointer)     M0112 */
} BorrowKill;

typedef enum {
  MEM_MODE_LOCAL = 0,  /* phase 1: direct facts only; any call escapes */
  MEM_MODE_SUMMARY,    /* phase 2a: collect a function's summary, no reports */
  MEM_MODE_INTERPROC   /* phase 2b: report summary-dependent diagnostics */
} MemMode;

/* Inferred ownership facts about one function. Bit i refers to parameter i
 * (parameters past MEM_MAX_PARAMS are treated as stored). */
typedef struct {
  const char *name;
  FunctionDeclaration *fn; /* NULL for seeded externs (malloc, free, ...) */
  unsigned frees_definite; /* unconditionally frees param i (spine) */
  unsigned frees_maybe;    /* may free param i (branch, or maybe-callee) */
  unsigned stores;         /* keeps a reference to param i */
  int returns_fresh;       /* every returned value is a fresh allocation */
} MemFnSummary;

typedef struct {
  MemFnSummary *items;
  size_t count;
  /* Open-addressing index (slot+1; 0 = empty) over items, keyed by name.
   * Consulted per call expression across every function walk: a linear scan
   * here is O(calls x functions) on large programs. Sized once at table
   * construction (the item set is fixed after seeding). */
  size_t *buckets;
  size_t bucket_count;
} MemSummaryTable;

typedef struct {
  const char *name;      /* AST-owned */
  const char *type_name; /* AST-owned */
  SourceLocation decl_loc;
  int param_index; /* -1 for locals */
  int is_stack;   /* array/struct/scalar local: its address dies with the frame */
  int is_pointer; /* trailing '*', cstring, or string (which carries a pointer) */
  int reassigned; /* a parameter overwritten since entry (summary collection) */
  MemFreedState freed;
  SourceLocation freed_loc;
  const char *freed_via; /* callee whose summary freed it; NULL = direct free */
  int holds_alloc; /* assigned from an allocator on the spine */
  SourceLocation alloc_loc;
  const char *alloc_via; /* wrapper allocator name; NULL = malloc/calloc/new */
  int escaped;     /* returned, stored, kept by a callee, or address taken */
  int ever_freed;  /* a free of this pointer appears ANYWHERE (defers count) */
  const char *points_to_stack; /* stack local whose address it holds, or NULL */
  size_t points_to_slot; /* that local's index plus one; 0 when unrecorded */
  int out_of_scope; /* its lexical block has exited; a later `&name` is not it */
  int scope_level; /* lexical block depth where this local was declared */
  const char *borrows_heap;    /* heap-pointer local this points into via
                                  `&buf[i]`; NULL when not a heap borrow */
  BorrowKill borrow_dangling;  /* referent gone; report on the next use */
  SourceLocation borrow_killed_loc; /* where the referent's lifetime ended */
  const char *borrow_killed_via;    /* referent/buffer name for the message */
  int alias_group; /* >0: shares a heap block with same-group locals (`q = p`);
                      0 = singleton. Freeing/realloc'ing one invalidates all. */
  const char *freed_alias; /* the aliasing name through which the shared block
                              was freed; NULL = this name was freed directly */
  int is_null;     /* pointer assigned a null constant on the spine */
  SourceLocation null_loc;
  int is_wild;     /* pointer assigned a small constant address (never mappable) */
  long long wild_value;
  SourceLocation wild_loc;
  long long points_to_offset; /* element offset into points_to_stack; -1 unknown */
  int has_const_value;    /* integer local with a known spine value */
  long long const_value;  /* (drives the loop-bound analysis) */
  int handed_over;
  SourceLocation handover_loc;
  const char *handover_via;
} MemLocal;

typedef struct {
  TypeChecker *checker;
  FunctionDeclaration *fn;
  SourceLocation fn_loc;
  MemMode mode;
  const MemSummaryTable *summaries; /* NULL in MEM_MODE_LOCAL */
  MemFnSummary *collect;            /* MEM_MODE_SUMMARY output */
  size_t local_count;
  int depth;      /* 0 = the function's straight-line spine */
  int scope_level; /* lexical block nesting (0 = function body); independent of
                      `depth`, which tracks branch conditionality */
  int next_alias_group; /* monotone id source for pointer alias groups */
  int in_defer;   /* defers run at scope exit: record facts, never report */
  int in_condition; /* inside an if/while/for condition: a mentioned pointer
                       counts as guarded, ending definite-null knowledge */
  int fn_returns_pointer;
  int saw_value_return;
  int returns_all_fresh;
  int had_error;
  /* Kept LAST so mem_ctx_init can zero only the fields above it. This is 256
   * entries of a large struct -- around 50KB -- and mem_add_local zeroes each
   * slot as it hands it out, so clearing the whole array once per function was
   * pure waste: 2.5% of total compile time on a 13k-function input. Every read
   * of a slot is bounded by local_count, so untouched slots are never seen. */
  MemLocal locals[MEM_MAX_LOCALS];
} MemCtx;

/* ---- small type helpers ----------------------------------------------------- */

static int mem_type_is_pointer(const char *type_name) {
  size_t len = type_name ? strlen(type_name) : 0;
  if (len == 0) {
    return 0;
  }
  if (type_name[len - 1] == '*') {
    return 1;
  }
  return strcmp(type_name, "cstring") == 0 ||
         strcmp(type_name, "rawptr") == 0;
}

static long long mem_scalar_size(MemCtx *ctx, const char *type_name) {
  if (!type_name) {
    return 0;
  }
  if (mem_type_is_pointer(type_name)) {
    return 8;
  }
  if (strcmp(type_name, "int8") == 0 || strcmp(type_name, "uint8") == 0 ||
      strcmp(type_name, "bool") == 0) {
    return 1;
  }
  if (strcmp(type_name, "int16") == 0 || strcmp(type_name, "uint16") == 0) {
    return 2;
  }
  if (strcmp(type_name, "int32") == 0 || strcmp(type_name, "uint32") == 0 ||
      strcmp(type_name, "float32") == 0) {
    return 4;
  }
  if (strcmp(type_name, "int64") == 0 || strcmp(type_name, "uint64") == 0 ||
      strcmp(type_name, "float64") == 0) {
    return 8;
  }
  if (strcmp(type_name, "string") == 0) {
    return 16;
  }
  Type *type = type_checker_get_type_by_name(ctx->checker, (char *)type_name);
  return type && type->size > 0 ? (long long)type->size : 0;
}

/* Parse `Elem[N]` out of a declared type. Returns 1 with the element count
 * and byte size on success. Multi-dimensional arrays are left alone. */
static int mem_array_extent(MemCtx *ctx, const char *type_name,
                            long long *count_out, long long *elem_size_out) {
  const char *bracket = type_name ? strchr(type_name, '[') : NULL;
  if (!bracket || bracket == type_name || strchr(bracket + 1, '[')) {
    return 0;
  }
  char *end = NULL;
  long long n = strtoll(bracket + 1, &end, 10);
  if (!end || *end != ']' || n <= 0) {
    return 0;
  }
  char elem[96];
  size_t elem_len = (size_t)(bracket - type_name);
  if (elem_len >= sizeof(elem)) {
    return 0;
  }
  memcpy(elem, type_name, elem_len);
  elem[elem_len] = '\0';
  long long elem_size = mem_scalar_size(ctx, elem);
  if (elem_size <= 0) {
    return 0;
  }
  *count_out = n;
  *elem_size_out = elem_size;
  return 1;
}

/* ---- summary table ------------------------------------------------------------ */

static MemFnSummary *mem_summary_find(const MemSummaryTable *table,
                                      const char *name) {
  if (!table || !name) {
    return NULL;
  }
  if (table->bucket_count) {
    size_t b = mettle_fnv1a_hash(name) & (table->bucket_count - 1);
    while (table->buckets[b]) {
      MemFnSummary *s = &table->items[table->buckets[b] - 1];
      if (strcmp(s->name, name) == 0) {
        return s;
      }
      b = (b + 1) & (table->bucket_count - 1);
    }
    return NULL;
  }
  for (size_t i = 0; i < table->count; i++) {
    if (strcmp(table->items[i].name, name) == 0) {
      return &table->items[i];
    }
  }
  return NULL;
}

/* Register items[slot] in the name index (no-op when the index is absent). */
static void mem_summary_index_put(MemSummaryTable *table, size_t slot) {
  if (!table->bucket_count) {
    return;
  }
  size_t b =
      mettle_fnv1a_hash(table->items[slot].name) & (table->bucket_count - 1);
  while (table->buckets[b]) {
    b = (b + 1) & (table->bucket_count - 1);
  }
  table->buckets[b] = slot + 1;
}

/* ---- local table ------------------------------------------------------------- */

static MemLocal *mem_referent(MemCtx *ctx, MemLocal *borrow);

static MemLocal *mem_find_local(MemCtx *ctx, const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = ctx->local_count; i > 0; i--) {
    if (!ctx->locals[i - 1].out_of_scope &&
        strcmp(ctx->locals[i - 1].name, name) == 0) {
      return &ctx->locals[i - 1];
    }
  }
  for (size_t i = ctx->local_count; i > 0; i--) {
    if (strcmp(ctx->locals[i - 1].name, name) == 0) {
      return &ctx->locals[i - 1];
    }
  }
  return NULL;
}

static MemLocal *mem_referent(MemCtx *ctx, MemLocal *borrow) {
  if (!ctx || !borrow || !borrow->points_to_stack) {
    return NULL;
  }
  if (borrow->points_to_slot > 0 &&
      borrow->points_to_slot <= ctx->local_count) {
    return &ctx->locals[borrow->points_to_slot - 1];
  }
  return mem_find_local(ctx, borrow->points_to_stack);
}

static MemLocal *mem_add_local(MemCtx *ctx, const char *name,
                               const char *type_name, SourceLocation loc,
                               int param_index) {
  if (!name || !type_name || ctx->local_count >= MEM_MAX_LOCALS) {
    return NULL;
  }
  MemLocal *local = &ctx->locals[ctx->local_count++];
  memset(local, 0, sizeof(*local));
  local->name = name;
  local->type_name = type_name;
  local->decl_loc = loc;
  local->param_index = param_index;
  local->is_pointer = mem_type_is_pointer(type_name);
  local->is_stack = param_index < 0 && !local->is_pointer &&
                    strncmp(type_name, "fn", 2) != 0;
  local->scope_level = ctx->scope_level;
  return local;
}

/* ---- diagnostics -------------------------------------------------------------- */

/* Every finding here reports under its own M-code rather than the generic
 * E0003 its ErrorType implies, so the reader can run `mettle explain M0107`
 * on the diagnostic in front of them. The code travels to three places at
 * once: the diagnostic, the --explain memory section, and the JSON sidecar. */
static void mem_warn(MemCtx *ctx, const char *code, SourceLocation loc,
                     const char *fmt, ...) {
  char message[512];
  va_list args;
  if (ctx->mode == MEM_MODE_SUMMARY) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  error_reporter_add_warning(ctx->checker->error_reporter, ERROR_SEMANTIC, loc,
                             message);
  error_reporter_set_last_code(ctx->checker->error_reporter, code);
  ir_explain_memory_note(
      error_reporter_current_filename(ctx->checker->error_reporter), 0,
      loc.line, code, message, NULL);
}

static void mem_warn_suggest(MemCtx *ctx, const char *code, SourceLocation loc,
                             const char *suggestion, const char *fmt, ...) {
  char message[512];
  va_list args;
  if (ctx->mode == MEM_MODE_SUMMARY) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  error_reporter_add_warning_with_suggestion(ctx->checker->error_reporter,
                                             ERROR_SEMANTIC, loc, message,
                                             suggestion);
  error_reporter_set_last_code(ctx->checker->error_reporter, code);
  ir_explain_memory_note(
      error_reporter_current_filename(ctx->checker->error_reporter), 0,
      loc.line, code, message, suggestion);
}

static void mem_error(MemCtx *ctx, const char *code, SourceLocation loc,
                      const char *suggestion, const char *fmt, ...) {
  char message[512];
  va_list args;
  if (ctx->mode != MEM_MODE_LOCAL) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  error_reporter_add_error_with_suggestion(ctx->checker->error_reporter,
                                           ERROR_SEMANTIC, loc, message,
                                           suggestion);
  error_reporter_set_last_code(ctx->checker->error_reporter, code);
  ir_explain_memory_note(
      error_reporter_current_filename(ctx->checker->error_reporter), 1,
      loc.line, code, message, suggestion);
  ctx->checker->has_error = 1;
  ctx->had_error = 1;
}

static void mem_error_late(MemCtx *ctx, const char *code, SourceLocation loc,
                           const char *suggestion, const char *fmt, ...) {
  char message[512];
  va_list args;
  if (ctx->mode != MEM_MODE_INTERPROC) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  error_reporter_add_error_with_suggestion(ctx->checker->error_reporter,
                                           ERROR_SEMANTIC, loc, message,
                                           suggestion);
  error_reporter_set_last_code(ctx->checker->error_reporter, code);
  ir_explain_memory_note(
      error_reporter_current_filename(ctx->checker->error_reporter), 1,
      loc.line, code, message, suggestion);
  ctx->checker->has_error = 1;
  ctx->had_error = 1;
}

/* ---- expression classification ----------------------------------------------- */

static ASTNode *mem_unwrap_cast(ASTNode *node) {
  int guard = 0;
  while (node && node->type == AST_CAST_EXPRESSION && guard++ < 8) {
    CastExpression *cast = (CastExpression *)node->data;
    node = cast ? cast->operand : NULL;
  }
  return node;
}

/* The stack local at the root of `&expr` (e.g. `&buf`, `&buf[i]`,
 * `&point.x`), or NULL when the expression is not an address of frame
 * memory. A dereference anywhere in the chain breaks it: `&p[i]` where p is
 * a pointer addresses the pointee. When `offset_out` is non-NULL it receives
 * the ELEMENT offset for the two exactly-understood shapes (`&arr` is 0,
 * `&arr[const]` is the constant) and -1 for everything else. */
static MemLocal *mem_addr_of_stack_at(MemCtx *ctx, ASTNode *expr,
                                      long long *offset_out) {
  if (offset_out) {
    *offset_out = -1;
  }
  expr = mem_unwrap_cast(expr);
  if (!expr || expr->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  UnaryExpression *unary = (UnaryExpression *)expr->data;
  if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
    return NULL;
  }

  /* the two offset-tracked shapes first */
  ASTNode *direct = mem_unwrap_cast(unary->operand);
  if (offset_out && direct && direct->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)direct->data;
    MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
    if (local && local->is_stack) {
      *offset_out = 0;
      return local;
    }
    return NULL;
  }
  if (offset_out && direct && direct->type == AST_INDEX_EXPRESSION) {
    ArrayIndexExpression *index = (ArrayIndexExpression *)direct->data;
    ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
    if (array && array->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)array->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      long long constant = 0;
      if (local && local->is_stack) {
        if (type_checker_eval_integer_constant_with_checker(
                ctx->checker, index->index, &constant) &&
            constant >= 0) {
          *offset_out = constant;
        }
        return local;
      }
      return NULL;
    }
  }

  ASTNode *node = unary->operand;
  int guard = 0;
  while (node && guard++ < 16) {
    node = mem_unwrap_cast(node);
    if (!node) {
      return NULL;
    }
    if (node->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)node->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      return (local && local->is_stack) ? local : NULL;
    }
    if (node->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
      node = index ? index->array : NULL;
      continue;
    }
    if (node->type == AST_MEMBER_ACCESS) {
      MemberAccess *member = (MemberAccess *)node->data;
      node = member ? member->object : NULL;
      continue;
    }
    return NULL;
  }
  return NULL;
}

static MemLocal *mem_addr_of_stack(MemCtx *ctx, ASTNode *expr) {
  return mem_addr_of_stack_at(ctx, expr, NULL);
}

/* The heap-pointer local at the root of `&buf[i]` (an interior pointer into
 * the block `buf` points to), or NULL when the expression is not the address
 * of an element of a pointer local. The stack analogue lives in
 * `mem_addr_of_stack_at`; the discriminator is `is_pointer` vs `is_stack`, so
 * the two never both match the same `&...`. */
static MemLocal *mem_addr_of_heap_at(MemCtx *ctx, ASTNode *expr,
                                     long long *offset_out) {
  if (offset_out) {
    *offset_out = -1;
  }
  expr = mem_unwrap_cast(expr);
  if (!expr || expr->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  UnaryExpression *unary = (UnaryExpression *)expr->data;
  if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
    return NULL;
  }
  ASTNode *direct = mem_unwrap_cast(unary->operand);
  if (!direct || direct->type != AST_INDEX_EXPRESSION) {
    return NULL;
  }
  ArrayIndexExpression *index = (ArrayIndexExpression *)direct->data;
  ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
  if (!array || array->type != AST_IDENTIFIER) {
    return NULL;
  }
  Identifier *id = (Identifier *)array->data;
  MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
  if (!local || !local->is_pointer) {
    return NULL;
  }
  long long constant = 0;
  if (offset_out &&
      type_checker_eval_integer_constant_with_checker(ctx->checker,
                                                       index->index, &constant) &&
      constant >= 0) {
    *offset_out = constant;
  }
  return local;
}

/* Fresh-allocation classification. Direct allocators always count; in the
 * summary/interproc modes a call to a function whose every return is fresh
 * counts too (the wrapper-allocator case). `via_out` names the wrapper. */
static int mem_is_allocation(MemCtx *ctx, ASTNode *expr, const char **via_out) {
  *via_out = NULL;
  expr = mem_unwrap_cast(expr);
  if (!expr) {
    return 0;
  }
  if (expr->type == AST_NEW_EXPRESSION) {
    return 1;
  }
  if (expr->type != AST_FUNCTION_CALL) {
    return 0;
  }
  CallExpression *call = (CallExpression *)expr->data;
  if (!call || !call->function_name || call->object) {
    return 0;
  }
  if (strcmp(call->function_name, "malloc") == 0 ||
      strcmp(call->function_name, "calloc") == 0 ||
      strcmp(call->function_name, "realloc") == 0) {
    return 1;
  }
  if (ctx->summaries) {
    MemFnSummary *summary = mem_summary_find(ctx->summaries,
                                             call->function_name);
    if (summary && summary->returns_fresh) {
      *via_out = summary->name;
      return 1;
    }
  }
  return 0;
}

static MemLocal *mem_expr_as_local(MemCtx *ctx, ASTNode *expr) {
  expr = mem_unwrap_cast(expr);
  if (!expr || expr->type != AST_IDENTIFIER) {
    return NULL;
  }
  Identifier *id = (Identifier *)expr->data;
  return id ? mem_find_local(ctx, id->name) : NULL;
}

static MemLocal *mem_addr_root_local(MemCtx *ctx, ASTNode *expr) {
  ASTNode *node = expr;
  int guard = 0;
  while (node && guard++ < 16) {
    node = mem_unwrap_cast(node);
    if (!node) {
      return NULL;
    }
    if (node->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)node->data;
      return id ? mem_find_local(ctx, id->name) : NULL;
    }
    if (node->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
      node = index ? index->array : NULL;
      continue;
    }
    if (node->type == AST_MEMBER_ACCESS) {
      MemberAccess *member = (MemberAccess *)node->data;
      node = member ? member->object : NULL;
      continue;
    }
    if (node->type == AST_UNARY_EXPRESSION) {
      UnaryExpression *unary = (UnaryExpression *)node->data;
      if (unary && unary->operator && strcmp(unary->operator, "*") == 0) {
        node = unary->operand;
        continue;
      }
      return NULL;
    }
    return NULL;
  }
  return NULL;
}

/* ---- the expression walk -------------------------------------------------------
 * One pass per expression: flags use-after-free on every read of a freed
 * pointer, bounds-checks constant indexes into stack arrays, classifies
 * free()/realloc() and summary-freeing calls, marks escapes, and checks
 * constant-size memory ops against their destination's capacity. */

static void mem_walk_expr(MemCtx *ctx, ASTNode *expr);

/* A dereference of a pointer the spine proved null: `var p: T* = 0;` with no
 * reassignment and no guard between. The runtime null trap would catch it;
 * the compiler can say it now. */
static void mem_check_null_deref(MemCtx *ctx, ASTNode *pointer_expr,
                                 SourceLocation loc) {
  if (ctx->mode != MEM_MODE_LOCAL || ctx->in_defer) {
    return;
  }
  MemLocal *local = mem_expr_as_local(ctx, pointer_expr);
  if (!local || !local->is_pointer) {
    return;
  }
  if (mtlc_target()->freestanding) {
    local->is_null = 0;
    local->is_wild = 0;
    return;
  }
  if (local->is_null) {
    char suggestion[160];
    snprintf(suggestion, sizeof(suggestion),
             "Assign a valid address first, or guard the access with "
             "`if (%s != 0)`",
             local->name);
    mem_warn_suggest(ctx, "M0113", loc, suggestion,
                     "`%s` is null here (assigned at line %zu and never "
                     "reassigned); this dereference will trap at runtime",
                     local->name, local->null_loc.line);
    local->is_null = 0; /* one report per null assignment */
    return;
  }
  if (local->is_wild) {
    mem_warn(ctx, "M0114", loc,
             "`%s` points at the constant address %lld (assigned at line "
             "%zu); the low 64K of the address space is never mapped, so "
             "this dereference will fault",
             local->name, local->wild_value, local->wild_loc.line);
    local->is_wild = 0;
  }
}

/* The borrow-lifetime checker: a pointer whose referent's lifetime ended on
 * the spine is reported the first time it is used. Like the direct-free and
 * null-deref diagnostics, these are phase-1 facts, so they only speak in
 * MEM_MODE_LOCAL and only once per borrow. */
static void mem_check_borrow_use(MemCtx *ctx, MemLocal *local,
                                 SourceLocation loc) {
  if (!local || ctx->in_defer || ctx->mode != MEM_MODE_LOCAL ||
      local->borrow_dangling == BORROW_OK) {
    return;
  }
  BorrowKill why = local->borrow_dangling;
  const char *via = local->borrow_killed_via ? local->borrow_killed_via : "it";
  size_t at = local->borrow_killed_loc.line;
  local->borrow_dangling = BORROW_OK; /* one report per borrow */
  if (why == BORROW_SCOPE) {
    char suggestion[200];
    snprintf(suggestion, sizeof(suggestion),
             "Copy the value out before the block ends, or widen `%s`'s scope "
             "so it outlives `%s`",
             via, local->name);
    mem_warn_suggest(ctx, "M0110", loc, suggestion,
                     "Use of `%s` after the scope of `%s` ended at line %zu; "
                     "`%s` borrows into `%s`, whose storage is reclaimed when "
                     "its block exits, so this pointer is dangling",
                     local->name, via, at, local->name, via);
  } else if (why == BORROW_REALLOC) {
    char suggestion[200];
    snprintf(suggestion, sizeof(suggestion),
             "Re-derive `%s` from `%s` after the `realloc`", local->name, via);
    mem_warn_suggest(ctx, "M0111", loc, suggestion,
                     "Use of `%s` after `%s` was reallocated at line %zu; "
                     "`realloc` may move the block, so this pointer is "
                     "dangling",
                     local->name, via, at);
  } else { /* BORROW_FREE */
    char suggestion[200];
    snprintf(suggestion, sizeof(suggestion),
             "Take the borrow after the last `free`, or copy the value out "
             "before freeing `%s`",
             via);
    mem_warn_suggest(ctx, "M0112", loc, suggestion,
                     "Use of `%s` after `%s` was freed at line %zu; `%s` "
                     "borrows into `%s`'s block, so this is use-after-free "
                     "through an interior pointer",
                     local->name, via, at, local->name, via);
  }
}

/* When `buf` is freed or realloc'd, every interior pointer borrowed from it
 * (`p = &buf[i]`) becomes dangling. Spine-only, definite by the C standard
 * (post-free/post-realloc interior pointers are indeterminate). */
static void mem_invalidate_heap_borrows(MemCtx *ctx, const char *buf_name,
                                        BorrowKill why, SourceLocation loc) {
  if (!buf_name || ctx->mode != MEM_MODE_LOCAL || ctx->depth != 0 ||
      ctx->in_defer) {
    return;
  }
  for (size_t i = 0; i < ctx->local_count; i++) {
    MemLocal *p = &ctx->locals[i];
    if (p->borrows_heap && strcmp(p->borrows_heap, buf_name) == 0) {
      p->borrow_dangling = why;
      p->borrow_killed_loc = loc;
      p->borrow_killed_via = buf_name;
    }
  }
}

/* `q = p` makes two names for one allocation. Put both pointers in one alias
 * group so that freeing or reallocating either invalidates the other. This is
 * the ownership/move discipline Rust enforces through the type system, applied
 * to raw pointers where Rust offers no checking at all. */
static void mem_alias_join(MemCtx *ctx, MemLocal *a, MemLocal *b) {
  if (!a || !b || a == b || !a->is_pointer || !b->is_pointer) {
    return;
  }
  int group = b->alias_group;
  if (group == 0) {
    group = ctx->next_alias_group++;
    b->alias_group = group;
  }
  if (a->alias_group == group) {
    return;
  }
  if (a->alias_group == 0) {
    a->alias_group = group;
    return;
  }
  int old = a->alias_group; /* both already grouped: merge into b's group */
  for (size_t i = 0; i < ctx->local_count; i++) {
    if (ctx->locals[i].alias_group == old) {
      ctx->locals[i].alias_group = group;
    }
  }
}

/* A free()/realloc() of `victim` invalidates every OTHER pointer that aliases
 * the same block. Definite by C semantics (a freed block is gone; a realloc'd
 * block may have moved, leaving the old pointer indeterminate), spine-only, so
 * it holds the zero-false-positive budget. Reported at the alias's next use. */
static void mem_alias_invalidate(MemCtx *ctx, MemLocal *victim, BorrowKill why,
                                 SourceLocation loc) {
  if (!victim || victim->alias_group == 0 || ctx->in_defer ||
      ctx->mode != MEM_MODE_LOCAL) {
    return;
  }
  for (size_t i = 0; i < ctx->local_count; i++) {
    MemLocal *m = &ctx->locals[i];
    if (m == victim || m->alias_group != victim->alias_group) {
      continue;
    }
    m->ever_freed = 1;
    if (why == BORROW_REALLOC) {
      if (m->borrow_dangling == BORROW_OK) {
        m->borrow_dangling = BORROW_REALLOC;
        m->borrow_killed_loc = loc;
        m->borrow_killed_via = victim->name;
      }
    } else if (m->freed != MEM_FREED_DEFINITE) {
      m->freed = ctx->depth == 0 ? MEM_FREED_DEFINITE : MEM_FREED_MAYBE;
      m->freed_loc = loc;
      m->freed_via = NULL;
      m->freed_alias = victim->name;
    }
  }
}

static void mem_check_use(MemCtx *ctx, MemLocal *local, SourceLocation loc) {
  mem_check_borrow_use(ctx, local, loc);
  if (!local || ctx->in_defer || local->freed != MEM_FREED_DEFINITE) {
    return;
  }
  /* Phase split: direct-free bugs are phase 1's; bugs where a CALL did the
   * free are phase 2's (phase 1 cannot see them). */
  if (ctx->mode == MEM_MODE_LOCAL && local->freed_via == NULL) {
    if (local->freed_alias) {
      char suggestion[200];
      snprintf(suggestion, sizeof(suggestion),
               "`%s` and `%s` name the same block; free it once, after the "
               "last use of either",
               local->name, local->freed_alias);
      mem_warn_suggest(ctx, "M0101", loc, suggestion,
                       "Use of `%s` after the block it shares with `%s` was "
                       "freed at line %zu; freeing one alias frees the block "
                       "both names point to, so this is use-after-free",
                       local->name, local->freed_alias, local->freed_loc.line);
    } else {
      mem_warn(ctx, "M0101", loc,
               "Use of `%s` after it was freed (freed at line %zu); this is "
               "use-after-free",
               local->name, local->freed_loc.line);
    }
    local->freed = MEM_FREED_MAYBE; /* one report per free site */
  } else if (ctx->mode == MEM_MODE_INTERPROC && local->freed_via != NULL) {
    mem_warn(ctx, "M0108", loc,
             "Use of `%s` after the call to `%s` at line %zu freed it; this "
             "is use-after-free",
             local->name, local->freed_via, local->freed_loc.line);
    local->freed = MEM_FREED_MAYBE;
  }
}

/* A free event for `local`: a direct free()/realloc(), or (phase 2) a call
 * whose summary says the parameter is unconditionally freed. */
static void mem_free_event(MemCtx *ctx, MemLocal *local, SourceLocation loc,
                           const char *via) {
  if (!local || !local->is_pointer) {
    return;
  }
  local->ever_freed = 1;
  /* Summary: a free of an un-reassigned parameter is part of what this
   * function does to its caller's pointer. A `defer free(p)` counts -- it
   * runs unconditionally at scope exit -- so the `in_defer` flag does not
   * suppress summary recording (it only suppresses intra-function flow
   * events below). A spine free/defer is definite; a free inside a branch
   * is a maybe. */
  if (ctx->mode == MEM_MODE_SUMMARY && local->param_index >= 0 &&
      local->param_index < MEM_MAX_PARAMS && !local->reassigned &&
      ctx->collect) {
    if (ctx->depth == 0) {
      ctx->collect->frees_definite |= 1u << local->param_index;
    } else {
      ctx->collect->frees_maybe |= 1u << local->param_index;
    }
  }
  if (ctx->in_defer) {
    return; /* defers run at scope exit; their free is not a flow event */
  }
  if (local->freed == MEM_FREED_DEFINITE) {
    /* Double free. Phase 1 owns the both-direct case; phase 2 owns every
     * case where a call performed at least one of the frees. */
    int involves_call = local->freed_via != NULL || via != NULL;
    if (ctx->mode == MEM_MODE_LOCAL && !involves_call) {
      if (local->freed_alias) {
        mem_warn(ctx, "M0102", loc,
                 "Double free of `%s`: it aliases `%s`, already freed at line "
                 "%zu",
                 local->name, local->freed_alias, local->freed_loc.line);
      } else {
        mem_warn(ctx, "M0102", loc, "Double free of `%s` (already freed at line %zu)",
                 local->name, local->freed_loc.line);
      }
    } else if (ctx->mode == MEM_MODE_INTERPROC && involves_call) {
      if (via && local->freed_via) {
        mem_warn(ctx, "M0109", loc,
                 "Double free of `%s`: the call to `%s` frees it, but the "
                 "call to `%s` at line %zu already did",
                 local->name, via, local->freed_via, local->freed_loc.line);
      } else if (via) {
        mem_warn(ctx, "M0109", loc,
                 "Double free of `%s`: the call to `%s` frees it, but it was "
                 "already freed at line %zu",
                 local->name, via, local->freed_loc.line);
      } else {
        mem_warn(ctx, "M0109", loc,
                 "Double free of `%s`: already freed by the call to `%s` at "
                 "line %zu",
                 local->name, local->freed_via, local->freed_loc.line);
      }
    }
    return;
  }
  local->freed = ctx->depth == 0 ? MEM_FREED_DEFINITE : MEM_FREED_MAYBE;
  local->freed_loc = loc;
  local->freed_via = via;
}

/* Constant-size memory ops: { name, dest arg, size arg }. */
static const struct {
  const char *name;
  int dest_index;
  int size_index;
} MEM_OPS[] = {
    {"memcpy", 0, 2},  {"memmove", 0, 2},  {"memset", 0, 2},
    {"mem_copy", 0, 2}, {"mem_move", 0, 2}, {"mem_zero", 0, 1},
    {"mem_fill", 0, 2},
};

/* Capacity in bytes of the destination when it is frame memory the analysis
 * understands: `&arr[k]` with constant k, `&arr`, or a pointer local that
 * was assigned `&arr...` (offset unknown: full capacity, which still
 * catches sizes larger than the whole array). Returns 0 when unknown. */
static long long mem_dest_capacity(MemCtx *ctx, ASTNode *dest,
                                   const char **array_name_out) {
  dest = mem_unwrap_cast(dest);
  if (!dest) {
    return 0;
  }
  long long count = 0, elem_size = 0;
  if (dest->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)dest->data;
    if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
      return 0;
    }
    ASTNode *target = mem_unwrap_cast(unary->operand);
    if (target && target->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)target->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      if (local && local->is_stack &&
          mem_array_extent(ctx, local->type_name, &count, &elem_size)) {
        *array_name_out = local->name;
        return count * elem_size;
      }
      return 0;
    }
    if (target && target->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)target->data;
      ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
      if (!array || array->type != AST_IDENTIFIER) {
        return 0;
      }
      Identifier *id = (Identifier *)array->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      long long offset = 0;
      if (local && local->is_stack &&
          mem_array_extent(ctx, local->type_name, &count, &elem_size) &&
          type_checker_eval_integer_constant_with_checker(
              ctx->checker, index->index, &offset) &&
          offset >= 0 && offset <= count) {
        *array_name_out = local->name;
        return (count - offset) * elem_size;
      }
      return 0;
    }
    return 0;
  }
  if (dest->type == AST_IDENTIFIER) {
    MemLocal *local = mem_expr_as_local(ctx, dest);
    if (local && local->points_to_stack) {
      MemLocal *target = mem_referent(ctx, local);
      if (target &&
          mem_array_extent(ctx, target->type_name, &count, &elem_size)) {
        *array_name_out = target->name;
        return count * elem_size;
      }
    }
  }
  return 0;
}

static void mem_check_mem_op(MemCtx *ctx, CallExpression *call,
                             SourceLocation loc) {
  if (ctx->mode != MEM_MODE_LOCAL) {
    return; /* phase 1 owns this diagnostic (function scope is live there) */
  }
  for (size_t i = 0; i < sizeof(MEM_OPS) / sizeof(MEM_OPS[0]); i++) {
    if (strcmp(call->function_name, MEM_OPS[i].name) != 0) {
      continue;
    }
    if ((size_t)MEM_OPS[i].dest_index >= call->argument_count ||
        (size_t)MEM_OPS[i].size_index >= call->argument_count) {
      return;
    }
    long long size = 0;
    if (!type_checker_eval_integer_constant_with_checker(
            ctx->checker, call->arguments[MEM_OPS[i].size_index], &size) ||
        size <= 0) {
      return;
    }
    const char *array_name = NULL;
    long long capacity = mem_dest_capacity(
        ctx, call->arguments[MEM_OPS[i].dest_index], &array_name);
    if (capacity > 0 && size > capacity) {
      mem_error(ctx, "M0106", loc,
                "Shrink the copy, or grow the destination array",
                "`%s` writes %lld bytes into `%s`, which only has %lld bytes "
                "left at this offset; this corrupts the stack frame",
                call->function_name, size, array_name, capacity);
    }
    return;
  }
}

static void mem_check_const_index(MemCtx *ctx, ASTNode *expr) {
  if (ctx->mode != MEM_MODE_LOCAL) {
    return; /* needs the live function scope for const locals */
  }
  ArrayIndexExpression *index = (ArrayIndexExpression *)expr->data;
  ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
  if (!array || array->type != AST_IDENTIFIER) {
    return;
  }
  Identifier *id = (Identifier *)array->data;
  MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
  long long count = 0, elem_size = 0, value = 0;
  if (!local ||
      !type_checker_eval_integer_constant_with_checker(ctx->checker,
                                                       index->index, &value)) {
    return;
  }
  if (local->is_stack &&
      mem_array_extent(ctx, local->type_name, &count, &elem_size)) {
    if (value < 0 || value >= count) {
      char suggestion[128];
      snprintf(suggestion, sizeof(suggestion),
               "Valid indexes for `%s` are 0..%lld", local->name, count - 1);
      mem_error(ctx, "M0105", expr->location, suggestion,
                "Index %lld is out of bounds for `%s` (%s)", value,
                local->name, local->type_name);
    }
    return;
  }

  /* Through a pointer alias with a known target and offset:
   * `var p = &a[2]; p[6]` lands at a[8]. Requires the pointee and element
   * types to match (no reinterpreting casts). */
  if (local->is_pointer && local->points_to_stack &&
      local->points_to_offset >= 0) {
    MemLocal *target = mem_referent(ctx, local);
    if (!target ||
        !mem_array_extent(ctx, target->type_name, &count, &elem_size)) {
      return;
    }
    const char *bracket = strchr(target->type_name, '[');
    size_t elem_len = bracket ? (size_t)(bracket - target->type_name) : 0;
    size_t ptr_len = strlen(local->type_name);
    if (elem_len == 0 || ptr_len == 0 ||
        local->type_name[ptr_len - 1] != '*' || ptr_len - 1 != elem_len ||
        strncmp(local->type_name, target->type_name, elem_len) != 0) {
      return;
    }
    long long effective = local->points_to_offset + value;
    if (effective < 0 || effective >= count) {
      char suggestion[160];
      snprintf(suggestion, sizeof(suggestion),
               "`%s` starts at `%s[%lld]`, so valid indexes through it are "
               "0..%lld",
               local->name, target->name, local->points_to_offset,
               count - 1 - local->points_to_offset);
      mem_error(ctx, "M0105", expr->location, suggestion,
                "Index %lld through `%s` lands at `%s[%lld]`, out of bounds "
                "for %s",
                value, local->name, target->name, effective,
                target->type_name);
    }
  }
}

static void mem_collect_param_store(MemCtx *ctx, MemLocal *local) {
  if (ctx->mode == MEM_MODE_SUMMARY && local && local->param_index >= 0 &&
      local->param_index < MEM_MAX_PARAMS && !local->reassigned &&
      ctx->collect) {
    ctx->collect->stores |= 1u << local->param_index;
  }
}

static void mem_collect_param_maybe_free(MemCtx *ctx, MemLocal *local) {
  if (ctx->mode == MEM_MODE_SUMMARY && local && local->param_index >= 0 &&
      local->param_index < MEM_MAX_PARAMS && !local->reassigned &&
      ctx->collect) {
    ctx->collect->frees_maybe |= 1u << local->param_index;
  }
}

/* The ownership effect of passing `local` as argument `arg_index` of a call
 * to `callee`. With no summary (externs, indirect calls, methods, phase 1)
 * the pointer conservatively escapes. */
static void mem_apply_call_arg(MemCtx *ctx, MemLocal *local,
                               const char *callee, size_t arg_index,
                               SourceLocation loc) {
  if (!local || !local->is_pointer) {
    return;
  }
  MemFnSummary *summary =
      ctx->summaries && callee ? mem_summary_find(ctx->summaries, callee)
                               : NULL;
  if (!summary || arg_index >= MEM_MAX_PARAMS) {
    local->escaped = 1;
    mem_collect_param_store(ctx, local);
    return;
  }
  unsigned bit = 1u << arg_index;
  if (summary->frees_definite & bit) {
    mem_free_event(ctx, local, loc, summary->name);
    return;
  }
  if (summary->frees_maybe & bit) {
    /* might free: silence both the leak and any later use */
    local->ever_freed = 1;
    local->escaped = 1;
    mem_collect_param_maybe_free(ctx, local);
    return;
  }
  if (summary->stores & bit) {
    local->escaped = 1;
    mem_collect_param_store(ctx, local);
    return;
  }
  /* pure borrow: the callee looked at it and gave it back; keep tracking */
}

static const char *mem_named_function(MemCtx *ctx, ASTNode *expr) {
  ASTNode *arg = mem_unwrap_cast(expr);
  UnaryExpression *unary = NULL;
  ASTNode *operand = NULL;
  Identifier *id = NULL;
  MemFnSummary *body = NULL;
  if (!arg || arg->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  unary = (UnaryExpression *)arg->data;
  if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
    return NULL;
  }
  operand = mem_unwrap_cast(unary->operand);
  if (!operand || operand->type != AST_IDENTIFIER) {
    return NULL;
  }
  id = (Identifier *)operand->data;
  if (!id || !id->name || mem_find_local(ctx, id->name)) {
    return NULL;
  }
  body = mem_summary_find(ctx->summaries, id->name);
  return body && body->fn ? id->name : NULL;
}

static const char *mem_carried_function(ASTNode *expr) {
  ASTNode *node = mem_unwrap_cast(expr);
  ASTNode *root = node;
  int guard = 0;
  if (!node || !node->resolved_type ||
      node->resolved_type->kind != TYPE_FUNCTION_POINTER) {
    return NULL;
  }
  while (root && guard++ < 16) {
    root = mem_unwrap_cast(root);
    if (!root) {
      return NULL;
    }
    if (root->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)root->data;
      if (!id || !id->name) {
        return NULL;
      }
      return id->name;
    }
    if (root->type == AST_MEMBER_ACCESS) {
      MemberAccess *member = (MemberAccess *)root->data;
      root = member ? member->object : NULL;
      continue;
    }
    if (root->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)root->data;
      root = index ? index->array : NULL;
      continue;
    }
    return NULL;
  }
  return NULL;
}

static const char *mem_task_entry(MemCtx *ctx, CallExpression *call,
                                  size_t *arg_out) {
  size_t i;
  MemFnSummary *callee = NULL;
  if (!ctx->summaries || !call || call->object || call->is_indirect_call ||
      !call->function_name) {
    return NULL;
  }
  callee = mem_summary_find(ctx->summaries, call->function_name);
  if (callee && callee->fn) {
    return NULL;
  }
  for (i = 0; i + 1 < call->argument_count; i++) {
    const char *named = mem_named_function(ctx, call->arguments[i]);
    const char *carried = NULL;
    if (named) {
      *arg_out = i + 1;
      return named;
    }
    carried = mem_carried_function(call->arguments[i]);
    if (carried) {
      *arg_out = i + 1;
      return carried;
    }
  }
  return NULL;
}

static void mem_check_task_capture(MemCtx *ctx, CallExpression *call,
                                   SourceLocation loc) {
  size_t at = 0;
  const char *entry = mem_task_entry(ctx, call, &at);
  ASTNode *arg = NULL;
  MemLocal *frame = NULL;
  MemLocal *held = NULL;
  if (!entry || at >= call->argument_count) {
    return;
  }
  arg = call->arguments[at];
  call->task_capture_argument = at;
  call->task_entry_name = entry;
  frame = mem_addr_of_stack(ctx, arg);
  held = mem_expr_as_local(ctx, arg);
  if (!frame && held && held->points_to_stack) {
    frame = held;
  }
  if (frame) {
    mem_error_late(
        ctx, "M0121", loc,
        "put what the task reads where it outlives the task: a global, an "
        "allocation the task or a later join owns, or a value passed by copy",
        "'%s' is handed to the task '%s', and it points into the frame of "
        "'%s', which the task can still be reading after that frame returns",
        frame->name, entry,
        ctx->fn && ctx->fn->name ? ctx->fn->name : "this function");
  }
  if (held && held->is_pointer) {
    held->handed_over = 1;
    held->handover_loc = loc;
    held->handover_via = entry;
  }
}

static void mem_check_handover_write(MemCtx *ctx, ASTNode *target,
                                     SourceLocation loc) {
  MemLocal *root = mem_addr_root_local(ctx, target);
  if (!root || !root->handed_over) {
    return;
  }
  mem_error_late(ctx, "M0122", loc,
                 "write it before the task starts, or hand the task its own "
                 "copy and keep this one",
                 "'%s' was handed to the task '%s' on line %d, and this "
                 "writes through it afterwards",
                 root->name, root->handover_via ? root->handover_via : "?",
                 (int)root->handover_loc.line);
  root->handed_over = 0;
}

/* Constant arithmetic that traps or surprises: division/modulo by a constant
 * zero (a guaranteed runtime trap) and shifts wider than the value (x86
 * masks the count, so `x << 32` on an int32 is `x`, which nobody means). */
static void mem_check_const_arithmetic(MemCtx *ctx, BinaryExpression *binary,
                                       SourceLocation loc) {
  if (ctx->mode != MEM_MODE_LOCAL || !binary->operator) {
    return;
  }
  long long value = 0;
  if (strcmp(binary->operator, "/") == 0 || strcmp(binary->operator, "%") == 0) {
    if (type_checker_eval_integer_constant_with_checker(ctx->checker,
                                                        binary->right, &value) &&
        value == 0) {
      mem_error(ctx, "M0116", loc, "Fix the constant, or guard the divisor",
                "%s by a constant zero; this traps the moment it executes",
                strcmp(binary->operator, "/") == 0 ? "Division" : "Modulo");
    }
    return;
  }
  if (strcmp(binary->operator, "<<") == 0 ||
      strcmp(binary->operator, ">>") == 0) {
    if (!type_checker_eval_integer_constant_with_checker(ctx->checker,
                                                         binary->right, &value)) {
      return;
    }
    Type *left_type = binary->left ? binary->left->resolved_type : NULL;
    if (!left_type || left_type->size == 0 || left_type->size > 8) {
      return;
    }
    switch (left_type->kind) {
    case TYPE_INT8: case TYPE_UINT8: case TYPE_INT16: case TYPE_UINT16:
    case TYPE_INT32: case TYPE_UINT32: case TYPE_INT64: case TYPE_UINT64:
      break;
    default:
      return;
    }
    long long width = (long long)left_type->size * 8;
    if (value < 0 || value >= width) {
      mem_warn(ctx, "M0115", loc,
               "Shift by %lld on a %lld-bit value (`%s`); the hardware masks "
               "the shift count, so this does not produce the zero the code "
               "reads as",
               value, width, left_type->name ? left_type->name : "?");
    }
  }
}

/* ---- loop-bound bounds analysis ---------------------------------------------
 * The classic off-by-one: `var a: T[8]; while (i <= 8) { a[i] = ...; }`.
 * Proven only when the loop shape is airtight:
 *   - the condition is `iv < bound` or `iv <= bound` with a constant bound
 *   - the induction variable has a known constant start >= 0
 *   - the body modifies it exactly once, as `iv = iv + 1`
 *   - nothing can leave early (no break/continue/return anywhere inside)
 * Under those rules the final iteration provably happens, so an `a[iv]`
 * whose maximum reaches the array length is a hard error. */

static int mem_ast_contains_exit(ASTNode *node) {
  if (!node) {
    return 0;
  }
  if (node->type == AST_BREAK_STATEMENT ||
      node->type == AST_CONTINUE_STATEMENT ||
      node->type == AST_RETURN_STATEMENT) {
    return 1;
  }
  /* statement-bearing payloads the generic child walk does not reach */
  switch (node->type) {
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)node->data;
    if (if_stmt) {
      if (mem_ast_contains_exit(if_stmt->then_branch) ||
          mem_ast_contains_exit(if_stmt->else_branch)) {
        return 1;
      }
      for (size_t i = 0; i < if_stmt->else_if_count; i++) {
        if (mem_ast_contains_exit(if_stmt->else_ifs[i].body)) {
          return 1;
        }
      }
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *w = (WhileStatement *)node->data;
    if (w && mem_ast_contains_exit(w->body)) {
      return 1;
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *f = (ForStatement *)node->data;
    if (f && mem_ast_contains_exit(f->body)) {
      return 1;
    }
    break;
  }
  case AST_SWITCH_STATEMENT:
  case AST_MATCH_STATEMENT:
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT:
    return 1; /* switch breaks are scoped, but stay conservative */
  default:
    break;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (mem_ast_contains_exit(node->children[i])) {
      return 1;
    }
  }
  return 0;
}

/* `expr` is exactly `iv + 1` (either operand order). */
static int mem_expr_is_increment_of(ASTNode *expr, const char *iv) {
  expr = mem_unwrap_cast(expr);
  if (!expr || expr->type != AST_BINARY_EXPRESSION) {
    return 0;
  }
  BinaryExpression *binary = (BinaryExpression *)expr->data;
  if (!binary || !binary->operator || strcmp(binary->operator, "+") != 0) {
    return 0;
  }
  ASTNode *left = mem_unwrap_cast(binary->left);
  ASTNode *right = mem_unwrap_cast(binary->right);
  ASTNode *ident = NULL, *literal = NULL;
  if (left && left->type == AST_IDENTIFIER) {
    ident = left;
    literal = right;
  } else if (right && right->type == AST_IDENTIFIER) {
    ident = right;
    literal = left;
  }
  if (!ident || !literal || literal->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  Identifier *id = (Identifier *)ident->data;
  NumberLiteral *num = (NumberLiteral *)literal->data;
  return id && num && !num->is_float && num->int_value == 1 &&
         strcmp(id->name, iv) == 0;
}

/* Count assignments to `iv` under `node`; sets *clean_increment when every
 * one is the `iv = iv + 1` shape. Declarations shadowing `iv` poison it. */
static void mem_count_iv_writes(ASTNode *node, const char *iv, int *count,
                                int *clean_increment) {
  if (!node) {
    return;
  }
  if (node->type == AST_ASSIGNMENT) {
    Assignment *assign = (Assignment *)node->data;
    if (assign && !assign->target && assign->variable_name &&
        strcmp(assign->variable_name, iv) == 0) {
      (*count)++;
      if (!mem_expr_is_increment_of(assign->value, iv)) {
        *clean_increment = 0;
      }
    }
  } else if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    if (decl && decl->name && strcmp(decl->name, iv) == 0) {
      *clean_increment = 0; /* shadowed: a different variable now */
    }
  } else if (node->type == AST_IF_STATEMENT) {
    IfStatement *if_stmt = (IfStatement *)node->data;
    if (if_stmt) {
      mem_count_iv_writes(if_stmt->then_branch, iv, count, clean_increment);
      mem_count_iv_writes(if_stmt->else_branch, iv, count, clean_increment);
      for (size_t i = 0; i < if_stmt->else_if_count; i++) {
        mem_count_iv_writes(if_stmt->else_ifs[i].body, iv, count,
                            clean_increment);
      }
    }
  } else if (node->type == AST_WHILE_STATEMENT) {
    WhileStatement *w = (WhileStatement *)node->data;
    if (w) {
      mem_count_iv_writes(w->body, iv, count, clean_increment);
    }
  } else if (node->type == AST_FOR_STATEMENT) {
    ForStatement *f = (ForStatement *)node->data;
    if (f) {
      mem_count_iv_writes(f->initializer, iv, count, clean_increment);
      mem_count_iv_writes(f->body, iv, count, clean_increment);
      mem_count_iv_writes(f->increment, iv, count, clean_increment);
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    mem_count_iv_writes(node->children[i], iv, count, clean_increment);
  }
}

/* Find `array[iv]` accesses under `node` and report the ones whose maximum
 * index provably reaches past the array. One report per loop. */
static void mem_find_oob_iv_indexes(MemCtx *ctx, ASTNode *node, const char *iv,
                                    long long max_iv, int *reported) {
  if (!node || *reported) {
    return;
  }
  if (node->type == AST_INDEX_EXPRESSION) {
    ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
    ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
    ASTNode *index_expr = index ? mem_unwrap_cast(index->index) : NULL;
    if (array && array->type == AST_IDENTIFIER && index_expr &&
        index_expr->type == AST_IDENTIFIER) {
      Identifier *array_id = (Identifier *)array->data;
      Identifier *index_id = (Identifier *)index_expr->data;
      if (array_id && index_id && strcmp(index_id->name, iv) == 0) {
        MemLocal *local = mem_find_local(ctx, array_id->name);
        long long count = 0, elem_size = 0;
        if (local && local->is_stack &&
            mem_array_extent(ctx, local->type_name, &count, &elem_size) &&
            max_iv >= count) {
          char suggestion[160];
          if (max_iv == count) {
            snprintf(suggestion, sizeof(suggestion),
                     "Use `%s < %lld` (the `<=` runs one iteration too many)",
                     iv, count);
          } else {
            snprintf(suggestion, sizeof(suggestion),
                     "Valid indexes for `%s` are 0..%lld", local->name,
                     count - 1);
          }
          mem_error(ctx, "M0117", node->location, suggestion,
                    "This loop runs `%s` up to %lld, but `%s` has %lld "
                    "element%s (valid indexes 0..%lld); the final iteration "
                    "reads or writes past the end",
                    iv, max_iv, local->name, count, count == 1 ? "" : "s",
                    count - 1);
          *reported = 1;
          return;
        }
      }
    }
  }
  switch (node->type) {
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)node->data;
    if (if_stmt) {
      mem_find_oob_iv_indexes(ctx, if_stmt->condition, iv, max_iv, reported);
      mem_find_oob_iv_indexes(ctx, if_stmt->then_branch, iv, max_iv, reported);
      mem_find_oob_iv_indexes(ctx, if_stmt->else_branch, iv, max_iv, reported);
      for (size_t i = 0; i < if_stmt->else_if_count && !*reported; i++) {
        mem_find_oob_iv_indexes(ctx, if_stmt->else_ifs[i].condition, iv,
                                max_iv, reported);
        mem_find_oob_iv_indexes(ctx, if_stmt->else_ifs[i].body, iv, max_iv,
                                reported);
      }
    }
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *assign = (Assignment *)node->data;
    if (assign) {
      mem_find_oob_iv_indexes(ctx, assign->target, iv, max_iv, reported);
      mem_find_oob_iv_indexes(ctx, assign->value, iv, max_iv, reported);
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret = (ReturnStatement *)node->data;
    if (ret) {
      mem_find_oob_iv_indexes(ctx, ret->value, iv, max_iv, reported);
    }
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
    if (index) {
      mem_find_oob_iv_indexes(ctx, index->array, iv, max_iv, reported);
      mem_find_oob_iv_indexes(ctx, index->index, iv, max_iv, reported);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)node->data;
    if (binary) {
      mem_find_oob_iv_indexes(ctx, binary->left, iv, max_iv, reported);
      mem_find_oob_iv_indexes(ctx, binary->right, iv, max_iv, reported);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)node->data;
    if (unary) {
      mem_find_oob_iv_indexes(ctx, unary->operand, iv, max_iv, reported);
    }
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)node->data;
    if (cast) {
      mem_find_oob_iv_indexes(ctx, cast->operand, iv, max_iv, reported);
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)node->data;
    if (call) {
      for (size_t i = 0; i < call->argument_count && !*reported; i++) {
        mem_find_oob_iv_indexes(ctx, call->arguments[i], iv, max_iv, reported);
      }
    }
    break;
  }
  default:
    break;
  }
  for (size_t i = 0; i < node->child_count && !*reported; i++) {
    mem_find_oob_iv_indexes(ctx, node->children[i], iv, max_iv, reported);
  }
}

/* Analyze one counted loop for a provable final-iteration overrun.
 * `increment` is the for-statement increment, or NULL for while loops
 * (whose increment must then live in the body). */
static void mem_check_loop_bounds(MemCtx *ctx, ASTNode *condition,
                                  ASTNode *body, ASTNode *increment) {
  if (ctx->mode != MEM_MODE_LOCAL || !condition || !body) {
    return;
  }
  if (condition->type != AST_BINARY_EXPRESSION) {
    return;
  }
  BinaryExpression *cmp = (BinaryExpression *)condition->data;
  if (!cmp || !cmp->operator ||
      (strcmp(cmp->operator, "<") != 0 && strcmp(cmp->operator, "<=") != 0)) {
    return;
  }
  ASTNode *left = mem_unwrap_cast(cmp->left);
  if (!left || left->type != AST_IDENTIFIER) {
    return;
  }
  Identifier *iv_id = (Identifier *)left->data;
  if (!iv_id || !iv_id->name) {
    return;
  }
  const char *iv = iv_id->name;

  long long bound = 0;
  if (!type_checker_eval_integer_constant_with_checker(ctx->checker,
                                                       cmp->right, &bound)) {
    return;
  }
  long long max_iv = strcmp(cmp->operator, "<=") == 0 ? bound : bound - 1;

  /* The induction variable needs a known non-negative start. (Scalar locals
   * classify as is_stack -- frame memory -- so only pointers are excluded.) */
  MemLocal *iv_local = mem_find_local(ctx, iv);
  if (!iv_local || iv_local->is_pointer || !iv_local->has_const_value ||
      iv_local->const_value < 0 || iv_local->const_value > max_iv) {
    return;
  }

  /* Nothing may leave the loop early, and the only write to the induction
   * variable must be a single `iv = iv + 1`. */
  if (mem_ast_contains_exit(body)) {
    return;
  }
  int writes = 0;
  int clean = 1;
  mem_count_iv_writes(body, iv, &writes, &clean);
  if (increment) {
    mem_count_iv_writes(increment, iv, &writes, &clean);
  }
  if (writes != 1 || !clean) {
    return;
  }

  int reported = 0;
  mem_find_oob_iv_indexes(ctx, body, iv, max_iv, &reported);
}

static void mem_walk_expr(MemCtx *ctx, ASTNode *expr) {
  if (!expr) {
    return;
  }
  switch (expr->type) {
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expr->data;
    MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
    mem_check_use(ctx, local, expr->location);
    if (local && local->is_pointer && ctx->in_condition) {
      /* mentioned in a condition: the code is (presumably) checking it, so
       * definite-null/wild knowledge ends here */
      local->is_null = 0;
      local->is_wild = 0;
    }
    return;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expr->data;
    mem_walk_expr(ctx, cast ? cast->operand : NULL);
    return;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expr->data;
    if (!unary) {
      return;
    }
    if (unary->operator && strcmp(unary->operator, "&") == 0) {
      /* &p makes the pointer reachable elsewhere: it may be freed, kept, or
       * written through the alias, so all definite knowledge ends here. */
      MemLocal *local = mem_expr_as_local(ctx, unary->operand);
      if (local) {
        local->escaped = 1;
        local->ever_freed = 1; /* could be freed through the alias */
        local->is_null = 0;
        local->is_wild = 0;
        return;
      }
      MemLocal *root = mem_addr_root_local(ctx, unary->operand);
      if (root && root->is_pointer) {
        root->escaped = 1;
      }
      return;
    }
    if (unary->operator && strcmp(unary->operator, "*") == 0) {
      mem_check_null_deref(ctx, unary->operand, expr->location);
    }
    mem_walk_expr(ctx, unary->operand);
    return;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expr->data;
    if (member) {
      mem_check_null_deref(ctx, member->object, expr->location);
      mem_walk_expr(ctx, member->object);
    }
    return;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index = (ArrayIndexExpression *)expr->data;
    mem_check_const_index(ctx, expr);
    if (index) {
      MemLocal *base = mem_expr_as_local(ctx, index->array);
      if (base && base->is_pointer) {
        mem_check_null_deref(ctx, index->array, expr->location);
      }
    }
    mem_walk_expr(ctx, index ? index->array : NULL);
    mem_walk_expr(ctx, index ? index->index : NULL);
    return;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expr->data;
    if (binary) {
      mem_check_const_arithmetic(ctx, binary, expr->location);
      mem_walk_expr(ctx, binary->left);
      mem_walk_expr(ctx, binary->right);
    }
    return;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expr->data;
    if (!call) {
      return;
    }
    if (call->object) {
      mem_walk_expr(ctx, call->object);
    }
    if (call->function_name && !call->object &&
        (strcmp(call->function_name, "free") == 0 ||
         strcmp(call->function_name, "realloc") == 0) &&
        call->argument_count >= 1) {
      /* The pointer argument is CONSUMED, not used; both invalidate it. */
      int is_realloc = strcmp(call->function_name, "realloc") == 0;
      BorrowKill why = is_realloc ? BORROW_REALLOC : BORROW_FREE;
      MemLocal *consumed = mem_expr_as_local(ctx, call->arguments[0]);
      mem_free_event(ctx, consumed, expr->location, NULL);
      if (consumed && is_realloc && consumed->freed == MEM_FREED_DEFINITE) {
        consumed->freed = MEM_FREED_MAYBE;
      }
      if (consumed) {
        /* interior pointers borrowed from it (`&buf[i]`) become dangling, and
         * so does every whole-pointer alias of it (`q = buf`) */
        mem_invalidate_heap_borrows(ctx, consumed->name, why, expr->location);
        mem_alias_invalidate(ctx, consumed, why, expr->location);
      }
      for (size_t i = 1; i < call->argument_count; i++) {
        mem_walk_expr(ctx, call->arguments[i]);
      }
      return;
    }
    if (call->function_name && !call->object) {
      mem_check_mem_op(ctx, call, expr->location);
    }
    /* Every argument is evaluated BEFORE the call runs, so check all the uses
     * first and only then apply the call's ownership effects (free / store).
     * Interleaving would make a pointer passed twice -- `f(p, p)` where the
     * callee frees the first parameter -- look like a use-after-free on the
     * second read, which actually happens before the callee runs. */
    for (size_t i = 0; i < call->argument_count; i++) {
      mem_walk_expr(ctx, call->arguments[i]);
    }
    for (size_t i = 0; i < call->argument_count; i++) {
      mem_apply_call_arg(ctx, mem_expr_as_local(ctx, call->arguments[i]),
                         (!call->object && !call->is_indirect_call)
                             ? call->function_name
                             : NULL,
                         i, expr->location);
    }
    mem_check_task_capture(ctx, call, expr->location);
    return;
  }
  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *call = (FuncPtrCall *)expr->data;
    if (!call) {
      return;
    }
    mem_walk_expr(ctx, call->function);
    for (size_t i = 0; i < call->argument_count; i++) {
      mem_walk_expr(ctx, call->arguments[i]);
      MemLocal *local = mem_expr_as_local(ctx, call->arguments[i]);
      if (local && local->is_pointer) {
        local->escaped = 1;
      }
    }
    return;
  }
  default:
    for (size_t i = 0; i < expr->child_count; i++) {
      mem_walk_expr(ctx, expr->children[i]);
    }
    return;
  }
}

/* ---- assignments and declarations -------------------------------------------- */

/* Apply `name = value` to the tracked state. */
static void mem_apply_assignment(MemCtx *ctx, const char *name, ASTNode *value,
                                 SourceLocation loc) {
  MemLocal *local = mem_find_local(ctx, name);
  if (local) {
    if (local->param_index >= 0) {
      local->reassigned = 1;
    }
    if (local->is_pointer) {
      local->freed = MEM_FREED_NO;
      local->handed_over = 0;
      local->handover_via = NULL;
      local->freed_via = NULL;
      local->freed_alias = NULL;
      local->holds_alloc = 0;
      local->points_to_stack = NULL;
      local->points_to_slot = 0;
      local->points_to_offset = -1;
      local->borrows_heap = NULL;
      local->borrow_dangling = BORROW_OK; /* re-derivation clears a stale borrow */
      local->alias_group = 0; /* re-pointing leaves any alias group */
      local->is_null = 0;
      local->is_wild = 0;
      if (!ctx->in_defer && ctx->depth == 0 &&
          type_checker_is_null_pointer_constant(value)) {
        local->is_null = 1;
        local->null_loc = loc;
      } else if (!ctx->in_defer && ctx->depth == 0) {
        /* a small constant cast to a pointer can never be valid memory
         * (the low 64K is never mapped on Windows or Linux) */
        long long constant = 0;
        if (type_checker_eval_integer_constant_with_checker(
                ctx->checker, mem_unwrap_cast(value), &constant) &&
            constant > 0 && constant < 65536) {
          local->is_wild = 1;
          local->wild_value = constant;
          local->wild_loc = loc;
        }
      }
      const char *via = NULL;
      if (!ctx->in_defer && ctx->depth == 0 &&
          mem_is_allocation(ctx, value, &via)) {
        local->holds_alloc = 1;
        local->alloc_loc = loc;
        local->alloc_via = via;
        local->escaped = 0;
        local->ever_freed = 0;
      }
      long long stack_offset = -1;
      MemLocal *stack_target = mem_addr_of_stack_at(ctx, value, &stack_offset);
      if (stack_target && ctx->depth == 0 && !ctx->in_defer) {
        /* spine only: a branch assignment may not have happened, so alias
         * knowledge from it would make false bounds claims */
        local->points_to_stack = stack_target->name;
        local->points_to_slot = (size_t)(stack_target - ctx->locals) + 1;
        local->points_to_offset = stack_offset;
      }
      MemLocal *heap_target = mem_addr_of_heap_at(ctx, value, NULL);
      if (heap_target && ctx->depth == 0 && !ctx->in_defer) {
        /* an interior pointer into a heap buffer: a later free/realloc of
         * that buffer leaves this borrow dangling */
        local->borrows_heap = heap_target->name;
      }
      MemLocal *source = mem_expr_as_local(ctx, value);
      if (source && source->is_pointer) {
        /* aliasing: the allocation now has two names. The leak analysis can no
         * longer pin a single owner (escaped), but the ownership analysis
         * joins them into an alias group so a free/realloc of either is seen
         * to invalidate both. */
        source->escaped = 1;
        if (ctx->depth == 0 && !ctx->in_defer) {
          local->points_to_stack = source->points_to_stack;
          local->points_to_slot = source->points_to_slot;
          local->points_to_offset = source->points_to_offset;
          mem_alias_join(ctx, local, source);
        }
      }
    } else {
      /* integer constant tracking (the loop-bound analysis needs the
       * induction variable's start): a spine assignment of a constant is
       * known, anything else (or a branch assignment) ends the knowledge */
      local->has_const_value = 0;
      long long value_const = 0;
      if (!ctx->in_defer && ctx->depth == 0 &&
          type_checker_eval_integer_constant_with_checker(ctx->checker, value,
                                                          &value_const)) {
        local->has_const_value = 1;
        local->const_value = value_const;
      }
    }
    return;
  }

  /* Not a local or parameter: a global. A stack address stored there
   * outlives the frame it points into. */
  MemLocal *stack_target = mem_addr_of_stack(ctx, value);
  if (stack_target && ctx->mode == MEM_MODE_LOCAL) {
    mem_warn(ctx, "M0104", loc,
             "Global `%s` is assigned the address of stack local `%s`; that "
             "address is dangling as soon as this function returns",
             name, stack_target->name);
  }
  MemLocal *source = mem_expr_as_local(ctx, value);
  if (source && source->is_pointer) {
    source->escaped = 1;
    if (ctx->mode == MEM_MODE_SUMMARY && source->param_index >= 0 &&
        source->param_index < MEM_MAX_PARAMS && ctx->collect) {
      ctx->collect->stores |= 1u << source->param_index;
    }
  }
}

/* ---- the statement walk --------------------------------------------------------- */

static void mem_walk_statement(MemCtx *ctx, ASTNode *statement);

static void mem_walk_block(MemCtx *ctx, ASTNode *block) {
  if (!block) {
    return;
  }
  if (block->type == AST_PROGRAM) {
    for (size_t i = 0; i < block->child_count; i++) {
      mem_walk_statement(ctx, block->children[i]);
    }
    return;
  }
  mem_walk_statement(ctx, block);
}

/* Run when the lexical block at `level` exits: a borrow whose pointer outlives
 * the block (`scope_level < level`) but whose stack referent was declared in it
 * (`scope_level >= level`) is now dangling. Reported at the borrow's next use. */
static void mem_scope_exit_check(MemCtx *ctx, int level) {
  for (size_t i = 0; i < ctx->local_count; i++) {
    MemLocal *p = &ctx->locals[i];
    if (!p->points_to_stack || p->scope_level >= level) {
      continue;
    }
    MemLocal *referent = mem_referent(ctx, p);
    if (referent && referent->is_stack && referent->scope_level >= level) {
      p->borrow_dangling = BORROW_SCOPE;
      p->borrow_killed_loc = referent->decl_loc;
      p->borrow_killed_via = referent->name;
    }
  }
  for (size_t i = 0; i < ctx->local_count; i++) {
    if (ctx->locals[i].scope_level >= level) {
      ctx->locals[i].out_of_scope = 1;
    }
  }
}

/* ---- paths ------------------------------------------------------------------
 *
 * A fact used to be definite only on the function's straight-line spine, so
 * everything a branch or a loop body did was demoted to "maybe" and went
 * unreported. That left the code most programs are actually made of invisible.
 *
 * A path is walked with its facts definite, because within one execution of
 * that block the statements do run in order: `if (c) { free(p); p[0] = 1; }`
 * is a use-after-free whenever the branch is taken, and saying so proves
 * something about every execution of it. What the paths disagree about stays
 * conservative: the state after an `if` is the state both arms agreed on, and
 * a loop merges its body against not running at all.
 *
 * Only the facts that drive a report are merged. The rest of a local -- its
 * name, type, declaration site, scope -- is the same on every path. */

typedef struct {
  MemLocal *locals;
  size_t count;
} MemPath;

static int mem_path_save(const MemCtx *ctx, MemPath *out) {
  out->count = ctx->local_count;
  out->locals = NULL;
  if (out->count == 0) {
    return 1;
  }
  out->locals = malloc(out->count * sizeof(MemLocal));
  if (!out->locals) {
    out->count = 0;
    return 0;
  }
  memcpy(out->locals, ctx->locals, out->count * sizeof(MemLocal));
  return 1;
}

static void mem_path_free(MemPath *path) {
  free(path->locals);
  path->locals = NULL;
  path->count = 0;
}

static void mem_path_restore(MemCtx *ctx, const MemPath *path) {
  if (path->count > 0 && path->locals) {
    memcpy(ctx->locals, path->locals, path->count * sizeof(MemLocal));
  }
}

/* What both paths agree on, written over `into`. */
static void mem_local_join(MemLocal *into, const MemLocal *a,
                           const MemLocal *b) {
  /* An "ever" fact is a fact about the whole function, so it survives either
     path having established it. */
  into->ever_freed = a->ever_freed || b->ever_freed;
  into->escaped = a->escaped || b->escaped;
  into->reassigned = a->reassigned || b->reassigned;
  into->out_of_scope = a->out_of_scope || b->out_of_scope;
  into->handed_over = a->handed_over || b->handed_over;
  if (into->handed_over) {
    const MemLocal *from = a->handed_over ? a : b;
    into->handover_loc = from->handover_loc;
    into->handover_via = from->handover_via;
  }

  if (a->freed == MEM_FREED_DEFINITE && b->freed == MEM_FREED_DEFINITE) {
    into->freed = MEM_FREED_DEFINITE;
    into->freed_loc = a->freed_loc;
    into->freed_via = a->freed_via;
    into->freed_alias = a->freed_alias;
  } else if (a->freed != MEM_FREED_NO || b->freed != MEM_FREED_NO) {
    const MemLocal *from = a->freed != MEM_FREED_NO ? a : b;
    into->freed = MEM_FREED_MAYBE;
    into->freed_loc = from->freed_loc;
    into->freed_via = from->freed_via;
    into->freed_alias = from->freed_alias;
  } else {
    into->freed = MEM_FREED_NO;
    into->freed_via = NULL;
    into->freed_alias = NULL;
  }

  if (a->holds_alloc && b->holds_alloc) {
    into->holds_alloc = 1;
    into->alloc_loc = a->alloc_loc;
    into->alloc_via = a->alloc_via;
  } else {
    into->holds_alloc = 0;
    into->alloc_via = NULL;
  }

  if (a->is_null && b->is_null) {
    into->is_null = 1;
    into->null_loc = a->null_loc;
  } else {
    into->is_null = 0;
  }

  if (a->is_wild && b->is_wild && a->wild_value == b->wild_value) {
    into->is_wild = 1;
    into->wild_value = a->wild_value;
    into->wild_loc = a->wild_loc;
  } else {
    into->is_wild = 0;
  }

  if (a->borrow_dangling != BORROW_OK && b->borrow_dangling != BORROW_OK) {
    into->borrow_dangling = a->borrow_dangling;
    into->borrow_killed_loc = a->borrow_killed_loc;
    into->borrow_killed_via = a->borrow_killed_via;
  } else {
    into->borrow_dangling = BORROW_OK;
    into->borrow_killed_via = NULL;
  }

  if (a->has_const_value && b->has_const_value &&
      a->const_value == b->const_value) {
    into->has_const_value = 1;
    into->const_value = a->const_value;
  } else {
    into->has_const_value = 0;
  }

  if (a->points_to_stack && b->points_to_stack &&
      a->points_to_slot == b->points_to_slot &&
      a->points_to_offset == b->points_to_offset) {
    into->points_to_stack = a->points_to_stack;
    into->points_to_slot = a->points_to_slot;
    into->points_to_offset = a->points_to_offset;
  } else {
    into->points_to_stack = NULL;
    into->points_to_slot = 0;
    into->points_to_offset = -1;
  }

  if (a->borrows_heap && b->borrows_heap &&
      strcmp(a->borrows_heap, b->borrows_heap) == 0) {
    into->borrows_heap = a->borrows_heap;
  } else {
    into->borrows_heap = NULL;
  }

  into->alias_group = a->alias_group == b->alias_group ? a->alias_group : 0;
}

static void mem_path_join(MemCtx *ctx, const MemPath *a, const MemPath *b) {
  size_t i;
  size_t shared = a->count < b->count ? a->count : b->count;
  for (i = 0; i < shared && i < ctx->local_count; i++) {
    mem_local_join(&ctx->locals[i], &a->locals[i], &b->locals[i]);
  }
}

/* One arm of a branch, or one iteration of a loop body: walked with its own
 * facts definite, then handed back for the join. */
static int mem_walk_path(MemCtx *ctx, ASTNode *body, const MemPath *entry,
                         MemPath *out) {
  int level;
  mem_path_restore(ctx, entry);
  level = ++ctx->scope_level;
  mem_walk_block(ctx, body);
  mem_scope_exit_check(ctx, level);
  ctx->scope_level--;
  return mem_path_save(ctx, out);
}

/* Walk each arm from the same entry state and keep what they all agree on.
 * `arms` may hold NULL entries, which are arms the program did not write; the
 * path where none of them ran is the entry state itself, and it is included
 * unless `exhaustive` says one arm always runs. */
static void mem_walk_arms(MemCtx *ctx, ASTNode **arms, size_t arm_count,
                          int exhaustive) {
  MemPath entry = {NULL, 0};
  MemPath merged = {NULL, 0};
  MemPath arm = {NULL, 0};
  size_t i;
  int have_merged = 0;

  if (ctx->depth > 0 || !mem_path_save(ctx, &entry)) {
    /* Already inside a path that is being joined, or out of memory: walk the
       arms the way they were always walked, conservatively. */
    for (i = 0; i < arm_count; i++) {
      if (!arms[i]) {
        continue;
      }
      {
        int level = ++ctx->scope_level;
        ctx->depth++;
        mem_walk_block(ctx, arms[i]);
        ctx->depth--;
        mem_scope_exit_check(ctx, level);
        ctx->scope_level--;
      }
    }
    mem_path_free(&entry);
    return;
  }

  for (i = 0; i < arm_count; i++) {
    if (!arms[i]) {
      continue;
    }
    if (!mem_walk_path(ctx, arms[i], &entry, &arm)) {
      mem_path_free(&arm);
      mem_path_free(&merged);
      mem_path_free(&entry);
      return;
    }
    if (!have_merged) {
      merged = arm;
      arm.locals = NULL;
      arm.count = 0;
      have_merged = 1;
    } else {
      mem_path_restore(ctx, &merged);
      mem_path_join(ctx, &merged, &arm);
      mem_path_free(&merged);
      mem_path_free(&arm);
      if (!mem_path_save(ctx, &merged)) {
        mem_path_free(&entry);
        return;
      }
    }
  }

  if (!have_merged) {
    mem_path_restore(ctx, &entry);
    mem_path_free(&entry);
    return;
  }

  if (!exhaustive) {
    mem_path_restore(ctx, &merged);
    mem_path_join(ctx, &merged, &entry);
  } else {
    mem_path_restore(ctx, &merged);
  }
  mem_path_free(&merged);
  mem_path_free(&entry);
}

static void mem_walk_branch(MemCtx *ctx, ASTNode *body) {
  ASTNode *arms[1];
  arms[0] = body;
  mem_walk_arms(ctx, arms, 1, 0);
}

static void mem_walk_return(MemCtx *ctx, ASTNode *statement) {
  ReturnStatement *ret = (ReturnStatement *)statement->data;
  if (!ret || !ret->value) {
    return;
  }
  mem_walk_expr(ctx, ret->value);
  if (ret->value) {
    ctx->saw_value_return = 1;
    /* Fresh if the value is an allocation expression, or a local that
     * holds one exclusively (the `var p = malloc(n); ...; return p;`
     * wrapper shape). A copy kept elsewhere disqualifies it: the caller
     * would not be the sole owner. */
    const char *via = NULL;
    MemLocal *returned_local = mem_expr_as_local(ctx, ret->value);
    int fresh = mem_is_allocation(ctx, ret->value, &via) ||
                (returned_local && returned_local->holds_alloc &&
                 !returned_local->escaped);
    if (!fresh) {
      ctx->returns_all_fresh = 0;
    }
  }
  if (ctx->fn_returns_pointer && ctx->mode == MEM_MODE_LOCAL) {
    MemLocal *stack_target = mem_addr_of_stack(ctx, ret->value);
    const char *via = NULL;
    if (!stack_target) {
      MemLocal *local = mem_expr_as_local(ctx, ret->value);
      if (local && local->points_to_stack) {
        stack_target = mem_referent(ctx, local);
        via = local->name;
      }
    }
    if (stack_target) {
      if (via) {
        mem_error(ctx, "M0103", ret->value->location,
                  "Allocate the memory (`new` / `malloc`) or have the "
                  "caller pass a buffer in",
                  "Returning `%s`, which points at stack local `%s`; the "
                  "frame is destroyed when this function returns, so the "
                  "caller receives a dangling pointer",
                  via, stack_target->name);
      } else {
        mem_error(ctx, "M0103", ret->value->location,
                  "Allocate the memory (`new` / `malloc`) or have the "
                  "caller pass a buffer in",
                  "Returning the address of stack local `%s`; the frame is "
                  "destroyed when this function returns, so the caller "
                  "receives a dangling pointer",
                  stack_target->name);
      }
    }
  }
  MemLocal *returned = mem_expr_as_local(ctx, ret->value);
  if (returned && returned->is_pointer) {
    returned->escaped = 1;
    if (ctx->mode == MEM_MODE_SUMMARY && returned->param_index >= 0 &&
        returned->param_index < MEM_MAX_PARAMS && ctx->collect) {
      ctx->collect->stores |= 1u << returned->param_index;
    }
  }
  return;
}

static void mem_walk_statement(MemCtx *ctx, ASTNode *statement) {
  if (!statement) {
    return;
  }
  switch (statement->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *decl = (VarDeclaration *)statement->data;
    if (!decl || !decl->name) {
      return;
    }
    const char *decl_type = decl->type_name;
    if (!decl_type) {
      long long structural_start = 0;
      if (decl->structural_type && decl->initializer &&
          type_checker_eval_integer_constant_with_checker(
              ctx->checker, decl->initializer, &structural_start)) {
        decl_type = "int64";
      }
    }
    if (!decl_type) {
      return;
    }
    if (decl->initializer) {
      mem_walk_expr(ctx, decl->initializer);
    }
    mem_add_local(ctx, decl->name, decl_type, statement->location, -1);
    if (decl->initializer) {
      mem_apply_assignment(ctx, decl->name, decl->initializer,
                           statement->location);
    }
    return;
  }
  case AST_ASSIGNMENT: {
    Assignment *assign = (Assignment *)statement->data;
    if (!assign) {
      return;
    }
    mem_walk_expr(ctx, assign->value);
    if (assign->target) {
      /* store through a field/index/deref: the value escapes */
      mem_walk_expr(ctx, assign->target);
      mem_check_handover_write(ctx, assign->target, statement->location);
      MemLocal *source = mem_expr_as_local(ctx, assign->value);
      if (source && source->is_pointer) {
        source->escaped = 1;
        if (ctx->mode == MEM_MODE_SUMMARY && source->param_index >= 0 &&
            source->param_index < MEM_MAX_PARAMS && ctx->collect) {
          ctx->collect->stores |= 1u << source->param_index;
        }
      }
      return;
    }
    if (assign->variable_name) {
      mem_apply_assignment(ctx, assign->variable_name, assign->value,
                           statement->location);
    }
    return;
  }
  case AST_RETURN_STATEMENT:
    mem_walk_return(ctx, statement);
    return;
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)statement->data;
    ASTNode **arms = NULL;
    size_t arm_count = 0;
    size_t i;
    if (!if_stmt) {
      return;
    }
    ctx->in_condition++;
    mem_walk_expr(ctx, if_stmt->condition);
    for (i = 0; i < if_stmt->else_if_count; i++) {
      mem_walk_expr(ctx, if_stmt->else_ifs[i].condition);
    }
    ctx->in_condition--;

    arm_count = 1 + if_stmt->else_if_count + (if_stmt->else_branch ? 1 : 0);
    arms = malloc(arm_count * sizeof(ASTNode *));
    if (!arms) {
      mem_walk_branch(ctx, if_stmt->then_branch);
      for (i = 0; i < if_stmt->else_if_count; i++) {
        mem_walk_branch(ctx, if_stmt->else_ifs[i].body);
      }
      if (if_stmt->else_branch) {
        mem_walk_branch(ctx, if_stmt->else_branch);
      }
      return;
    }
    arms[0] = if_stmt->then_branch;
    for (i = 0; i < if_stmt->else_if_count; i++) {
      arms[1 + i] = if_stmt->else_ifs[i].body;
    }
    if (if_stmt->else_branch) {
      arms[arm_count - 1] = if_stmt->else_branch;
    }
    /* An `else` makes one arm certain to run; without one, not running any of
       them is a path of its own. */
    mem_walk_arms(ctx, arms, arm_count, if_stmt->else_branch != NULL);
    free(arms);
    return;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *while_stmt = (WhileStatement *)statement->data;
    if (!while_stmt) {
      return;
    }
    mem_check_loop_bounds(ctx, while_stmt->condition, while_stmt->body, NULL);
    ctx->in_condition++;
    mem_walk_expr(ctx, while_stmt->condition);
    ctx->in_condition--;
    /* The body is one path and not running it is another, so what survives the
       loop is what holds either way. */
    mem_walk_branch(ctx, while_stmt->body);
    return;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *for_stmt = (ForStatement *)statement->data;
    if (!for_stmt) {
      return;
    }
    if (for_stmt->initializer) {
      mem_walk_statement(ctx, for_stmt->initializer);
    }
    mem_check_loop_bounds(ctx, for_stmt->condition, for_stmt->body,
                          for_stmt->increment);
    ctx->in_condition++;
    mem_walk_expr(ctx, for_stmt->condition);
    ctx->in_condition--;
    mem_walk_branch(ctx, for_stmt->body);
    if (for_stmt->increment) {
      int increment_depth = ctx->depth;
      ctx->depth++;
      mem_walk_statement(ctx, for_stmt->increment);
      ctx->depth = increment_depth;
    }
    return;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *switch_stmt = (SwitchStatement *)statement->data;
    if (!switch_stmt) {
      return;
    }
    ASTNode **arms = NULL;
    size_t arm_count = 0;
    int has_default = 0;
    size_t i;
    mem_walk_expr(ctx, switch_stmt->expression);
    if (switch_stmt->case_count > 0) {
      arms = malloc(switch_stmt->case_count * sizeof(ASTNode *));
    }
    if (!arms) {
      for (i = 0; i < switch_stmt->case_count; i++) {
        CaseClause *clause = switch_stmt->cases[i]
                                 ? (CaseClause *)switch_stmt->cases[i]->data
                                 : NULL;
        if (clause) {
          mem_walk_branch(ctx, clause->body);
        }
      }
      return;
    }
    for (i = 0; i < switch_stmt->case_count; i++) {
      CaseClause *clause = switch_stmt->cases[i]
                               ? (CaseClause *)switch_stmt->cases[i]->data
                               : NULL;
      arms[arm_count++] = clause ? clause->body : NULL;
      if (clause && clause->is_default) {
        has_default = 1;
      }
    }
    mem_walk_arms(ctx, arms, arm_count, has_default);
    free(arms);
    return;
  }
  case AST_MATCH_STATEMENT: {
    MatchStatement *match = (MatchStatement *)statement->data;
    if (!match) {
      return;
    }
    ASTNode **arms = NULL;
    size_t i;
    mem_walk_expr(ctx, match->expression);
    if (match->arm_count > 0) {
      arms = malloc(match->arm_count * sizeof(ASTNode *));
    }
    if (!arms) {
      for (i = 0; i < match->arm_count; i++) {
        mem_walk_branch(ctx, match->arms[i].body);
      }
      return;
    }
    for (i = 0; i < match->arm_count; i++) {
      arms[i] = match->arms[i].body;
    }
    /* A match names every variant or ends with a default, so one arm runs. */
    mem_walk_arms(ctx, arms, match->arm_count, 1);
    free(arms);
    return;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *defer = (DeferStatement *)statement->data;
    if (!defer) {
      return;
    }
    int saved = ctx->in_defer;
    ctx->in_defer = 1;
    mem_walk_statement(ctx, defer->statement);
    ctx->in_defer = saved;
    return;
  }
  case AST_PROGRAM: {
    /* a bare `{ ... }` block: a new lexical scope, but still on the spine
     * (unconditional), so borrows recorded inside are definite */
    int block_level = ++ctx->scope_level;
    mem_walk_block(ctx, statement);
    mem_scope_exit_check(ctx, block_level);
    ctx->scope_level--;
    return;
  }
  case AST_FUNCTION_CALL:
  case AST_FUNC_PTR_CALL:
  case AST_GPU_LAUNCH:
    mem_walk_expr(ctx, statement);
    return;
  default:
    for (size_t i = 0; i < statement->child_count; i++) {
      mem_walk_statement(ctx, statement->children[i]);
    }
    return;
  }
}

/* ---- shared walk setup ------------------------------------------------------------ */

static void mem_ctx_init(MemCtx *ctx, TypeChecker *checker, ASTNode *decl,
                         FunctionDeclaration *fn, MemMode mode,
                         const MemSummaryTable *summaries,
                         MemFnSummary *collect) {
  /* Everything except the trailing locals array; see the note on its
   * declaration for why that one is left alone. */
  memset(ctx, 0, offsetof(MemCtx, locals));
  ctx->checker = checker;
  ctx->fn = fn;
  ctx->fn_loc = decl->location;
  ctx->mode = mode;
  ctx->summaries = summaries;
  ctx->collect = collect;
  ctx->next_alias_group = 1;
  ctx->returns_all_fresh = 1;
  ctx->fn_returns_pointer = fn->return_type &&
                            mem_type_is_pointer(fn->return_type);
  for (size_t i = 0; i < fn->parameter_count; i++) {
    mem_add_local(ctx, fn->parameter_names[i], fn->parameter_types[i],
                  decl->location, (int)i);
  }
}

/* ---- phase 1 entry point ------------------------------------------------------------ */

int type_checker_check_function_memory(TypeChecker *checker,
                                       ASTNode *declaration) {
  if (!checker || !checker->error_reporter || !declaration ||
      declaration->type != AST_FUNCTION_DECLARATION) {
    return 1;
  }
  FunctionDeclaration *fn = (FunctionDeclaration *)declaration->data;
  if (!fn || !fn->body || fn->is_extern) {
    return 1;
  }

  MemCtx ctx;
  mem_ctx_init(&ctx, checker, declaration, fn, MEM_MODE_LOCAL, NULL, NULL);
  mem_walk_block(&ctx, fn->body);
  return ctx.had_error ? 0 : 1;
}

/* ---- phase 2: whole-program ownership inference ------------------------------------- */

static int mem_decl_is_analyzable(ASTNode *decl) {
  if (!decl || decl->type != AST_FUNCTION_DECLARATION) {
    return 0;
  }
  FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
  return fn && fn->body && !fn->is_extern && fn->type_param_count == 0 &&
         fn->name;
}

/* One summary-collection walk of `decl`; returns 1 when the recorded facts
 * changed (drives the fixpoint). */
static int mem_collect_summary(TypeChecker *checker, ASTNode *decl,
                               MemSummaryTable *table, MemFnSummary *summary) {
  FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
  MemFnSummary before = *summary;

  MemCtx ctx;
  mem_ctx_init(&ctx, checker, decl, fn, MEM_MODE_SUMMARY, table, summary);
  mem_walk_block(&ctx, fn->body);

  /* returns_fresh: a pointer-returning function whose every value-return is
   * a fresh allocation behaves like malloc for its callers. */
  int fresh = ctx.fn_returns_pointer && ctx.saw_value_return &&
              ctx.returns_all_fresh;
  summary->returns_fresh |= fresh; /* monotone: only ever turns on */

  return summary->frees_definite != before.frees_definite ||
         summary->frees_maybe != before.frees_maybe ||
         summary->stores != before.stores ||
         summary->returns_fresh != before.returns_fresh;
}

int type_checker_check_program_memory(TypeChecker *checker, ASTNode *program) {
  if (!checker || !checker->error_reporter || !program ||
      program->type != AST_PROGRAM || checker->has_error) {
    return 1;
  }
  Program *prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  /* Seed the table with the C allocator externs, then one slot per
   * analyzable function. */
  size_t capacity = prog->declaration_count + 4;
  MemFnSummary *items = calloc(capacity, sizeof(MemFnSummary));
  ASTNode **decls = calloc(capacity, sizeof(ASTNode *));
  if (!items || !decls) {
    free(items);
    free(decls);
    return 1;
  }
  MemSummaryTable table = {items, 0, NULL, 0};
  {
    size_t nb = 64;
    while (nb < capacity * 2) {
      nb *= 2;
    }
    table.buckets = calloc(nb, sizeof(size_t));
    table.bucket_count = table.buckets ? nb : 0;
  }

  items[table.count] = (MemFnSummary){"free", NULL, 1u, 0, 0, 0};
  mem_summary_index_put(&table, table.count++);
  items[table.count] = (MemFnSummary){"realloc", NULL, 1u, 0, 0, 1};
  mem_summary_index_put(&table, table.count++);
  items[table.count] = (MemFnSummary){"malloc", NULL, 0, 0, 0, 1};
  mem_summary_index_put(&table, table.count++);
  items[table.count] = (MemFnSummary){"calloc", NULL, 0, 0, 0, 1};
  mem_summary_index_put(&table, table.count++);

  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (!mem_decl_is_analyzable(prog->declarations[i]) ||
        table.count >= capacity) {
      continue;
    }
    FunctionDeclaration *fn =
        (FunctionDeclaration *)prog->declarations[i]->data;
    if (mem_summary_find(&table, fn->name)) {
      continue; /* duplicate name: first definition wins */
    }
    decls[table.count] = prog->declarations[i];
    items[table.count] = (MemFnSummary){fn->name, fn, 0, 0, 0, 0};
    mem_summary_index_put(&table, table.count);
    table.count++;
  }

  /* Fixpoint: facts are monotone (bits only get set), so this terminates;
   * the iteration cap is belt-and-braces. */
  for (int iteration = 0; iteration < MEM_SUMMARY_MAX_ITER; iteration++) {
    int changed = 0;
    for (size_t i = 0; i < table.count; i++) {
      if (decls[i]) {
        changed |= mem_collect_summary(checker, decls[i], &table, &items[i]);
      }
    }
    if (!changed) {
      break;
    }
  }

  /* Reporting pass: summary-aware leaks and cross-call use-after-free. */
  for (size_t i = 0; i < table.count; i++) {
    if (!decls[i]) {
      continue;
    }
    FunctionDeclaration *fn = (FunctionDeclaration *)decls[i]->data;
    MemCtx ctx;
    mem_ctx_init(&ctx, checker, decls[i], fn, MEM_MODE_INTERPROC, &table,
                 NULL);
    mem_walk_block(&ctx, fn->body);

    /* Leaks: a spine allocation that was never freed (not even in a defer)
     * and never left the function has no owner when the function returns.
     * `main` is exempt: process exit reclaims everything, and warning
     * about it would train people to ignore this diagnostic. */
    if (strcmp(fn->name, "main") != 0) {
      for (size_t j = 0; j < ctx.local_count; j++) {
        MemLocal *local = &ctx.locals[j];
        if (!local->holds_alloc || local->ever_freed || local->escaped) {
          continue;
        }
        char suggestion[160];
        snprintf(suggestion, sizeof(suggestion),
                 "Add `defer free(%s);` right after the allocation, or free "
                 "it on every path out of `%s`",
                 local->name, fn->name);
        if (local->alloc_via) {
          mem_warn_suggest(&ctx, "M0107", local->alloc_loc, suggestion,
                           "`%s` holds the allocation `%s` returns, but it "
                           "is never freed, returned, stored, or passed on; "
                           "the allocation leaks when `%s` returns",
                           local->name, local->alloc_via, fn->name);
        } else {
          mem_warn_suggest(&ctx, "M0107", local->alloc_loc, suggestion,
                           "`%s` is allocated here but never freed, "
                           "returned, stored, or passed on; the allocation "
                           "leaks when `%s` returns",
                           local->name, fn->name);
        }
      }
    }
  }

  free(table.buckets);
  free(items);
  free(decls);
  return !checker->has_error;
}
