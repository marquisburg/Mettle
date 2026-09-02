/* Reference interpreter for optimized IR: the semantic arbiter behind
 * --verify. See ir_interp.h for the model. Every opcode implementation here
 * encodes the DOCUMENTED semantics from ir.h; if a pass emits IR whose real
 * meaning differs from what it documents, the before/after comparison in
 * ir_verify.c diverges and the pass is caught. */
#include "ir_interp.h"
#include "../runtime/mt_math.h"
#include "../common.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define II_ADDR_BASE 0x0000200000000000ULL
#define II_POISON_BYTE 0xA5
#define II_ADDR_STRIDE 0x0000000001000000ULL /* 16 MiB per buffer slot */
#define II_MAX_BUFFERS 65536
#define II_MAX_DEPTH 256
/* Storage grows on demand, so the cap only bounds a pathological run; fuel
 * bounds the entry count long before the memory matters. 512 made a test that
 * prints a thousand lines die with "extern trace overflow". */
#define II_TRACE_CAP (1 << 20)
#define II_MAX_BUFFER_SIZE (16LL * 1024 * 1024)

/* Function-address tokens. Taking a function's address yields a deterministic
 * value in a region disjoint from buffer space; an indirect call maps it back.
 * Defined functions index program->functions, extern declarations index the
 * module symbol table. */
#define II_FN_ADDR_BASE 0x0000100000000000ULL
#define II_XFN_ADDR_BASE 0x0000180000000000ULL
#define II_FN_ADDR_STRIDE 16ULL

typedef struct {
  unsigned long long base;
  unsigned char *data;
  long long size;
  int freed;
  /* A frame local whose address the function returned (the aggregate-return
   * convention: the value travels as an address). It outlives its frame until
   * the aggregate assignment that consumes it copies it out. */
  int escaped_local;
  size_t alloc_line; /* NEW/malloc source line; 0 for harness inputs */
  /* One byte per data byte: nonzero once something has written it. Buffers
     start known (zeroed or seeded); the sites that stamp the uninitialized
     pattern clear it here too, so a read can tell an undefined byte from a
     byte that genuinely holds 0xA5. */
  unsigned char *init_map;
} IIBuffer;

/* A materialized string literal: the characters (NUL-terminated) and the
 * { chars, length } record a fat `string` value points at, both inside one
 * buffer, laid out the way the backend parks them in .rdata. */
typedef struct {
  const char *text; /* not owned; lives as long as the IR */
  size_t length;    /* bytes, which is not strlen once a literal holds a NUL */
  unsigned long long chars;
  unsigned long long record;
} IILiteral;

typedef struct {
  const char *key; /* not owned; lives as long as the IR */
  IRInterpValue value;
  /* Slot-backed local: reads/writes go through memory at value.i. */
  int slotted;
  int slot_size;
  int slot_is_float;
  int slot_is_unsigned;
  int slot_alias;
  /* Aggregate local or global: value.i is the base address of its storage and
   * assignment through the name is a block copy of this many bytes. */
  long long agg_size;
  /* Storage was allocated for this variable by a DECLARE_LOCAL (or a global
   * materialization); a re-executed declaration re-poisons instead of
   * allocating again, so a loop body's local costs one buffer, not one per
   * iteration. */
  int has_local_storage;
  /* Global whose initializer has been applied on first touch. */
  int global_inited;
  /* Declared integer width of a register-resident local or parameter, in
     bytes, and its signedness. A write is narrowed to it, so a narrow local
     wraps here exactly as it wraps in a register: without this an int32 that
     overflows keeps all 64 bits and the interpreter disagrees with the
     machine -- which would make mettle test, trace, --pgo and the two
     differential gates disagree with the program they are describing.
     0 means unknown (a temp, a global first touched by a write), which keeps
     the full width. */
  int value_size;
  int value_is_unsigned;
  unsigned long long string_record;
  unsigned char is_cstring;
} IIVar;

typedef struct {
  IIVar *vars;
  size_t count;
  size_t capacity; /* power of two; 0 = empty */
} IIEnv;

struct IRInterpMachine {
  IRProgram *program;
  const char *override_name;
  IRFunction *override_fn;

  /* Grown on demand (capped at II_MAX_BUFFERS / II_TRACE_CAP). Embedding the
   * full-capacity arrays made the machine struct ~570 KB, and translation
   * validation creates tens of thousands of machines per compile - the calloc
   * zeroing alone dominated --verify wall time. Entries are fully initialized
   * on write, so the grown storage is plain malloc. */
  IIBuffer *buffers;
  size_t buffer_count;
  size_t buffer_capacity;

  /* Reclaimed frame-local slots, reused by later locals the way a native
   * frame reuses stack. Heap frees never enter this list, so a freed heap
   * buffer stays a tombstone and use-after-free keeps trapping. */
  size_t *free_slots;
  size_t free_slot_count;
  size_t free_slot_capacity;

  IILiteral *literals;
  size_t literal_count;
  size_t literal_capacity;

  IIEnv globals;

  IRInterpExternCall *trace;
  size_t trace_count;
  size_t trace_capacity;

  long long fuel;
  int depth;
  IRInterpStatus status;
  char detail[128];

  /* Source location of the CALL currently dispatching to an extern; used to
   * attribute assert failures and heap allocations to source lines. */
  SourceLocation current_call_loc;

  /* assert()/assert_eq() failure details (mettle test). */
  int assert_failed;
  size_t assert_line;
  size_t assert_column;
  IRInterpValue assert_left;
  IRInterpValue assert_right;
  int assert_is_eq;

  /* Value tracing (mettle trace). */
  IRInterpValueHook value_hook;
  void *value_hook_ctx;
  const IRFunction *value_hook_fn;

  int held_mutex_count;
  int last_read_undefined;
  int last_socket_error;
  int branched_on_undefined;
  unsigned long long next_thread_handle;

  unsigned long long swap_slots[256];
  unsigned long long swap_replacements[256];
  long long swap_pending_count;

  /* Execution counting (zero-run PGO). */
  int count_enabled;
  struct {
    const IRFunction *fn;
    long long *counts;
    size_t n;
  } *count_tables;
  size_t count_table_count;
  size_t count_table_capacity;
};

/* ---------------- environment ---------------- */

static size_t ii_hash(const char *s) {
  size_t h = 1469598103934665603ull;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 1099511628211ull;
  }
  return h;
}

static void ii_env_free(IIEnv *env) {
  free(env->vars);
  env->vars = NULL;
  env->count = 0;
  env->capacity = 0;
}

static IIVar *ii_env_slot(IIEnv *env, const char *key) {
  size_t mask = env->capacity - 1;
  size_t i = ii_hash(key) & mask;
  while (env->vars[i].key) {
    if (strcmp(env->vars[i].key, key) == 0) {
      return &env->vars[i];
    }
    i = (i + 1) & mask;
  }
  return &env->vars[i];
}

static int ii_env_grow(IIEnv *env) {
  size_t new_capacity = env->capacity ? env->capacity * 2 : 64;
  IIVar *old = env->vars;
  size_t old_capacity = env->capacity;
  IIVar *grown = (IIVar *)calloc(new_capacity, sizeof(IIVar));
  if (!grown) {
    return 0;
  }
  env->vars = grown;
  env->capacity = new_capacity;
  env->count = 0;
  for (size_t i = 0; i < old_capacity; i++) {
    if (old[i].key) {
      IIVar *slot = ii_env_slot(env, old[i].key);
      *slot = old[i];
      env->count++;
    }
  }
  free(old);
  return 1;
}

/* Find existing entry or NULL. */
static IIVar *ii_env_find(IIEnv *env, const char *key) {
  if (!env->capacity) {
    return NULL;
  }
  IIVar *slot = ii_env_slot(env, key);
  return slot->key ? slot : NULL;
}

/* Find or insert (zero value). Returns NULL only on OOM. */
static IIVar *ii_env_upsert(IIEnv *env, const char *key) {
  if (env->capacity == 0 || env->count * 10 >= env->capacity * 7) {
    if (!ii_env_grow(env)) {
      return NULL;
    }
  }
  IIVar *slot = ii_env_slot(env, key);
  if (!slot->key) {
    slot->key = key;
    memset(&slot->value, 0, sizeof(slot->value));
    slot->slotted = 0;
    env->count++;
  }
  return slot;
}

/* ---------------- machine ---------------- */

IRInterpMachine *ir_interp_create(IRProgram *program) {
  IRInterpMachine *machine = (IRInterpMachine *)calloc(1, sizeof(*machine));
  if (!machine) {
    return NULL;
  }
  machine->program = program;
  machine->status = IR_INTERP_OK;
  return machine;
}

void ir_interp_destroy(IRInterpMachine *machine) {
  if (!machine) {
    return;
  }
  for (size_t i = 0; i < machine->buffer_count; i++) {
    free(machine->buffers[i].data);
    free(machine->buffers[i].init_map);
  }
  free(machine->buffers);
  free(machine->free_slots);
  free(machine->literals);
  free(machine->trace);
  for (size_t i = 0; i < machine->count_table_count; i++) {
    free(machine->count_tables[i].counts);
  }
  free(machine->count_tables);
  ii_env_free(&machine->globals);
  free(machine);
}

void ir_interp_enable_counting(IRInterpMachine *machine) {
  if (machine) {
    machine->count_enabled = 1;
  }
}

static long long *ii_counts_for(IRInterpMachine *machine,
                                const IRFunction *fn) {
  if (!machine->count_enabled || !fn || fn->instruction_count == 0) {
    return NULL;
  }
  for (size_t i = 0; i < machine->count_table_count; i++) {
    if (machine->count_tables[i].fn == fn) {
      return machine->count_tables[i].counts;
    }
  }
  if (machine->count_table_count >= machine->count_table_capacity) {
    size_t new_capacity =
        machine->count_table_capacity ? machine->count_table_capacity * 2 : 32;
    void *grown = realloc(machine->count_tables,
                          new_capacity * sizeof(*machine->count_tables));
    if (!grown) {
      return NULL;
    }
    machine->count_tables = grown;
    machine->count_table_capacity = new_capacity;
  }
  long long *counts =
      (long long *)calloc(fn->instruction_count, sizeof(long long));
  if (!counts) {
    return NULL;
  }
  machine->count_tables[machine->count_table_count].fn = fn;
  machine->count_tables[machine->count_table_count].counts = counts;
  machine->count_tables[machine->count_table_count].n = fn->instruction_count;
  machine->count_table_count++;
  return counts;
}

const long long *ir_interp_get_counts(const IRInterpMachine *machine,
                                      const IRFunction *function,
                                      size_t *count_out) {
  if (!machine || !function) {
    return NULL;
  }
  for (size_t i = 0; i < machine->count_table_count; i++) {
    if (machine->count_tables[i].fn == function) {
      if (count_out) {
        *count_out = machine->count_tables[i].n;
      }
      return machine->count_tables[i].counts;
    }
  }
  return NULL;
}

void ir_interp_set_override(IRInterpMachine *machine, const char *name,
                            IRFunction *fn) {
  if (!machine) {
    return;
  }
  machine->override_name = name;
  machine->override_fn = fn;
}

static void ii_fail(IRInterpMachine *machine, IRInterpStatus status,
                    const char *detail) {
  if (machine->status == IR_INTERP_OK) {
    machine->status = status;
    snprintf(machine->detail, sizeof(machine->detail), "%s",
             detail ? detail : "");
  }
}

static int ii_free_slot_push(IRInterpMachine *machine, size_t index) {
  if (machine->free_slot_count == machine->free_slot_capacity) {
    size_t grown =
        machine->free_slot_capacity ? machine->free_slot_capacity * 2 : 16;
    size_t *table =
        (size_t *)realloc(machine->free_slots, grown * sizeof(size_t));
    if (!table) {
      return 0;
    }
    machine->free_slots = table;
    machine->free_slot_capacity = grown;
  }
  machine->free_slots[machine->free_slot_count++] = index;
  return 1;
}

/* Give a frame local's slot back: the data is released and the slot becomes
 * reusable by a later local, the way returning frames reuse stack pages. */
static void ii_reclaim_buffer(IRInterpMachine *machine, size_t index) {
  IIBuffer *buf = &machine->buffers[index];
  free(buf->data);
  free(buf->init_map);
  buf->data = NULL;
  buf->init_map = NULL;
  buf->freed = 1;
  buf->escaped_local = 0;
  ii_free_slot_push(machine, index);
}

static void ii_mark_bytes(IIBuffer *buf, long long offset, long long length,
                          unsigned char known) {
  if (!buf || !buf->init_map || offset < 0 || length <= 0 ||
      offset + length > buf->size) {
    return;
  }
  memset(buf->init_map + offset, known, (size_t)length);
}

static unsigned long long ii_add_buffer_ex(IRInterpMachine *machine,
                                           const void *init, long long size,
                                           int allow_reuse) {
  if (!machine || size < 0 || size > II_MAX_BUFFER_SIZE) {
    return 0;
  }
  size_t index;
  if (allow_reuse && machine->free_slot_count > 0) {
    index = machine->free_slots[--machine->free_slot_count];
  } else {
    if (machine->buffer_count >= II_MAX_BUFFERS) {
      return 0;
    }
    if (machine->buffer_count == machine->buffer_capacity) {
      size_t grown =
          machine->buffer_capacity ? machine->buffer_capacity * 2 : 16;
      IIBuffer *table =
          (IIBuffer *)realloc(machine->buffers, grown * sizeof(IIBuffer));
      if (!table) {
        return 0;
      }
      machine->buffers = table;
      machine->buffer_capacity = grown;
    }
    index = machine->buffer_count;
  }
  IIBuffer *buf = &machine->buffers[index];
  buf->size = size;
  buf->freed = 0;
  buf->escaped_local = 0;
  buf->alloc_line = 0;
  buf->base = II_ADDR_BASE + (unsigned long long)index * II_ADDR_STRIDE;
  buf->init_map = NULL;
  buf->data = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
  if (buf->data) {
    buf->init_map = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    if (!buf->init_map) {
      free(buf->data);
      buf->data = NULL;
    } else {
      memset(buf->init_map, 1, size > 0 ? (size_t)size : 1);
    }
  }
  if (!buf->data) {
    if (index != machine->buffer_count) {
      buf->freed = 1;
      ii_free_slot_push(machine, index);
    }
    return 0;
  }
  if (init) {
    memcpy(buf->data, init, (size_t)size);
  } else {
    memset(buf->data, 0, (size_t)size);
  }
  if (index == machine->buffer_count) {
    machine->buffer_count++;
  }
  return buf->base;
}

unsigned long long ir_interp_add_buffer(IRInterpMachine *machine,
                                        const void *init, long long size) {
  return ii_add_buffer_ex(machine, init, size, 0);
}

static IIBuffer *ii_addr_to_buffer(IRInterpMachine *machine,
                                   unsigned long long addr, long long size,
                                   long long *offset_out) {
  if (addr < II_ADDR_BASE) {
    return NULL;
  }
  unsigned long long index = (addr - II_ADDR_BASE) / II_ADDR_STRIDE;
  if (index >= machine->buffer_count) {
    return NULL;
  }
  IIBuffer *buf = &machine->buffers[index];
  long long offset = (long long)(addr - buf->base);
  if (buf->freed || offset < 0 || size < 0 || offset + size > buf->size) {
    return NULL;
  }
  *offset_out = offset;
  return buf;
}

/* Give a string literal real bytes, so a callee that reads them (`print`
 * loading msg.length, `strlen` scanning for the terminator) sees memory rather
 * than a made-up token. One buffer holds the characters, a NUL, and the
 * 16-byte { chars, length } record; the address handed to a call is the record
 * for a fat `string` parameter and the characters for a `cstring` one, exactly
 * as the backend chooses between them. Cached per literal so the same call in
 * a loop does not exhaust the buffer table. */
/* `length` is the literal's byte count. It is passed rather than measured
 * because `\\0` is a legal escape: the bytes can run past an interior NUL, and
 * two literals that share a prefix up to one are different strings. */
static int ii_string_literal(IRInterpMachine *machine, const char *text,
                             size_t length, unsigned long long *chars_out,
                             unsigned long long *record_out) {
  if (!text) {
    text = "";
    length = 0;
  }
  for (size_t i = 0; i < machine->literal_count; i++) {
    if (machine->literals[i].length == length &&
        memcmp(machine->literals[i].text, text, length) == 0) {
      *chars_out = machine->literals[i].chars;
      *record_out = machine->literals[i].record;
      return 1;
    }
  }
  size_t record_offset = (length + 1 + 7u) & ~(size_t)7u;
  unsigned long long addr =
      ir_interp_add_buffer(machine, NULL, (long long)(record_offset + 16u));
  if (!addr) {
    ii_fail(machine, IR_INTERP_TRAP, "string literal storage");
    return 0;
  }
  IIBuffer *buf = &machine->buffers[machine->buffer_count - 1];
  memcpy(buf->data, text, length);
  unsigned long long chars = addr;
  unsigned long long chars_length = (unsigned long long)length;
  memcpy(buf->data + record_offset, &chars, 8);
  memcpy(buf->data + record_offset + 8, &chars_length, 8);

  if (machine->literal_count == machine->literal_capacity) {
    size_t grown = machine->literal_capacity ? machine->literal_capacity * 2 : 8;
    IILiteral *table =
        (IILiteral *)realloc(machine->literals, grown * sizeof(IILiteral));
    if (!table) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    machine->literals = table;
    machine->literal_capacity = grown;
  }
  machine->literals[machine->literal_count].text = text;
  machine->literals[machine->literal_count].length = length;
  machine->literals[machine->literal_count].chars = addr;
  machine->literals[machine->literal_count].record = addr + record_offset;
  machine->literal_count++;

  *chars_out = addr;
  *record_out = addr + record_offset;
  return 1;
}

/* A fresh heap string record, the shape the concat kernel and the
 * mettle_string_from_* conversions produce at runtime: bytes, a NUL the type
 * does not count, then the {chars, length} record. Returns the record address,
 * 0 on failure. */
static unsigned long long ii_make_string(IRInterpMachine *machine,
                                         const char *bytes, size_t length) {
  size_t record_offset = (length + 1 + 7u) & ~(size_t)7u;
  unsigned long long addr =
      ir_interp_add_buffer(machine, NULL, (long long)(record_offset + 16u));
  if (!addr) {
    return 0;
  }
  IIBuffer *buf = &machine->buffers[machine->buffer_count - 1];
  if (bytes && length > 0) {
    memcpy(buf->data, bytes, length);
  }
  unsigned long long chars = addr;
  unsigned long long chars_length = (unsigned long long)length;
  memcpy(buf->data + record_offset, &chars, 8);
  memcpy(buf->data + record_offset + 8, &chars_length, 8);
  return addr + record_offset;
}

/* Resolve a string VALUE (the address of a {chars, length} record) to its
 * bytes. Returns 0 when either the record or the byte range is not interpreter
 * memory. */
static int ii_read_string(IRInterpMachine *machine,
                          unsigned long long record_addr,
                          const unsigned char **bytes_out, size_t *length_out) {
  long long record_offset = 0;
  IIBuffer *record_buf =
      ii_addr_to_buffer(machine, record_addr, 16, &record_offset);
  if (!record_buf) {
    return 0;
  }
  unsigned long long chars = 0, length = 0;
  memcpy(&chars, record_buf->data + record_offset, 8);
  memcpy(&length, record_buf->data + record_offset + 8, 8);
  if (length > (unsigned long long)II_MAX_BUFFER_SIZE) {
    return 0;
  }
  if (length == 0) {
    *bytes_out = (const unsigned char *)"";
    *length_out = 0;
    return 1;
  }
  long long chars_offset = 0;
  IIBuffer *chars_buf =
      ii_addr_to_buffer(machine, chars, (long long)length, &chars_offset);
  if (!chars_buf) {
    return 0;
  }
  *bytes_out = chars_buf->data + chars_offset;
  *length_out = (size_t)length;
  return 1;
}

/* ---------------- function-address tokens ---------------- */

static unsigned long long ii_function_token(IRInterpMachine *machine,
                                            const char *name) {
  if (!machine->program || !name) {
    return 0;
  }
  for (size_t i = 0; i < machine->program->function_count; i++) {
    IRFunction *fn = machine->program->functions[i];
    if (fn && fn->name && strcmp(fn->name, name) == 0) {
      return II_FN_ADDR_BASE + (unsigned long long)i * II_FN_ADDR_STRIDE;
    }
  }
  for (size_t i = 0; i < machine->program->module_symbol_count; i++) {
    const IRModuleSymbol *sym = &machine->program->module_symbols[i];
    if (sym->kind == IR_MODSYM_FUNCTION && sym->name &&
        strcmp(sym->name, name) == 0) {
      return II_XFN_ADDR_BASE + (unsigned long long)i * II_FN_ADDR_STRIDE;
    }
  }
  return 0;
}

int ir_interp_buffer_is_literal(const IRInterpMachine *machine,
                                size_t index) {
  size_t i = 0;
  unsigned long long base = 0;
  if (!machine || index >= machine->buffer_count) {
    return 0;
  }
  base = machine->buffers[index].base;
  for (i = 0; i < machine->literal_count; i++) {
    if (machine->literals[i].chars == base) {
      return 1;
    }
  }
  return 0;
}

unsigned long long ir_interp_function_address(IRInterpMachine *machine,
                                              const char *name) {
  return machine ? ii_function_token(machine, name) : 0;
}

/* Map a token back: a defined function to execute, or the extern's name. */
static IRFunction *ii_token_function(IRInterpMachine *machine,
                                     unsigned long long token,
                                     const char **extern_name) {
  *extern_name = NULL;
  if (!machine->program) {
    return NULL;
  }
  if (token >= II_FN_ADDR_BASE && token < II_XFN_ADDR_BASE) {
    unsigned long long index =
        (token - II_FN_ADDR_BASE) / II_FN_ADDR_STRIDE;
    if (index < machine->program->function_count) {
      return machine->program->functions[index];
    }
    return NULL;
  }
  if (token >= II_XFN_ADDR_BASE && token < II_ADDR_BASE) {
    unsigned long long index =
        (token - II_XFN_ADDR_BASE) / II_FN_ADDR_STRIDE;
    if (index < machine->program->module_symbol_count) {
      *extern_name = machine->program->module_symbols[index].name;
    }
  }
  return NULL;
}

static int ii_mem_read(IRInterpMachine *machine, unsigned long long addr,
                       int size, unsigned long long *out) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP, "load out of bounds / after free");
    return 0;
  }
  unsigned long long value = 0;
  memcpy(&value, buf->data + offset, (size_t)size);
  /* Every byte unwritten, not merely some: a struct copy legitimately reads
     the padding between fields, and an enum reads a payload its tag says is
     absent. Those carry real data alongside. A read where nothing at all was
     written carries none, and its value is whatever the allocator or the
     stack left there. */
  /* Both signals, not either: the map says nothing wrote these bytes and the
     bytes still carry the pattern. The map alone would accuse every write
     path that reaches memory without going through here; the bytes alone
     would accuse any program that legitimately stores 0xA5. */
  machine->last_read_undefined = 0;
  if (buf->init_map) {
    int undefined = 1;
    for (int b = 0; b < size && undefined; b++) {
      undefined = buf->init_map[offset + b] == 0 &&
                  buf->data[offset + b] == II_POISON_BYTE;
    }
    if (undefined) {
      machine->last_read_undefined = 1;
    }
  }
  *out = value;
  return 1;
}

static int ii_mem_write(IRInterpMachine *machine, unsigned long long addr,
                        int size, unsigned long long value) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP, "store out of bounds / after free");
    return 0;
  }
  memcpy(buf->data + offset, &value, (size_t)size);
  ii_mark_bytes(buf, offset, size, 1);
  return 1;
}

/* Typed element helpers for the SIMD kernels. */
static int ii_read_i32(IRInterpMachine *m, unsigned long long a, int *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 4, &raw)) return 0;
  *v = (int)(unsigned int)raw;
  return 1;
}
static int ii_write_i32(IRInterpMachine *m, unsigned long long a, int v) {
  return ii_mem_write(m, a, 4, (unsigned int)v);
}
/* A byte widened into an int32 lane, and the truncation back. */
static int ii_read_byte_as_i32(IRInterpMachine *m, unsigned long long a,
                               int is_unsigned, int *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 1, &raw)) return 0;
  *v = is_unsigned ? (int)(unsigned char)raw : (int)(signed char)raw;
  return 1;
}
static int ii_write_byte(IRInterpMachine *m, unsigned long long a, int v) {
  return ii_mem_write(m, a, 1, (unsigned long long)(unsigned char)(unsigned int)v);
}
static int ii_read_f64(IRInterpMachine *m, unsigned long long a, double *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 8, &raw)) return 0;
  memcpy(v, &raw, 8);
  return 1;
}
static int ii_write_f64(IRInterpMachine *m, unsigned long long a, double v) {
  unsigned long long raw;
  memcpy(&raw, &v, 8);
  return ii_mem_write(m, a, 8, raw);
}
static int ii_read_f32(IRInterpMachine *m, unsigned long long a, float *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 4, &raw)) return 0;
  unsigned int bits = (unsigned int)raw;
  memcpy(v, &bits, 4);
  return 1;
}
static int ii_write_f32(IRInterpMachine *m, unsigned long long a, float v) {
  unsigned int bits;
  memcpy(&bits, &v, 4);
  return ii_mem_write(m, a, 4, bits);
}

/* ---------------- frames ---------------- */

typedef struct {
  const char *label;
  size_t index;
} IILabel;

typedef struct {
  IIEnv env;
  IILabel *labels;
  size_t label_count;
  IRFunction *fn;
  /* Buffer slots this frame's locals occupy, reclaimed on return the way a
   * native frame's stack is. */
  size_t *owned;
  size_t owned_count;
  size_t owned_capacity;
} IIFrame;

static int ii_frame_own(IIFrame *frame, size_t index) {
  if (frame->owned_count == frame->owned_capacity) {
    size_t grown = frame->owned_capacity ? frame->owned_capacity * 2 : 8;
    size_t *table = (size_t *)realloc(frame->owned, grown * sizeof(size_t));
    if (!table) {
      return 0;
    }
    frame->owned = table;
    frame->owned_capacity = grown;
  }
  frame->owned[frame->owned_count++] = index;
  return 1;
}

static const IILabel *ii_find_label(const IIFrame *frame, const char *name) {
  for (size_t i = 0; i < frame->label_count; i++) {
    if (strcmp(frame->labels[i].label, name) == 0) {
      return &frame->labels[i];
    }
  }
  return NULL;
}

/* Parse a DECLARE_LOCAL type string. Returns 1 on success with element size,
 * count (1 for scalars), float-ness, unsignedness; 0 for uninterpretable
 * types (structs, strings, closures). */
static int ii_parse_local_type(const char *text, int *elem_size, long long *count,
                               int *is_float, int *is_unsigned) {
  static const struct {
    const char *name;
    int size;
    int is_float;
    int is_unsigned;
  } SCALARS[] = {
      {"int8", 1, 0, 0},    {"int16", 2, 0, 0},   {"int32", 4, 0, 0},
      {"int64", 8, 0, 0},   {"uint8", 1, 0, 1},   {"uint16", 2, 0, 1},
      {"uint32", 4, 0, 1},  {"uint64", 8, 0, 1},  {"bool", 1, 0, 1},
      {"float32", 4, 1, 0}, {"float64", 8, 1, 0},
      {"float16", 2, 1, 0}, {"bfloat16", 2, 1, 0},
  };
  if (!text) {
    return 0;
  }
  size_t len = strlen(text);
  *count = 1;
  /* Pointer-typed local: an 8-byte scalar value. */
  if (len > 0 && text[len - 1] == '*') {
    *elem_size = 8;
    *is_float = 0;
    *is_unsigned = 1;
    return 1;
  }
  /* Function-pointer and closure locals are 8-byte code/record pointers. */
  if (strncmp(text, "fn(", 3) == 0 || strncmp(text, "Fn(", 3) == 0) {
    *elem_size = 8;
    *is_float = 0;
    *is_unsigned = 1;
    return 1;
  }
  if (strcmp(text, "cstring") == 0 || strcmp(text, "rawptr") == 0) {
    *elem_size = 8;
    *is_float = 0;
    *is_unsigned = 1;
    return 1;
  }
  char base[32];
  const char *bracket = strchr(text, '[');
  if (bracket) {
    size_t base_len = (size_t)(bracket - text);
    if (base_len >= sizeof(base)) {
      return 0;
    }
    memcpy(base, text, base_len);
    base[base_len] = '\0';
    /* Every dimension multiplies: `int32[3][4]` holds twelve elements, not
     * three. Reading only the first sized a two-dimensional local at its outer
     * count, and the first store past the opening row ran off the buffer. */
    long long n = 1;
    const char *scan = bracket;
    while (*scan == '[') {
      const char *digit = scan + 1;
      long long dim = 0;
      if (*digit < '0' || *digit > '9') {
        return 0;
      }
      for (; *digit >= '0' && *digit <= '9'; digit++) {
        dim = dim * 10 + (*digit - '0');
        if (dim > (1 << 24)) {
          return 0;
        }
      }
      if (*digit != ']' || dim <= 0 || n > (1 << 24) / dim) {
        return 0;
      }
      n *= dim;
      scan = digit + 1;
    }
    if (*scan != '\0') {
      return 0;
    }
    if (strcmp(base, "cstring") == 0 || strcmp(base, "rawptr") == 0) {
      *elem_size = 8;
      *is_float = 0;
      *is_unsigned = 1;
      *count = n;
      return 1;
    }
    *count = n;
  } else {
    if (len >= sizeof(base)) {
      return 0;
    }
    memcpy(base, text, len + 1);
  }
  for (size_t i = 0; i < sizeof(SCALARS) / sizeof(SCALARS[0]); i++) {
    if (strcmp(base, SCALARS[i].name) == 0) {
      *elem_size = SCALARS[i].size;
      *is_float = SCALARS[i].is_float;
      *is_unsigned = SCALARS[i].is_unsigned;
      return 1;
    }
  }
  return 0;
}

/* ---------------- value plumbing ---------------- */

static long long ii_as_int(const IRInterpValue *value) {
  return value->is_float ? (long long)value->f : value->i;
}

static double ii_as_float(const IRInterpValue *value) {
  return value->is_float ? value->f : (double)value->i;
}

static IRInterpValue ii_int_value(long long v) {
  IRInterpValue value;
  value.i = v;
  value.f = 0;
  value.is_float = 0;
  value.undefined = 0;
  return value;
}

static IRInterpValue ii_float_value(double v) {
  IRInterpValue value;
  value.i = 0;
  value.f = v;
  value.is_float = 1;
  value.undefined = 0;
  return value;
}

/* Uninitialized locals/temps read this instead of 0. Native code gives them
 * stack or register garbage, so a zero-defaulting interpreter would blind the
 * differential to a deleted initializing store (`@neg <- 0` in print_int was
 * exactly that). Deterministic, identical in both machines - only a transform
 * that changes WHETHER a read sees its initialization can diverge on it. */
static IRInterpValue ii_poison_value(void) {
  IRInterpValue value;
  value.i = (long long)0xA5A5A5A5A5A5A5A5ULL;
  value.f = 0;
  value.is_float = 0;
  value.undefined = 1;
  return value;
}

/* Equality for assert_eq: exact for ints; floats compare as doubles (a test
 * author asserting float equality means bit-for-bit intent). */
static int ii_value_matches(const IRInterpValue *a, const IRInterpValue *b) {
  if (a->is_float || b->is_float) {
    double x = a->is_float ? a->f : (double)a->i;
    double y = b->is_float ? b->f : (double)b->i;
    return x == y;
  }
  return a->i == b->i;
}

static int ii_exec_function(IRInterpMachine *machine, IRFunction *fn,
                            const IRInterpValue *args, size_t arg_count,
                            IRInterpValue *result);

/* Read a variable, honoring slot-backed locals. */
static int ii_var_read(IRInterpMachine *machine, IIVar *var,
                       IRInterpValue *out) {
  if (!var->slotted) {
    *out = var->value;
    return 1;
  }
  unsigned long long raw = 0;
  if (!ii_mem_read(machine, (unsigned long long)var->value.i, var->slot_size,
                   &raw)) {
    return 0;
  }
  if (var->slot_is_float) {
    if (var->slot_size == 4) {
      float f;
      unsigned int bits = (unsigned int)raw;
      memcpy(&f, &bits, 4);
      *out = ii_float_value((double)f);
    } else if (var->slot_size == 2 && var->slot_alias == IR_ALIAS_CLASS_BF16) {
      uint16_t h = (uint16_t)raw;
      *out = ii_float_value((double)mettle_bf16bits_to_f32(h));
    } else if (var->slot_size == 2) {
      uint16_t h = (uint16_t)raw;
      *out = ii_float_value((double)mettle_f16bits_to_f32(h));
    } else {
      double d;
      memcpy(&d, &raw, 8);
      *out = ii_float_value(d);
    }
    return 1;
  }
  long long v = (long long)raw;
  if (!var->slot_is_unsigned && var->slot_size < 8) {
    int shift = 64 - var->slot_size * 8;
    v = (v << shift) >> shift;
  }
  *out = ii_int_value(v);
  return 1;
}

/* An integer just read from memory, widened the way its element type reads:
 * signed sign-extends, unsigned zero-extends. Byte and half loads were pinned
 * to zero-extension while the backends widened them with movzx whatever the
 * type said. */
static long long ii_widen_loaded_int(unsigned long long raw, int size,
                                     int is_unsigned) {
  if (is_unsigned) {
    return (long long)raw;
  }
  switch (size) {
  case 1: return (long long)(signed char)raw;
  case 2: return (long long)(short)raw;
  case 4: return (long long)(int)(unsigned int)raw;
  default: return (long long)raw;
  }
}

/* Narrow an integer to a declared width, the way a store to a narrow home
   does. Signedness decides which extension refills the high bits. */
static long long ii_narrow_int(long long v, int size, int is_unsigned) {
  switch (size) {
  case 1: return is_unsigned ? (long long)(unsigned char)v : (long long)(signed char)v;
  case 2: return is_unsigned ? (long long)(unsigned short)v : (long long)(short)v;
  case 4: return is_unsigned ? (long long)(unsigned int)v : (long long)(int)v;
  default: return v;
  }
}

/* An aggregate that fits in a register travels as its BYTES, not as an
 * address: `s = arr[i]` on a two-int32 struct lowers to an 8-byte load feeding
 * an aggregate assign, and a by-value argument of one is passed the same way.
 * Fills `out` and answers 1 when the value is content rather than an address,
 * which is decided by asking whether it addresses a live buffer at all. */
static int ii_aggregate_value_is_bytes(IRInterpMachine *machine,
                                       const IRInterpValue *value,
                                       long long size, unsigned char *out) {
  if (size <= 0 || size > 8 || value->is_float) {
    return 0;
  }
  long long off = 0;
  if (ii_addr_to_buffer(machine, (unsigned long long)ii_as_int(value), size,
                        &off)) {
    return 0; /* it really is an address */
  }
  unsigned long long raw = (unsigned long long)value->i;
  for (long long i = 0; i < size; i++) {
    out[i] = (unsigned char)((raw >> (i * 8)) & 0xFFu);
  }
  return 1;
}

static int ii_var_write(IRInterpMachine *machine, IIVar *var,
                        const IRInterpValue *value) {
  if (var->string_record && !value->is_float) {
    unsigned long long src = (unsigned long long)ii_as_int(value);
    long long src_off = 0;
    IIBuffer *sbuf = src ? ii_addr_to_buffer(machine, src, 16, &src_off) : NULL;
    long long dst_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, var->string_record, 16,
                                       &dst_off);
    if (sbuf && dbuf) {
      if (sbuf != dbuf || src_off != dst_off) {
        memmove(dbuf->data + dst_off, sbuf->data + src_off, 16);
      }
      var->value = ii_int_value((long long)var->string_record);
      return 1;
    }
  }
  if (var->agg_size > 0) {
    /* Aggregate: assignment through the name is a block copy from the source
     * address (a returned aggregate, another aggregate's storage, or a folded
     * constant image). */
    unsigned long long src = (unsigned long long)ii_as_int(value);
    unsigned long long dst = (unsigned long long)var->value.i;
    long long src_off = 0, dst_off = 0;
    IIBuffer *sbuf = ii_addr_to_buffer(machine, src, var->agg_size, &src_off);
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, var->agg_size, &dst_off);
    if (!sbuf && dbuf) {
      unsigned char bytes[8];
      if (ii_aggregate_value_is_bytes(machine, value, var->agg_size, bytes)) {
        memcpy(dbuf->data + dst_off, bytes, (size_t)var->agg_size);
        return 1;
      }
    }
    if (!sbuf || !dbuf) {
      ii_fail(machine, IR_INTERP_TRAP,
              "aggregate copy out of bounds / after free");
      return 0;
    }
    if (sbuf != dbuf || src_off != dst_off) {
      memmove(dbuf->data + dst_off, sbuf->data + src_off,
              (size_t)var->agg_size);
    }
    if (sbuf->escaped_local && sbuf != dbuf) {
      /* The consumed aggregate return: its frame is gone and its bytes are
       * copied out, so the slot goes back to the pool. */
      ii_reclaim_buffer(machine, (size_t)((sbuf->base - II_ADDR_BASE) /
                                          II_ADDR_STRIDE));
    }
    return 1;
  }
  if (!var->slotted) {
    var->value = *value;
    if (!var->value.is_float && var->value_size > 0 && var->value_size < 8) {
      var->value.i =
          ii_narrow_int(var->value.i, var->value_size, var->value_is_unsigned);
    }
    /* value_size -4 marks a float32 home: round writes to single precision. */
    if (var->value.is_float && var->value_size == -4) {
      var->value.f = (double)(float)var->value.f;
    }
    return 1;
  }
  unsigned long long raw = 0;
  if (var->slot_is_float) {
    if (var->slot_size == 4) {
      float f = (float)ii_as_float(value);
      unsigned int bits;
      memcpy(&bits, &f, 4);
      raw = bits;
    } else if (var->slot_size == 2 && var->slot_alias == IR_ALIAS_CLASS_BF16) {
      float f = (float)ii_as_float(value);
      uint32_t b;
      memcpy(&b, &f, (size_t)4);
      raw = (unsigned long long)mettle_f32bits_to_bf16bits(b);
    } else if (var->slot_size == 2) {
      float f = (float)ii_as_float(value);
      uint32_t b;
      memcpy(&b, &f, (size_t)4);
      raw = (unsigned long long)mettle_f32bits_to_f16bits(b);
    } else {
      double d = ii_as_float(value);
      memcpy(&raw, &d, 8);
    }
  } else {
    raw = (unsigned long long)ii_as_int(value);
  }
  return ii_mem_write(machine, (unsigned long long)var->value.i,
                      var->slot_size, raw);
}

/* Scalar layout of an MtlcType, enums and pointers included. Returns 0 for
 * aggregates and unknowns. */
static int ii_scalar_from_mtlc(const MtlcType *type, int *size, int *is_float,
                               int *is_unsigned) {
  if (!type) {
    return 0;
  }
  *is_float = 0;
  *is_unsigned = 0;
  switch (type->kind) {
  case MTLC_TYPE_INT8: *size = 1; return 1;
  case MTLC_TYPE_INT16: *size = 2; return 1;
  case MTLC_TYPE_INT32: *size = 4; return 1;
  case MTLC_TYPE_INT64: *size = 8; return 1;
  case MTLC_TYPE_UINT8: *size = 1; *is_unsigned = 1; return 1;
  case MTLC_TYPE_UINT16: *size = 2; *is_unsigned = 1; return 1;
  case MTLC_TYPE_UINT32: *size = 4; *is_unsigned = 1; return 1;
  case MTLC_TYPE_UINT64: *size = 8; *is_unsigned = 1; return 1;
  case MTLC_TYPE_BOOL: *size = 1; *is_unsigned = 1; return 1;
  case MTLC_TYPE_FLOAT32: *size = 4; *is_float = 1; return 1;
  case MTLC_TYPE_FLOAT64: *size = 8; *is_float = 1; return 1;
  case MTLC_TYPE_FLOAT16: *size = 2; *is_float = 1; return 1;
  case MTLC_TYPE_BFLOAT16: *size = 2; *is_float = 1; return 1;
  case MTLC_TYPE_ENUM:
    *size = (type->size == 1 || type->size == 2 || type->size == 8)
                ? (int)type->size
                : 4;
    return 1;
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_FUNCTION_POINTER:
    *size = 8;
    *is_unsigned = 1;
    return 1;
  default:
    return 0;
  }
}

static const IRModuleSymbol *ii_symbol(IRInterpMachine *machine,
                                       const char *name) {
  return machine->program ? ir_program_lookup_symbol(machine->program, name)
                          : NULL;
}

static unsigned long long ii_global_storage(IRInterpMachine *machine,
                                            const char *name);

/* First touch of a global by name: apply its initializer and record its
 * declared width so narrow globals wrap the way their .data image does.
 * Without this every global read 0 regardless of its initializer.
 *
 * Returns a fresh pointer into the globals table: recursion through another
 * global's storage can grow the table, so a pointer held across this call is
 * invalid by contract. */
static IIVar *ii_global_touch(IRInterpMachine *machine, const char *name) {
  IIVar *var = ii_env_upsert(&machine->globals, name);
  if (!var || var->global_inited) {
    return var;
  }
  var->global_inited = 1;
  const IRModuleSymbol *sym = ii_symbol(machine, name);
  if (!sym) {
    return var;
  }
  if (sym->kind == IR_MODSYM_CONSTANT) {
    var->value = ii_int_value(sym->const_value);
    return var;
  }
  if (sym->kind != IR_MODSYM_VARIABLE) {
    return var;
  }
  int size = 0, is_float = 0, is_unsigned = 0;
  if (sym->type && sym->type->kind == MTLC_TYPE_STRING) {
    /* String global: the home holds the record address. */
    if (sym->init_string) {
      unsigned long long chars = 0, record = 0;
      if (ii_string_literal(machine, sym->init_string,
                            sym->init_string ? sym->init_string_length : 0,
                            &chars, &record)) {
        var->value = ii_int_value((long long)record);
      }
    }
    var->value_size = 8;
    var->value_is_unsigned = 1;
    return var;
  }
  if (sym->type && (sym->type->kind == MTLC_TYPE_STRUCT ||
                    sym->type->kind == MTLC_TYPE_ARRAY ||
                    sym->type->kind == MTLC_TYPE_TAGGED_ENUM)) {
    /* Aggregate global touched by name: place its storage now so the value
     * is its address and assignment is a block copy. */
    ii_global_storage(machine, name);
    return ii_env_upsert(&machine->globals, name);
  }
  if (ii_scalar_from_mtlc(sym->type, &size, &is_float, &is_unsigned)) {
    var->value_size = is_float ? 0 : size;
    var->value_is_unsigned = is_unsigned;
  }
  if (sym->init_symbol_ref) {
    unsigned long long token = ii_function_token(machine, sym->init_symbol_ref);
    if (!token) {
      token = ii_global_storage(machine, sym->init_symbol_ref);
    }
    var = ii_env_upsert(&machine->globals, name);
    if (var) {
      var->value = ii_int_value((long long)token);
    }
    return var;
  }
  if (sym->has_initializer) {
    if (sym->init_is_float) {
      double d = 0;
      memcpy(&d, &sym->init_bits, 8);
      if (sym->type && sym->type->kind == MTLC_TYPE_FLOAT32) {
        d = (double)(float)d;
      }
      if (sym->type && sym->type->kind == MTLC_TYPE_FLOAT16) {
        float f = (float)d;
        uint32_t b;
        memcpy(&b, &f, (size_t)4);
        d = (double)mettle_f16bits_to_f32(mettle_f32bits_to_f16bits(b));
      }
      if (sym->type && sym->type->kind == MTLC_TYPE_BFLOAT16) {
        float f = (float)d;
        uint32_t b;
        memcpy(&b, &f, (size_t)4);
        d = (double)mettle_bf16bits_to_f32(mettle_f32bits_to_bf16bits(b));
      }
      var->value = ii_float_value(d);
    } else if (sym->init_string) {
      unsigned long long chars = 0, record = 0;
      if (ii_string_literal(machine, sym->init_string,
                            sym->init_string ? sym->init_string_length : 0,
                            &chars, &record)) {
        var->value = ii_int_value((long long)chars);
      }
    } else {
      var->value = ii_int_value(sym->init_bits);
      if (var->value_size > 0 && var->value_size < 8) {
        var->value.i = ii_narrow_int(var->value.i, var->value_size,
                                     var->value_is_unsigned);
      }
    }
  }
  return var;
}

/* Materialize a global's storage: its initializer image with relocation holes
 * filled (string literals, other globals, function addresses), the folded
 * scalar value, or bss zeros. Scalar globals become slot-backed so name
 * access and pointer access alias the same bytes. Returns the base address,
 * 0 when the symbol has no storage to give. */
static unsigned long long ii_global_storage(IRInterpMachine *machine,
                                            const char *name) {
  IIVar *var = ii_env_upsert(&machine->globals, name);
  if (!var) {
    return 0;
  }
  if (var->has_local_storage) {
    return (unsigned long long)var->value.i;
  }
  const IRModuleSymbol *sym = ii_symbol(machine, name);
  if (!sym) {
    return 0;
  }
  if (sym->type && sym->type->kind == MTLC_TYPE_STRING) {
    /* The string convention: address-of yields the record the value points
     * at, so touch the value into existence and hand that back. */
    var = ii_global_touch(machine, name);
    return var ? (unsigned long long)var->value.i : 0;
  }
  long long size = 0;
  if (sym->init_bytes && sym->init_bytes_size > 0) {
    size = (long long)sym->init_bytes_size;
  } else if (sym->type && sym->type->size > 0) {
    size = (long long)sym->type->size;
  } else {
    size = 8;
  }
  unsigned long long addr =
      ii_add_buffer_ex(machine, sym->init_bytes, size, 0);
  if (!addr) {
    return 0;
  }
  int scalar_size = 0, scalar_float = 0, scalar_unsigned = 0;
  int is_scalar = ii_scalar_from_mtlc(sym->type, &scalar_size, &scalar_float,
                                      &scalar_unsigned) &&
                  sym->kind == IR_MODSYM_VARIABLE;
  if (is_scalar) {
    /* The value the global held before its address was taken (its
     * initializer, or whatever name-writes left) becomes the home's bytes. */
    IRInterpValue seed;
    int seeded = 0;
    if (var->global_inited) {
      seed = var->value;
      seeded = 1;
    }
    var = ii_global_touch(machine, name);
    if (!var) {
      return 0;
    }
    if (!seeded) {
      seed = var->value;
    }
    var->slotted = 1;
    var->slot_size = scalar_size;
    var->slot_is_float = scalar_float;
    var->slot_is_unsigned = scalar_unsigned;
    var->slot_alias = 0;
    if (sym->type && sym->type->kind == MTLC_TYPE_BFLOAT16) {
      var->slot_alias = IR_ALIAS_CLASS_BF16;
    } else if (sym->type && sym->type->kind == MTLC_TYPE_FLOAT16) {
      var->slot_alias = IR_ALIAS_CLASS_F16;
    }
    var->has_local_storage = 1;
    var->value = ii_int_value((long long)addr);
    if (!sym->init_bytes) {
      ii_var_write(machine, var, &seed);
    }
  } else {
    var->global_inited = 1;
    var->has_local_storage = 1;
    var->value = ii_int_value((long long)addr);
    var->agg_size = (sym->type && (sym->type->kind == MTLC_TYPE_STRUCT ||
                                   sym->type->kind == MTLC_TYPE_TAGGED_ENUM))
                        ? size
                        : 0;
  }
  /* Storage is marked placed above, so cyclic references resolve to this
   * address instead of recursing forever. `var` is invalid past this point:
   * recursive placements grow the table. */
  for (size_t r = 0; r < sym->init_reloc_count; r++) {
    const IRInitReloc *reloc = &sym->init_relocs[r];
    unsigned long long value = 0;
    if (reloc->string) {
      unsigned long long chars = 0, record = 0;
      if (!ii_string_literal(machine, reloc->string,
                             reloc->string ? reloc->string_length : 0, &chars,
                             &record)) {
        return 0;
      }
      value = chars;
      if (reloc->string_wants_record) {
        unsigned long long length = (unsigned long long)reloc->string_length;
        long long len_off = 0;
        IIBuffer *len_buf = ii_addr_to_buffer(
            machine, addr + reloc->offset + 8, 8, &len_off);
        if (!len_buf) {
          return 0;
        }
        memcpy(len_buf->data + len_off, &length, 8);
      }
    } else if (reloc->symbol) {
      value = ii_function_token(machine, reloc->symbol);
      if (!value) {
        value = ii_global_storage(machine, reloc->symbol);
      }
      if (!value) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED,
                "aggregate initializer references unplaceable symbol");
        return 0;
      }
    }
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, addr + reloc->offset, 8,
                                      &offset);
    if (!buf) {
      return 0;
    }
    memcpy(buf->data + offset, &value, 8);
  }
  return addr;
}

/* Resolve a TEMP/SYMBOL operand name to its variable: frame first, then
 * globals (creating there on first touch, matching "undeclared symbol is a
 * global" lowering). */
static IIVar *ii_resolve(IRInterpMachine *machine, IIFrame *frame,
                         const IROperand *operand) {
  IIVar *var = ii_env_find(&frame->env, operand->name);
  if (var) {
    return var;
  }
  if (operand->kind == IR_OPERAND_TEMP) {
    /* Temps are function-local by construction. A brand-new temp slot means a
     * read before any def (possible only after a transform deleted the def):
     * it reads POISON, not 0, because native code would see a stale register.
     * A deterministic-but-nonzero value keeps the differential faithful and
     * both machines identical. Globals stay zero (.bss is zeroed for real). */
    IIVar *fresh = ii_env_upsert(&frame->env, operand->name);
    if (fresh) {
      fresh->value = ii_poison_value();
    }
    return fresh;
  }
  return ii_global_touch(machine, operand->name);
}

static int ii_fetch(IRInterpMachine *machine, IIFrame *frame,
                    const IROperand *operand, IRInterpValue *out) {
  switch (operand->kind) {
  case IR_OPERAND_NONE:
    *out = ii_int_value(0);
    return 1;
  case IR_OPERAND_INT:
    *out = ii_int_value(operand->int_value);
    return 1;
  case IR_OPERAND_FLOAT:
    *out = ii_float_value(operand->float_value);
    return 1;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    if (!operand->name) {
      *out = ii_int_value(0);
      return 1;
    }
    IIVar *var = ii_resolve(machine, frame, operand);
    if (!var) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    return ii_var_read(machine, var, out);
  }
  case IR_OPERAND_STRING: {
    /* A string literal used as a value IS the address of its fat record, the
     * same thing the backend's operand load produces. */
    unsigned long long chars = 0, record = 0;
    if (!ii_string_literal(machine, operand->name,
                           ir_operand_string_length(operand), &chars,
                           &record)) {
      return 0;
    }
    *out = ii_int_value((long long)record);
    return 1;
  }
  default:
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "label operand as value");
    return 0;
  }
}

static int ii_store_dest(IRInterpMachine *machine, IIFrame *frame,
                         const IROperand *dest, const IRInterpValue *value) {
  if (dest->kind == IR_OPERAND_NONE) {
    return 1;
  }
  if ((dest->kind != IR_OPERAND_TEMP && dest->kind != IR_OPERAND_SYMBOL) ||
      !dest->name) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "unwritable destination operand");
    return 0;
  }
  IIVar *var = ii_resolve(machine, frame, dest);
  if (!var) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    return 0;
  }
  return ii_var_write(machine, var, value);
}

/* Fetch as address (integer). */
static int ii_fetch_addr(IRInterpMachine *machine, IIFrame *frame,
                         const IROperand *operand, unsigned long long *out) {
  IRInterpValue value;
  if (!ii_fetch(machine, frame, operand, &value)) {
    return 0;
  }
  *out = (unsigned long long)ii_as_int(&value);
  return 1;
}

static int ii_fetch_int(IRInterpMachine *machine, IIFrame *frame,
                        const IROperand *operand, long long *out) {
  IRInterpValue value;
  if (!ii_fetch(machine, frame, operand, &value)) {
    return 0;
  }
  *out = ii_as_int(&value);
  return 1;
}

/* ---------------- scalar ops ---------------- */

static int ii_binary(IRInterpMachine *machine, const IRInstruction *insn,
                     const IRInterpValue *a, const IRInterpValue *b,
                     IRInterpValue *out) {
  const char *op = insn->text ? insn->text : "?";

  /* String '+' concatenates contents, matching the backend's concat kernel.
   * Without this the generic path summed the two record addresses. */
  if (insn->value_type && insn->value_type->kind == MTLC_TYPE_STRING &&
      strcmp(op, "+") == 0) {
    const unsigned char *left_bytes = NULL, *right_bytes = NULL;
    size_t left_length = 0, right_length = 0;
    if (!ii_read_string(machine, (unsigned long long)ii_as_int(a),
                        &left_bytes, &left_length) ||
        !ii_read_string(machine, (unsigned long long)ii_as_int(b),
                        &right_bytes, &right_length) ||
        left_length > (size_t)II_MAX_BUFFER_SIZE - right_length) {
      ii_fail(machine, IR_INTERP_TRAP, "string concat operand");
      return 0;
    }
    char *joined = malloc(left_length + right_length + 1);
    if (!joined) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    memcpy(joined, left_bytes, left_length);
    memcpy(joined + left_length, right_bytes, right_length);
    unsigned long long record =
        ii_make_string(machine, joined, left_length + right_length);
    free(joined);
    if (!record) {
      ii_fail(machine, IR_INTERP_TRAP, "string concat storage");
      return 0;
    }
    *out = ii_int_value((long long)record);
    return 1;
  }

  if (insn->is_float) {
    double x = ii_as_float(a);
    double y = ii_as_float(b);
    int narrow = insn->float_bits == 32;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "/") == 0) {
      double r;
      if (narrow) {
        float fx = (float)x, fy = (float)y;
        float fr = op[0] == '+' ? fx + fy
                   : op[0] == '-' ? fx - fy
                   : op[0] == '*' ? fx * fy
                                  : fx / fy;
        r = (double)fr;
      } else {
        r = op[0] == '+' ? x + y : op[0] == '-' ? x - y : op[0] == '*' ? x * y : x / y;
      }
      *out = ii_float_value(r);
      return 1;
    }
    if (strcmp(op, "==") == 0) { *out = ii_int_value(x == y); return 1; }
    if (strcmp(op, "!=") == 0) { *out = ii_int_value(x != y); return 1; }
    if (strcmp(op, "<") == 0)  { *out = ii_int_value(x < y);  return 1; }
    if (strcmp(op, "<=") == 0) { *out = ii_int_value(x <= y); return 1; }
    if (strcmp(op, ">") == 0)  { *out = ii_int_value(x > y);  return 1; }
    if (strcmp(op, ">=") == 0) { *out = ii_int_value(x >= y); return 1; }
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "float binary op");
    return 0;
  }

  long long sx = ii_as_int(a);
  long long sy = ii_as_int(b);
  unsigned long long ux = (unsigned long long)sx;
  unsigned long long uy = (unsigned long long)sy;
  int is_unsigned = insn->is_unsigned;

  if (strcmp(op, "+") == 0) { *out = ii_int_value((long long)(ux + uy)); return 1; }
  if (strcmp(op, "-") == 0) { *out = ii_int_value((long long)(ux - uy)); return 1; }
  if (strcmp(op, "*") == 0) { *out = ii_int_value((long long)(ux * uy)); return 1; }
  if (strcmp(op, "&") == 0) { *out = ii_int_value((long long)(ux & uy)); return 1; }
  if (strcmp(op, "|") == 0) { *out = ii_int_value((long long)(ux | uy)); return 1; }
  if (strcmp(op, "^") == 0) { *out = ii_int_value((long long)(ux ^ uy)); return 1; }
  if (strcmp(op, "<<") == 0) {
    *out = ii_int_value((long long)(ux << (uy & 63)));
    return 1;
  }
  if (strcmp(op, ">>") == 0) {
    if (is_unsigned) {
      *out = ii_int_value((long long)(ux >> (uy & 63)));
    } else {
      *out = ii_int_value(sx >> (uy & 63));
    }
    return 1;
  }
  if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    if (sy == 0) {
      ii_fail(machine, IR_INTERP_TRAP, "integer divide trap");
      return 0;
    }
    /* Dividing by -1 is a negation with no remainder, and the negation wraps
     * INT64_MIN to itself. Spelled out because the host would call it
     * undefined, and because it is what the compiled code now does. */
    if (!is_unsigned && sy == -1) {
      *out = ii_int_value(op[0] == '/'
                              ? (long long)(0ULL - (unsigned long long)sx)
                              : 0);
      return 1;
    }
    long long q;
    if (is_unsigned) {
      q = op[0] == '/' ? (long long)(ux / uy) : (long long)(ux % uy);
    } else {
      q = op[0] == '/' ? sx / sy : sx % sy;
    }
    *out = ii_int_value(q);
    return 1;
  }
  if (strcmp(op, "==") == 0) { *out = ii_int_value(sx == sy); return 1; }
  if (strcmp(op, "!=") == 0) { *out = ii_int_value(sx != sy); return 1; }
  if (strcmp(op, "<") == 0) {
    *out = ii_int_value(is_unsigned ? ux < uy : sx < sy);
    return 1;
  }
  if (strcmp(op, "<=") == 0) {
    *out = ii_int_value(is_unsigned ? ux <= uy : sx <= sy);
    return 1;
  }
  if (strcmp(op, ">") == 0) {
    *out = ii_int_value(is_unsigned ? ux > uy : sx > sy);
    return 1;
  }
  if (strcmp(op, ">=") == 0) {
    *out = ii_int_value(is_unsigned ? ux >= uy : sx >= sy);
    return 1;
  }
  if (strcmp(op, "&&") == 0) { *out = ii_int_value(sx != 0 && sy != 0); return 1; }
  if (strcmp(op, "||") == 0) { *out = ii_int_value(sx != 0 || sy != 0); return 1; }
  ii_fail(machine, IR_INTERP_UNSUPPORTED, "binary op");
  return 0;
}

static int ii_integer_cast_width(const char *type, int *size_out,
                                 int *unsigned_out) {
  static const struct {
    const char *name;
    int size;
    int is_unsigned;
  } widths[] = {{"int8", 1, 0},   {"int16", 2, 0},  {"int32", 4, 0},
                {"int64", 8, 0},  {"uint8", 1, 1},  {"uint16", 2, 1},
                {"uint32", 4, 1}, {"uint64", 8, 1}};
  size_t i = 0u;

  for (i = 0u; i < sizeof(widths) / sizeof(widths[0]); i++) {
    if (strcmp(type, widths[i].name) == 0) {
      *size_out = widths[i].size;
      *unsigned_out = widths[i].is_unsigned;
      return 1;
    }
  }
  return 0;
}

static int ii_cast(IRInterpMachine *machine, const IRInstruction *insn,
                   const IRInterpValue *in, IRInterpValue *out) {
  const char *type = insn->text ? insn->text : "";
  size_t len = strlen(type);
  if (len > 0 && type[len - 1] == '*') {
    /* Pointer cast: value-preserving. */
    *out = ii_int_value(ii_as_int(in));
    return 1;
  }
  if (strcmp(type, "cstring") == 0 || strcmp(type, "rawptr") == 0 ||
      strcmp(type, "string") == 0 || strncmp(type, "fn(", 3) == 0 ||
      strncmp(type, "Fn(", 3) == 0) {
    /* Pointer-shaped targets: value-preserving. */
    *out = ii_int_value(ii_as_int(in));
    return 1;
  }
  /* is_unsigned on a CAST says the SOURCE is an unsigned integer, so a value
   * with bit 63 set is a large positive number and not a negative one. */
  if (strcmp(type, "float64") == 0 || strcmp(type, "float32") == 0) {
    double d;
    if (!in->is_float && insn->is_unsigned) {
      d = (double)(unsigned long long)in->i;
    } else {
      d = ii_as_float(in);
    }
    *out = ii_float_value(strcmp(type, "float32") == 0 ? (double)(float)d : d);
    return 1;
  }
  if (strcmp(type, "float16") == 0 || strcmp(type, "bfloat16") == 0) {
    double d;
    float f;
    uint32_t b;
    uint16_t h;
    if (!in->is_float && insn->is_unsigned) {
      d = (double)(unsigned long long)in->i;
    } else {
      d = ii_as_float(in);
    }
    f = (float)d;
    memcpy(&b, &f, (size_t)4);
    if (strcmp(type, "float16") == 0) {
      h = mettle_f32bits_to_f16bits(b);
      *out = ii_float_value((double)mettle_f16bits_to_f32(h));
    } else {
      h = mettle_f32bits_to_bf16bits(b);
      *out = ii_float_value((double)mettle_bf16bits_to_f32(h));
    }
    return 1;
  }
  int size = 0, target_unsigned = 0;
  if (!ii_integer_cast_width(type, &size, &target_unsigned)) {
    if (strcmp(type, "bool") == 0) {
      *out = ii_int_value(in->is_float ? in->f != 0.0 : in->i != 0);
      return 1;
    }
    if (insn->value_type && insn->value_type->kind == MTLC_TYPE_ENUM) {
      size = (insn->value_type->size == 1 || insn->value_type->size == 2 ||
              insn->value_type->size == 8)
                 ? (int)insn->value_type->size
                 : 4;
    } else if (insn->value_type &&
               (insn->value_type->kind == MTLC_TYPE_POINTER ||
                insn->value_type->kind == MTLC_TYPE_FUNCTION_POINTER ||
                insn->value_type->kind == MTLC_TYPE_STRING)) {
      *out = ii_int_value(ii_as_int(in));
      return 1;
    } else {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "cast target type");
      return 0;
    }
  }

  long long v;
  if (in->is_float) {
    /* Float -> int: truncate toward zero; x86 cvtt sentinel on overflow/NaN.
     * The machine always truncates at 64 bits and narrows afterwards, so only
     * the 64-bit range has a sentinel. Checking a 4-byte target against the
     * int32 range instead answered 0x80000000 for `(uint32)4e9`, where the
     * hardware gives 4000000000. */
    double d = in->f;
    if (size == 8 && target_unsigned) {
      /* A uint64 target reaches 2^64, and the backend biases the value down
       * rather than letting the signed truncation answer its sentinel. */
      if (!(d >= 0.0 && d < 18446744073709551616.0)) {
        v = LLONG_MIN;
      } else {
        v = (long long)(unsigned long long)d;
      }
    } else if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
      v = LLONG_MIN;
    } else {
      v = (long long)d;
    }
  } else {
    v = in->i;
  }
  if (size < 8) {
    int shift = 64 - size * 8;
    if (target_unsigned) {
      v = (long long)(((unsigned long long)v << shift) >> shift);
    } else {
      v = (v << shift) >> shift;
    }
  }
  *out = ii_int_value(v);
  return 1;
}

/* ---------------- extern model ---------------- */

static double ii_sqrt(double value) {
  if (value <= 0.0) {
    return value == 0.0 ? value : 0.0 / 0.0;
  }
  if (value != value || value > 1.7e308) {
    return value;
  }
  double estimate = value >= 1.0 ? value : 1.0;
  for (int i = 0; i < 64; i++) {
    double next = 0.5 * (estimate + value / estimate);
    if (next == estimate) {
      break;
    }
    estimate = next;
  }
  return estimate;
}

static double ii_trunc(double value) {
  if (value != value || value >= 9007199254740992.0 ||
      value <= -9007199254740992.0) {
    return value;
  }
  return (double)(long long)value;
}

static double ii_floor(double value) {
  double t = ii_trunc(value);
  return (t > value) ? t - 1.0 : t;
}

static double ii_ceil(double value) {
  double t = ii_trunc(value);
  return (t < value) ? t + 1.0 : t;
}

static double ii_round(double value) {
  return value < 0.0 ? -ii_floor(-value + 0.5) : ii_floor(value + 0.5);
}

static int ii_extern_math(const char *name, const IRInterpValue *args,
                          size_t arg_count, IRInterpValue *out) {
  double x = 0.0;
  double y = 0.0;
  if (!name || arg_count < 1) {
    return 0;
  }
  x = args[0].is_float ? args[0].f : (double)args[0].i;
  if (arg_count > 1) {
    y = args[1].is_float ? args[1].f : (double)args[1].i;
  }
  if (arg_count == 1) {
    static const struct {
      const char *name;
      int single;
      double (*fn)(double);
    } unary[] = {
        {"sqrt", 0, ii_sqrt},   {"floor", 0, ii_floor},  {"ceil", 0, ii_ceil},
        {"trunc", 0, ii_trunc}, {"round", 0, ii_round},  {"floorf", 1, ii_floor},
        {"ceilf", 1, ii_ceil},  {"truncf", 1, ii_trunc}, {"roundf", 1, ii_round},
        {"log", 0, mt_log},     {"sin", 0, mt_sin},      {"cos", 0, mt_cos},
    };
    for (size_t i = 0; i < sizeof(unary) / sizeof(unary[0]); i++) {
      if (strcmp(name, unary[i].name) == 0) {
        double r = unary[i].single ? unary[i].fn((double)(float)x)
                                   : unary[i].fn(x);
        *out = ii_float_value(unary[i].single ? (double)(float)r : r);
        return 1;
      }
    }
    if (strcmp(name, "fabsf") == 0) {
      float f = (float)x;
      *out = ii_float_value((double)(f < 0.0f ? -f : f));
      return 1;
    }
    if (strcmp(name, "sqrtf") == 0) {
      *out = ii_float_value((double)(float)ii_sqrt((double)(float)x));
      return 1;
    }
    if (strcmp(name, "expf") == 0) {
      *out = ii_float_value((double)(float)mt_exp((double)(float)x));
      return 1;
    }
    if (strcmp(name, "logf") == 0) {
      *out = ii_float_value((double)(float)mt_log((double)(float)x));
      return 1;
    }
    if (strcmp(name, "sinf") == 0) {
      *out = ii_float_value((double)(float)mt_sin((double)(float)x));
      return 1;
    }
    if (strcmp(name, "cosf") == 0) {
      *out = ii_float_value((double)(float)mt_cos((double)(float)x));
      return 1;
    }
    if (strcmp(name, "tanhf") == 0) {
      *out = ii_float_value((double)(float)mt_tanh((double)(float)x));
      return 1;
    }
    if (strcmp(name, "fabs") == 0) {
      *out = ii_float_value(x < 0.0 ? -x : x);
      return 1;
    }
    if (strcmp(name, "exp") == 0) {
      *out = ii_float_value(mt_exp(x));
      return 1;
    }
    if (strcmp(name, "tanh") == 0) {
      *out = ii_float_value(mt_tanh(x));
      return 1;
    }
  }
  if (arg_count == 2 && strcmp(name, "powf") == 0) {
    *out = ii_float_value((double)(float)mt_pow((double)(float)x,
                                                (double)(float)y));
    return 1;
  }
  if (arg_count == 2 && strcmp(name, "pow") == 0) {
    *out = ii_float_value(mt_pow(x, y));
    return 1;
  }
  if (arg_count == 2 && (strcmp(name, "fmin") == 0 || strcmp(name, "fmax") == 0 ||
                         strcmp(name, "fminf") == 0 ||
                         strcmp(name, "fmaxf") == 0)) {
    int single = name[strlen(name) - 1] == 'f';
    double a = single ? (double)(float)x : x;
    double b = single ? (double)(float)y : y;
    double r = 0.0;
    if (a != a) {
      r = b;
    } else if (b != b) {
      r = a;
    } else if (name[2] == 'i') {
      r = a < b ? a : b;
    } else {
      r = a > b ? a : b;
    }
    *out = ii_float_value(r);
    return 1;
  }
  return 0;
}



long long ir_interp_pointee_window(IRInterpMachine *machine,
                                   unsigned long long value,
                                   unsigned char *out, size_t capacity) {
  long long offset = 0;
  long long avail = 0;
  long long take = 0;
  IIBuffer *buf = NULL;
  if (!machine || !out || capacity == 0) {
    return -1;
  }
  buf = ii_addr_to_buffer(machine, value, 0, &offset);
  if (!buf) {
    return -1;
  }
  avail = buf->size - offset;
  if (avail <= 0) {
    return 0;
  }
  take = avail < (long long)capacity ? avail : (long long)capacity;
  memcpy(out, buf->data + offset, (size_t)take);
  for (long long w = 0; w + 8 <= take; w++) {
    unsigned long long word = 0;
    long long unused = 0;
    if (((offset + w) & 7) != 0) {
      continue;
    }
    memcpy(&word, out + w, 8);
    if (word < II_ADDR_BASE || !ii_addr_to_buffer(machine, word, 0, &unused)) {
      continue;
    }
    word = II_ADDR_BASE;
    memcpy(out + w, &word, 8);
  }
  return take;
}

static void ii_trace_extern(IRInterpMachine *machine, const char *name,
                            const IRInterpValue *args, size_t arg_count) {
  if (machine->trace_count >= II_TRACE_CAP) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "extern trace overflow");
    return;
  }
  if (machine->trace_count == machine->trace_capacity) {
    size_t grown = machine->trace_capacity ? machine->trace_capacity * 2 : 8;
    IRInterpExternCall *table = (IRInterpExternCall *)realloc(
        machine->trace, grown * sizeof(IRInterpExternCall));
    if (!table) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return;
    }
    machine->trace = table;
    machine->trace_capacity = grown;
  }
  IRInterpExternCall *call = &machine->trace[machine->trace_count++];
  call->modelled = 0;
  snprintf(call->name, sizeof(call->name), "%s", name);
  call->arg_count = arg_count > 8 ? 8 : arg_count;
  for (size_t i = 0; i < call->arg_count; i++) {
    call->args[i] = args[i];
    call->arg_mem_len[i] = 0;
    call->arg_is_pointer[i] = 0;
    if (args[i].is_float) {
      continue;
    }
    /* Pointer argument: capture the bytes it addresses right now, up to the
     * cap or the end of its buffer. The extern observes memory at the moment
     * of the call, so the trace must too. */
    long long take = ir_interp_pointee_window(
        machine, (unsigned long long)args[i].i, call->arg_mem[i],
        IR_INTERP_EXTERN_MEM_CAP);
    if (take < 0) {
      continue;
    }
    call->arg_is_pointer[i] = 1;
    call->arg_mem_len[i] = (unsigned short)take;
  }
}

static IRFunction *ii_find_function(IRInterpMachine *machine,
                                    const char *name) {
  if (machine->override_name && strcmp(machine->override_name, name) == 0) {
    return machine->override_fn;
  }
  if (!machine->program) {
    return NULL;
  }
  for (size_t i = 0; i < machine->program->function_count; i++) {
    IRFunction *fn = machine->program->functions[i];
    if (fn && fn->name && strcmp(fn->name, name) == 0) {
      return fn;
    }
  }
  return NULL;
}

/* Mirrors the backend's cstring test: a pointer named `cstring`, or a pointer
 * to bytes. Everything else takes a string literal as the fat record. */
static int ii_type_is_cstring(const MtlcType *type) {
  if (!type || type->kind != MTLC_TYPE_POINTER) {
    return 0;
  }
  if (type->name && strcmp(type->name, "cstring") == 0) {
    return 1;
  }
  return type->base_type && type->base_type->name &&
         strcmp(type->base_type->name, "uint8") == 0;
}

static int ii_callee_param_is_cstring(IRInterpMachine *machine,
                                      const char *callee, size_t index) {
  const IRModuleSymbol *sym =
      machine->program ? ir_program_lookup_symbol(machine->program, callee)
                       : NULL;
  if (!sym || !sym->param_types || index >= sym->param_count) {
    return 0;
  }
  return ii_type_is_cstring(sym->param_types[index]);
}

/* By-value aggregate arguments travel INDIRECT: the caller passes the address
 * of a copy, so callee writes never reach the caller's aggregate. The copies
 * live in the caller's frame like the copy slots the backend reserves. */
static int ii_copy_aggregate_args(IRInterpMachine *machine, IIFrame *frame,
                                  const char *callee, IRInterpValue *args,
                                  size_t arg_count) {
  const IRModuleSymbol *sym = ii_symbol(machine, callee);
  if (!sym || !sym->param_types) {
    return 1;
  }
  for (size_t i = 0; i < arg_count && i < sym->param_count; i++) {
    const MtlcType *pt = sym->param_types[i];
    if (!pt || pt->size == 0 ||
        (pt->kind != MTLC_TYPE_STRUCT && pt->kind != MTLC_TYPE_ARRAY &&
         pt->kind != MTLC_TYPE_TAGGED_ENUM)) {
      continue;
    }
    long long size = (long long)pt->size;
    unsigned long long src = (unsigned long long)ii_as_int(&args[i]);
    long long src_off = 0;
    IIBuffer *sbuf = ii_addr_to_buffer(machine, src, size, &src_off);
    unsigned char inline_bytes[8];
    int carried_in_value = 0;
    if (!sbuf) {
      carried_in_value =
          ii_aggregate_value_is_bytes(machine, &args[i], size, inline_bytes);
      if (!carried_in_value) {
        ii_fail(machine, IR_INTERP_TRAP,
                "aggregate argument out of bounds / after free");
        return 0;
      }
    }
    unsigned long long copy = ii_add_buffer_ex(machine, NULL, size, 1);
    if (!copy ||
        !ii_frame_own(frame,
                      (size_t)((copy - II_ADDR_BASE) / II_ADDR_STRIDE))) {
      ii_fail(machine, IR_INTERP_TRAP, "aggregate argument copy");
      return 0;
    }
    long long dst_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, copy, size, &dst_off);
    sbuf = carried_in_value ? NULL
                            : ii_addr_to_buffer(machine, src, size, &src_off);
    if (!dbuf || (!sbuf && !carried_in_value)) {
      ii_fail(machine, IR_INTERP_TRAP, "aggregate argument copy");
      return 0;
    }
    if (carried_in_value) {
      memcpy(dbuf->data + dst_off, inline_bytes, (size_t)size);
      ii_mark_bytes(dbuf, dst_off, size, 1);
    } else {
      memcpy(dbuf->data + dst_off, sbuf->data + src_off, (size_t)size);
      if (dbuf->init_map && sbuf->init_map) {
        memcpy(dbuf->init_map + dst_off, sbuf->init_map + src_off,
               (size_t)size);
      } else {
        ii_mark_bytes(dbuf, dst_off, size, 1);
      }
    }
    args[i] = ii_int_value((long long)copy);
  }
  return 1;
}

static long long ii_buffer_size_at(IRInterpMachine *machine,
                                   unsigned long long addr) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, 0, &offset);
  if (!buf || offset != 0) {
    return -1;
  }
  return buf->size;
}

/* Modeled externs return 1 and set *result; unknown externs are traced and
 * return 0 (meaning: use the pure default). Returns -1 on trap. */
static int ii_extern_call(IRInterpMachine *machine, const char *name,
                          const IRInterpValue *args, size_t arg_count,
                          IRInterpValue *result) {
  *result = ii_int_value(0);

  {
    const IRModuleSymbol *sym = ii_symbol(machine, name);
    if (sym && sym->link_name && sym->link_name[0]) {
      name = sym->link_name;
    }
  }

  if ((strcmp(name, "malloc") == 0 && arg_count == 1) ||
      (strcmp(name, "calloc") == 0 && arg_count == 2)) {
    long long size = ii_as_int(&args[0]);
    if (arg_count == 2) {
      size *= ii_as_int(&args[1]);
    }
    if (size < 0 || size > II_MAX_BUFFER_SIZE) {
      return -1;
    }
    unsigned long long addr = ir_interp_add_buffer(machine, NULL, size);
    if (!addr) {
      return -1;
    }
    if (strcmp(name, "malloc") == 0) {
      /* Deterministic "uninitialized" pattern. */
      long long offset = 0;
      IIBuffer *buf = ii_addr_to_buffer(machine, addr, 0, &offset);
      if (buf) {
        memset(buf->data, II_POISON_BYTE, (size_t)buf->size);
        ii_mark_bytes(buf, 0, buf->size, 0);
      }
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }
  if (arg_count >= 1 && strncmp(name, "mettle_atomic_", 14) == 0) {
    unsigned long long target = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long prior = 0;
    if (!target || !ii_mem_read(machine, target, 4, &prior)) {
      return -1;
    }
    {
      int before = (int)(unsigned int)prior;
      int next = before;
      if (strcmp(name, "mettle_atomic_compare_exchange_i32") == 0 &&
          arg_count >= 3) {
        if (before == (int)ii_as_int(&args[2])) {
          next = (int)ii_as_int(&args[1]);
        }
      } else if (strcmp(name, "mettle_atomic_exchange_i32") == 0 &&
                 arg_count >= 2) {
        next = (int)ii_as_int(&args[1]);
      } else if (strcmp(name, "mettle_atomic_inc_i32") == 0) {
        next = (int)((unsigned int)before + 1u);
      } else if (strcmp(name, "mettle_atomic_dec_i32") == 0) {
        next = (int)((unsigned int)before - 1u);
      } else {
        return 0;
      }
      if (next != before &&
          !ii_mem_write(machine, target, 4, (unsigned long long)(unsigned int)next)) {
        return -1;
      }
      *result = ii_int_value(
          (strcmp(name, "mettle_atomic_inc_i32") == 0 ||
           strcmp(name, "mettle_atomic_dec_i32") == 0)
              ? (long long)next
              : (long long)before);
      return 1;
    }
  }

  if (strcmp(name, "fwrite") == 0 || strcmp(name, "write") == 0 ||
      strcmp(name, "putchar") == 0 || strcmp(name, "fputc") == 0 ||
      strcmp(name, "putc") == 0 || strcmp(name, "puts") == 0 ||
      strcmp(name, "fputs") == 0 || strcmp(name, "fflush") == 0) {
    ii_trace_extern(machine, name, args, arg_count);
    if (machine->status != IR_INTERP_OK) {
      return -1;
    }
    machine->trace[machine->trace_count - 1].modelled = 1;
  }

  if (arg_count >= 4 && strcmp(name, "fwrite") == 0) {
    long long size = ii_as_int(&args[1]);
    long long count = ii_as_int(&args[2]);
    *result = ii_int_value(size > 0 && ii_as_int(&args[3]) ? count : 0);
    return 1;
  }

  if (arg_count >= 3 && strcmp(name, "write") == 0) {
    *result = ii_int_value(ii_as_int(&args[2]));
    return 1;
  }

  if (arg_count >= 1 && (strcmp(name, "putchar") == 0 ||
                         strcmp(name, "fputc") == 0 ||
                         strcmp(name, "putc") == 0)) {
    *result = ii_int_value(ii_as_int(&args[0]) & 0xFF);
    return 1;
  }

  if (arg_count >= 1 && (strcmp(name, "puts") == 0 ||
                         strcmp(name, "fputs") == 0)) {
    *result = ii_int_value(0);
    return 1;
  }

  if (strcmp(name, "fflush") == 0) {
    *result = ii_int_value(0);
    return 1;
  }

  if (arg_count >= 2 && strcmp(name, "mettle_swap_stage") == 0) {
    unsigned long long slot = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long replacement = (unsigned long long)ii_as_int(&args[1]);
    if (!slot) {
      *result = ii_int_value(0);
      return 1;
    }
    for (long long i = 0; i < machine->swap_pending_count; i++) {
      if (machine->swap_slots[i] == slot) {
        machine->swap_replacements[i] = replacement;
        *result = ii_int_value(1);
        return 1;
      }
    }
    if (machine->swap_pending_count >= 256) {
      *result = ii_int_value(0);
      return 1;
    }
    machine->swap_slots[machine->swap_pending_count] = slot;
    machine->swap_replacements[machine->swap_pending_count] = replacement;
    machine->swap_pending_count++;
    *result = ii_int_value(1);
    return 1;
  }

  if (strcmp(name, "mettle_swap_apply") == 0) {
    long long applied = machine->swap_pending_count;
    for (long long i = 0; i < applied; i++) {
      if (!ii_mem_write(machine, machine->swap_slots[i], 8,
                        machine->swap_replacements[i])) {
        return -1;
      }
    }
    machine->swap_pending_count = 0;
    *result = ii_int_value(applied);
    return 1;
  }

  if (strcmp(name, "mettle_swap_pending") == 0) {
    *result = ii_int_value(machine->swap_pending_count);
    return 1;
  }

  if (strcmp(name, "mettle_swap_discard") == 0) {
    machine->swap_pending_count = 0;
    *result = ii_int_value(0);
    return 1;
  }

  /* A machine with sockets and no peers, which is what the compile host is
     from the program's point of view. Creating and closing one succeeds;
     anything needing a peer fails and reports why. No operating system is
     involved: the handle is a counter and the failure is the only answer a
     socket that was never connected can give. */
  if (strcmp(name, "socket") == 0 || strcmp(name, "WSASocketA") == 0) {
    machine->next_thread_handle++;
    *result = ii_int_value((long long)(machine->next_thread_handle + 2));
    return 1;
  }

  if (strcmp(name, "closesocket") == 0 || strcmp(name, "shutdown") == 0 ||
      strcmp(name, "close") == 0 ||
      strcmp(name, "bind") == 0 || strcmp(name, "listen") == 0 ||
      strcmp(name, "setsockopt") == 0 || strcmp(name, "WSAStartup") == 0 ||
      strcmp(name, "WSACleanup") == 0) {
    *result = ii_int_value(0);
    return 1;
  }

  if (strcmp(name, "posix_get_errno") == 0 ||
      strcmp(name, "WSAGetLastError") == 0) {
    *result = ii_int_value(machine->last_socket_error);
    return 1;
  }

  if (arg_count >= 2 && (strcmp(name, "send") == 0 ||
                         strcmp(name, "sendto") == 0)) {
    machine->last_socket_error = 107; /* ENOTCONN */
    *result = ii_int_value(-1);
    return 1;
  }

  if (arg_count >= 2 && (strcmp(name, "recv") == 0 ||
                         strcmp(name, "recvfrom") == 0 ||
                         strcmp(name, "accept") == 0 ||
                         strcmp(name, "connect") == 0)) {
    machine->last_socket_error = 107; /* ENOTCONN */
    *result = ii_int_value(-1);
    return 1;
  }

  if (arg_count >= 2 && (strcmp(name, "posix_cas_i32") == 0 ||
                         strcmp(name, "posix_atomic_exchange_i32") == 0 ||
                         strcmp(name, "posix_atomic_add_i32") == 0)) {
    unsigned long long target = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long prior = 0;
    if (!target || !ii_mem_read(machine, target, 4, &prior)) {
      return -1;
    }
    {
      int before = (int)(unsigned int)prior;
      int next = before;
      long long answer = (long long)before;
      if (strcmp(name, "posix_cas_i32") == 0) {
        answer = 0;
        if (arg_count >= 3 && before == (int)ii_as_int(&args[1])) {
          next = (int)ii_as_int(&args[2]);
          answer = 1;
        }
      } else if (strcmp(name, "posix_atomic_exchange_i32") == 0) {
        next = (int)ii_as_int(&args[1]);
      } else {
        next = (int)((unsigned int)before + (unsigned int)ii_as_int(&args[1]));
      }
      if (next != before &&
          !ii_mem_write(machine, target, 4,
                        (unsigned long long)(unsigned int)next)) {
        return -1;
      }
      *result = ii_int_value(answer);
      return 1;
    }
  }

  if (arg_count >= 4 && (strcmp(name, "CreateThread") == 0 ||
                         strcmp(name, "mettle_thread_create") == 0 ||
                         strcmp(name, "pthread_create") == 0)) {
    int answers_zero_on_success = strcmp(name, "pthread_create") == 0;
    const char *thread_body_extern = NULL;
    IRFunction *thread_body = ii_token_function(
        machine, (unsigned long long)ii_as_int(&args[2]), &thread_body_extern);
    unsigned long long handle_out =
        answers_zero_on_success
            ? (unsigned long long)ii_as_int(&args[0])
            : (arg_count >= 6 ? (unsigned long long)ii_as_int(&args[5]) : 0);
    if (machine->held_mutex_count > 0 || !thread_body ||
        thread_body->instruction_count == 0) {
      return -1;
    }
    {
      IRInterpValue thread_arg = args[3];
      IRInterpValue thread_result = ii_int_value(0);
      if (!ii_exec_function(machine, thread_body, &thread_arg, 1,
                            &thread_result)) {
        return -1;
      }
    }
    machine->next_thread_handle++;
    if (handle_out &&
        !ii_mem_write(machine, handle_out, answers_zero_on_success ? 8 : 4,
                      machine->next_thread_handle)) {
      return -1;
    }
    *result = ii_int_value(
        answers_zero_on_success ? 0 : (long long)machine->next_thread_handle);
    return 1;
  }

  if (arg_count >= 1 && (strcmp(name, "CreateMutexA") == 0 ||
                         strcmp(name, "mettle_mutex_create") == 0)) {
    machine->next_thread_handle++;
    if (arg_count >= 2 && ii_as_int(&args[1]) != 0) {
      machine->held_mutex_count++;
    }
    *result = ii_int_value((long long)machine->next_thread_handle);
    return 1;
  }

  if (arg_count >= 1 && (strcmp(name, "pthread_mutex_lock") == 0 ||
                         strcmp(name, "pthread_mutex_trylock") == 0 ||
                         strcmp(name, "mettle_mutex_wait") == 0)) {
    machine->held_mutex_count++;
    *result = ii_int_value(0);
    return 1;
  }

  if (arg_count >= 1 && (strcmp(name, "pthread_mutex_unlock") == 0 ||
                         strcmp(name, "ReleaseMutex") == 0 ||
                         strcmp(name, "mettle_mutex_release") == 0)) {
    if (machine->held_mutex_count > 0) {
      machine->held_mutex_count--;
    }
    *result =
        ii_int_value(strcmp(name, "pthread_mutex_unlock") == 0 ? 0 : 1);
    return 1;
  }

  if (strcmp(name, "GetCurrentThreadId") == 0 ||
      strcmp(name, "mettle_thread_current_id") == 0 ||
      strcmp(name, "pthread_self") == 0) {
    *result = ii_int_value(1);
    return 1;
  }

  if (strcmp(name, "CloseHandle") == 0 ||
      strcmp(name, "mettle_thread_close") == 0 ||
      strcmp(name, "mettle_mutex_close") == 0) {
    *result = ii_int_value(1);
    return 1;
  }

  if (strcmp(name, "pthread_join") == 0 ||
      strcmp(name, "pthread_detach") == 0 ||
      strcmp(name, "pthread_mutex_init") == 0 ||
      strcmp(name, "pthread_mutex_destroy") == 0 ||
      strcmp(name, "pthread_cond_init") == 0 ||
      strcmp(name, "pthread_cond_destroy") == 0 ||
      strcmp(name, "pthread_cond_wait") == 0 ||
      strcmp(name, "pthread_cond_signal") == 0 ||
      strcmp(name, "pthread_cond_broadcast") == 0 ||
      strcmp(name, "mettle_thread_wait") == 0 ||
      strcmp(name, "mettle_thread_detach") == 0 ||
      strcmp(name, "WaitForSingleObject") == 0 ||
      strcmp(name, "Sleep") == 0 ||
      strcmp(name, "mettle_thread_sleep_ms") == 0 ||
      strcmp(name, "usleep") == 0 || strcmp(name, "nanosleep") == 0 ||
      strcmp(name, "posix_yield") == 0 || strcmp(name, "sched_yield") == 0) {
    *result = ii_int_value(0);
    return 1;
  }

  if ((strcmp(name, "mmap") == 0 && arg_count >= 2) ||
      (strcmp(name, "VirtualAlloc") == 0 && arg_count >= 2)) {
    long long size = ii_as_int(&args[1]);
    unsigned long long addr = 0;
    if (size <= 0 || size > II_MAX_BUFFER_SIZE) {
      return -1;
    }
    addr = ir_interp_add_buffer(machine, NULL, size);
    if (!addr) {
      *result = ii_int_value(strcmp(name, "mmap") == 0 ? -1 : 0);
      return 1;
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }
  if ((strcmp(name, "munmap") == 0 && arg_count >= 1) ||
      (strcmp(name, "VirtualFree") == 0 && arg_count >= 1)) {
    unsigned long long addr = (unsigned long long)ii_as_int(&args[0]);
    long long offset = 0;
    IIBuffer *buf = addr ? ii_addr_to_buffer(machine, addr, 0, &offset) : NULL;
    if (buf && offset == 0) {
      ii_reclaim_buffer(machine,
                        (size_t)((buf->base - II_ADDR_BASE) / II_ADDR_STRIDE));
    }
    *result = ii_int_value(0);
    return 1;
  }
  if (strcmp(name, "free") == 0 && arg_count == 1) {
    unsigned long long addr = (unsigned long long)ii_as_int(&args[0]);
    if (addr == 0) {
      return 1;
    }
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, addr, 0, &offset);
    if (!buf || offset != 0) {
      return -1;
    }
    buf->freed = 1;
    return 1;
  }
  if (strcmp(name, "realloc") == 0 && arg_count == 2) {
    unsigned long long old_addr = (unsigned long long)ii_as_int(&args[0]);
    long long new_size = ii_as_int(&args[1]);
    if (new_size < 0 || new_size > II_MAX_BUFFER_SIZE) {
      return -1;
    }
    long long old_size = old_addr ? ii_buffer_size_at(machine, old_addr) : 0;
    if (old_addr && old_size < 0) {
      return -1;
    }
    unsigned long long addr = ir_interp_add_buffer(machine, NULL, new_size);
    if (!addr) {
      return -1;
    }
    if (old_addr) {
      long long copy = old_size < new_size ? old_size : new_size;
      long long src_off = 0, dst_off = 0;
      IIBuffer *src = ii_addr_to_buffer(machine, old_addr, copy, &src_off);
      IIBuffer *dst = ii_addr_to_buffer(machine, addr, copy, &dst_off);
      if (src && dst) {
        memcpy(dst->data + dst_off, src->data + src_off, (size_t)copy);
      }
      if (src) {
        src->freed = 1;
      }
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }
  if ((strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0) &&
      arg_count == 3) {
    unsigned long long dst = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long src = (unsigned long long)ii_as_int(&args[1]);
    long long n = ii_as_int(&args[2]);
    long long dst_off = 0, src_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, n, &dst_off);
    IIBuffer *sbuf = ii_addr_to_buffer(machine, src, n, &src_off);
    if (!dbuf || !sbuf || n < 0) {
      return -1;
    }
    memmove(dbuf->data + dst_off, sbuf->data + src_off, (size_t)n);
    *result = ii_int_value((long long)dst);
    return 1;
  }
  if (strcmp(name, "memset") == 0 && arg_count == 3) {
    unsigned long long dst = (unsigned long long)ii_as_int(&args[0]);
    long long fill = ii_as_int(&args[1]);
    long long n = ii_as_int(&args[2]);
    long long dst_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, n, &dst_off);
    if (!dbuf || n < 0) {
      return -1;
    }
    memset(dbuf->data + dst_off, (int)(fill & 0xFF), (size_t)n);
    *result = ii_int_value((long long)dst);
    return 1;
  }
  if (strcmp(name, "memcmp") == 0 && arg_count == 3) {
    unsigned long long a = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long b = (unsigned long long)ii_as_int(&args[1]);
    long long n = ii_as_int(&args[2]);
    long long a_off = 0, b_off = 0;
    IIBuffer *abuf = ii_addr_to_buffer(machine, a, n, &a_off);
    IIBuffer *bbuf = ii_addr_to_buffer(machine, b, n, &b_off);
    if (!abuf || !bbuf || n < 0) {
      return -1;
    }
    *result = ii_int_value(memcmp(abuf->data + a_off, bbuf->data + b_off,
                                  (size_t)n));
    return 1;
  }

  if (arg_count == 1 && (strcmp(name, "htons") == 0 ||
                         strcmp(name, "ntohs") == 0 ||
                         strcmp(name, "htonl") == 0 ||
                         strcmp(name, "ntohl") == 0)) {
    unsigned int host = (unsigned int)ii_as_int(&args[0]);
    if (name[4] == 's') {
      unsigned int v = host & 0xFFFFu;
      *result = ii_int_value((long long)(((v & 0xFFu) << 8) | (v >> 8)));
    } else {
      *result = ii_int_value((long long)(unsigned int)(
          ((host & 0xFFu) << 24) | ((host & 0xFF00u) << 8) |
          ((host & 0xFF0000u) >> 8) | (host >> 24)));
    }
    return 1;
  }

  if (arg_count == 1 && strcmp(name, "inet_addr") == 0) {
    unsigned long long addr = (unsigned long long)ii_as_int(&args[0]);
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, addr, 1, &offset);
    if (!buf) {
      return -1;
    }
    {
      unsigned int parts[4] = {0, 0, 0, 0};
      int part = 0;
      int digits = 0;
      long long i = offset;
      int bad = 0;
      for (; i < buf->size; i++) {
        unsigned char c = buf->data[i];
        if (c == 0) {
          break;
        }
        if (c == '.') {
          if (!digits || part >= 3) {
            bad = 1;
            break;
          }
          part++;
          digits = 0;
          continue;
        }
        if (c < '0' || c > '9') {
          bad = 1;
          break;
        }
        parts[part] = parts[part] * 10u + (unsigned int)(c - '0');
        if (parts[part] > 255u || ++digits > 3) {
          bad = 1;
          break;
        }
      }
      if (bad || part != 3 || !digits || i >= buf->size) {
        *result = ii_int_value((long long)0xFFFFFFFFLL);
        return 1;
      }
      *result = ii_int_value((long long)(unsigned int)(
          parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24)));
      return 1;
    }
  }

  if (strcmp(name, "strlen") == 0 && arg_count == 1) {
    unsigned long long addr = (unsigned long long)ii_as_int(&args[0]);
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, addr, 1, &offset);
    if (!buf) {
      return -1;
    }
    long long n = 0;
    while (offset + n < buf->size && buf->data[offset + n] != 0) {
      n++;
    }
    if (offset + n >= buf->size) {
      return -1; /* unterminated within the buffer: refuse to guess */
    }
    *result = ii_int_value(n);
    return 1;
  }
  if ((strcmp(name, "strcmp") == 0 && arg_count == 2) ||
      (strcmp(name, "strncmp") == 0 && arg_count == 3)) {
    long long limit = arg_count == 3 ? ii_as_int(&args[2]) : -1;
    long long a_off = 0, b_off = 0;
    IIBuffer *abuf = ii_addr_to_buffer(
        machine, (unsigned long long)ii_as_int(&args[0]), 1, &a_off);
    IIBuffer *bbuf = ii_addr_to_buffer(
        machine, (unsigned long long)ii_as_int(&args[1]), 1, &b_off);
    if (!abuf || !bbuf || (arg_count == 3 && limit < 0)) {
      return -1;
    }
    long long i = 0;
    int r = 0;
    while (limit < 0 || i < limit) {
      if (a_off + i >= abuf->size || b_off + i >= bbuf->size) {
        return -1; /* ran off a buffer before the terminator */
      }
      unsigned char ca = abuf->data[a_off + i];
      unsigned char cb = bbuf->data[b_off + i];
      if (ca != cb) {
        r = ca < cb ? -1 : 1;
        break;
      }
      if (ca == 0) {
        break;
      }
      i++;
    }
    *result = ii_int_value(r);
    return 1;
  }
  if ((strcmp(name, "strcpy") == 0 && arg_count == 2) ||
      (strcmp(name, "strncpy") == 0 && arg_count == 3) ||
      (strcmp(name, "strcat") == 0 && arg_count == 2)) {
    long long limit = arg_count == 3 ? ii_as_int(&args[2]) : -1;
    unsigned long long dst_addr = (unsigned long long)ii_as_int(&args[0]);
    long long d_off = 0, s_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst_addr, 1, &d_off);
    IIBuffer *sbuf = ii_addr_to_buffer(
        machine, (unsigned long long)ii_as_int(&args[1]), 1, &s_off);
    if (!dbuf || !sbuf || (arg_count == 3 && limit < 0)) {
      return -1;
    }
    if (strcmp(name, "strcat") == 0) { /* append at dst's terminator */
      while (d_off < dbuf->size && dbuf->data[d_off] != 0) {
        d_off++;
      }
      if (d_off >= dbuf->size) {
        return -1;
      }
    }
    long long i = 0;
    for (;;) {
      if (limit >= 0 && i >= limit) {
        break;
      }
      if (s_off + i >= sbuf->size || d_off + i >= dbuf->size) {
        return -1; /* overrun either side: refuse */
      }
      unsigned char c = sbuf->data[s_off + i];
      dbuf->data[d_off + i] = c;
      if (c == 0) {
        i++;
        break;
      }
      i++;
    }
    if (limit >= 0) { /* strncpy zero-fills to the limit */
      while (i < limit && d_off + i < dbuf->size) {
        dbuf->data[d_off + i++] = 0;
      }
    }
    *result = ii_int_value((long long)dst_addr);
    return 1;
  }
  if ((strcmp(name, "strchr") == 0 || strcmp(name, "memchr") == 0) &&
      (arg_count == 2 || arg_count == 3)) {
    unsigned long long base = (unsigned long long)ii_as_int(&args[0]);
    unsigned char want = (unsigned char)ii_as_int(&args[1]);
    long long limit = arg_count == 3 ? ii_as_int(&args[2]) : -1;
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, base, 1, &offset);
    if (!buf || (arg_count == 3 && limit < 0)) {
      return -1;
    }
    long long i = 0;
    for (;;) {
      if (limit >= 0 && i >= limit) {
        break;
      }
      if (offset + i >= buf->size) {
        return -1;
      }
      unsigned char c = buf->data[offset + i];
      if (c == want) {
        *result = ii_int_value((long long)(base + (unsigned long long)i));
        return 1;
      }
      if (limit < 0 && c == 0) {
        break;
      }
      i++;
    }
    *result = ii_int_value(0);
    return 1;
  }
  if ((strcmp(name, "abs") == 0 || strcmp(name, "labs") == 0 ||
       strcmp(name, "llabs") == 0) &&
      arg_count == 1) {
    long long v = ii_as_int(&args[0]);
    if (strcmp(name, "abs") == 0) {
      int iv = (int)v;
      *result = ii_int_value(iv < 0 ? -iv : iv);
    } else {
      *result = ii_int_value(v < 0 ? -v : v);
    }
    return 1;
  }
  if (strcmp(name, "mettle_heap_zeroed") == 0 && arg_count == 1) {
    /* The lowered form of `new T`: zeroed heap storage. */
    long long size = ii_as_int(&args[0]);
    if (size < 0 || size > II_MAX_BUFFER_SIZE) {
      return -1;
    }
    unsigned long long addr = ir_interp_add_buffer(machine, NULL, size);
    if (!addr) {
      return -1;
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }

  /* assert()/assert_eq() builtins: interpreted natively by `mettle test`. */
  if (strcmp(name, "assert_eq") == 0 && arg_count == 2) {
    if (!ii_value_matches(&args[0], &args[1])) {
      machine->assert_failed = 1;
      machine->assert_line = machine->current_call_loc.line;
      machine->assert_column = machine->current_call_loc.column;
      machine->assert_left = args[0];
      machine->assert_right = args[1];
      machine->assert_is_eq = 1;
      ii_fail(machine, IR_INTERP_ASSERT_FAIL, "assert_eq");
      return -1;
    }
    return 1;
  }
  if (strcmp(name, "assert") == 0 && arg_count == 1) {
    long long cond = args[0].is_float ? (args[0].f != 0.0) : (args[0].i != 0);
    if (!cond) {
      machine->assert_failed = 1;
      machine->assert_line = machine->current_call_loc.line;
      machine->assert_column = machine->current_call_loc.column;
      machine->assert_left = args[0];
      machine->assert_right = ii_int_value(0);
      machine->assert_is_eq = 0;
      ii_fail(machine, IR_INTERP_ASSERT_FAIL, "assert");
      return -1;
    }
    return 1;
  }

  /* Runtime guard traps (null-check, bounds) abort the program. */
  if (strncmp(name, "mettle_crash_trap", 17) == 0) {
    ii_fail(machine, IR_INTERP_GUARD_TRAP, name);
    return -1;
  }

  /* The string runtime, modeled so interpreted string programs mean what they
   * mean natively. mettle_string_from_f64 mirrors the Mettle implementation in
   * src/runtime/string.mettle operation for operation; a change there without
   * one here shows up as an interp-vs-native output difference. */
  if (strcmp(name, "mettle_string_eq") == 0 && arg_count == 2) {
    const unsigned char *left_bytes = NULL, *right_bytes = NULL;
    size_t left_length = 0, right_length = 0;
    if (!ii_read_string(machine, (unsigned long long)ii_as_int(&args[0]),
                        &left_bytes, &left_length) ||
        !ii_read_string(machine, (unsigned long long)ii_as_int(&args[1]),
                        &right_bytes, &right_length)) {
      return -1;
    }
    *result = ii_int_value(left_length == right_length &&
                           (left_length == 0 ||
                            memcmp(left_bytes, right_bytes, left_length) == 0));
    return 1;
  }
  if ((strcmp(name, "mettle_string_from_uint") == 0 ||
       strcmp(name, "mettle_string_from_int") == 0) &&
      arg_count == 1) {
    char digits[24];
    int written =
        name[19] == 'u'
            ? snprintf(digits, sizeof digits, "%llu",
                       (unsigned long long)ii_as_int(&args[0]))
            : snprintf(digits, sizeof digits, "%lld",
                       (long long)ii_as_int(&args[0]));
    unsigned long long record =
        written > 0 ? ii_make_string(machine, digits, (size_t)written) : 0;
    if (!record) {
      return -1;
    }
    *result = ii_int_value((long long)record);
    return 1;
  }
  if (strcmp(name, "mettle_string_from_char") == 0 && arg_count == 1) {
    char byte = (char)(unsigned char)ii_as_int(&args[0]);
    unsigned long long record = ii_make_string(machine, &byte, 1);
    if (!record) {
      return -1;
    }
    *result = ii_int_value((long long)record);
    return 1;
  }
  if (strcmp(name, "mettle_string_from_bool") == 0 && arg_count == 1) {
    const char *text = ii_as_int(&args[0]) != 0 ? "true" : "false";
    unsigned long long record = ii_make_string(machine, text, strlen(text));
    if (!record) {
      return -1;
    }
    *result = ii_int_value((long long)record);
    return 1;
  }
  if (strcmp(name, "mettle_string_from_f64") == 0 && arg_count == 1) {
    double value = 0.0;
    if (args[0].is_float) {
      value = args[0].f;
    } else {
      long long raw_bits = args[0].i;
      memcpy(&value, &raw_bits, 8);
    }
    char text[64];
    size_t text_length = 0;
    if (value != value) {
      text_length = (size_t)snprintf(text, sizeof text, "nan");
    } else {
      const char *sign = "";
      if (value < 0.0) {
        sign = "-";
        value = 0.0 - value;
      }
      if (value * 0.5 == value) {
        text_length = (size_t)snprintf(text, sizeof text, "%s%s", sign,
                                       value != 0.0 ? "inf" : "0.0");
      } else {
        long long exponent = 0;
        int scientific = value >= 100000000000000000.0 || value < 0.0001;
        if (scientific) {
          while (value >= 10.0) {
            value = value / 10.0;
            exponent = exponent + 1;
          }
          while (value < 1.0) {
            value = value * 10.0;
            exponent = exponent - 1;
          }
        }
        long long int_part = (long long)value;
        double frac = value - (double)int_part;
        long long scaled = (long long)(frac * 1000000.0 + 0.5);
        if (scaled >= 1000000) {
          scaled = scaled - 1000000;
          int_part = int_part + 1;
        }
        char frac_digits[8];
        snprintf(frac_digits, sizeof frac_digits, "%06lld", scaled);
        size_t frac_length = 6;
        while (frac_length > 1 && frac_digits[frac_length - 1] == '0') {
          frac_length--;
        }
        if (scientific) {
          text_length = (size_t)snprintf(text, sizeof text, "%s%lld.%.*se%lld",
                                         sign, int_part, (int)frac_length,
                                         frac_digits, exponent);
        } else {
          text_length = (size_t)snprintf(text, sizeof text, "%s%lld.%.*s",
                                         sign, int_part, (int)frac_length,
                                         frac_digits);
        }
      }
    }
    unsigned long long record = ii_make_string(machine, text, text_length);
    if (!record) {
      return -1;
    }
    *result = ii_int_value((long long)record);
    return 1;
  }

  {
    IRInterpValue math_result = ii_float_value(0.0);
    if (ii_extern_math(name, args, arg_count, &math_result)) {
      *result = math_result;
      return 1;
    }
  }

  /* Unknown extern: the call is traced so a pass that deletes or reorders it
   * still diverges, and the answer is marked undefined. Zero is a value the
   * program will branch on -- `dir_exists` answering 0 sends it down the
   * directory-is-missing arm and every step after that is fiction. Marking it
   * lets whoever consumes the run say it does not know, rather than report a
   * number computed from a guess. */
  ii_trace_extern(machine, name, args, arg_count);
  result->undefined = 1;
  return machine->status == IR_INTERP_OK ? 0 : -1;
}

static double ii_outer_lane_uniform(const IRInstruction *insn, size_t prog,
                                    size_t fconst_at, long long n_fconst,
                                    long long index) {
  long long micro = insn->arguments[prog].int_value;
  long long iv = index;
  double fv = 0.0;
  int in_float = 0;
  size_t at = prog + 1;
  for (long long m = 0; m < micro; m++, at += 2) {
    long long op = insn->arguments[at].int_value;
    long long imm = insn->arguments[at + 1].int_value;
    double k = 0.0;
    if (op >= 10 && imm >= 0 && imm < n_fconst) {
      k = insn->arguments[fconst_at + (size_t)imm].float_value;
    }
    switch (op) {
    case 1: iv &= imm; break;
    case 2: iv |= imm; break;
    case 3: iv ^= imm; break;
    case 4: iv += imm; break;
    case 5: iv -= imm; break;
    case 6: iv *= imm; break;
    case 7: iv = (long long)((unsigned long long)iv << imm); break;
    case 8: iv >>= imm; break;
    case 9: fv = (double)iv; in_float = 1; break;
    case 10: fv += k; break;
    case 11: fv -= k; break;
    case 12: fv *= k; break;
    case 13: fv /= k; break;
    default: break;
    }
  }
  return in_float ? fv : (double)iv;
}

/* ---------------- SIMD kernel ops (documented scalar semantics) ---------- */

static int ii_exec_memory(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_PREFETCH:
    /* Advisory cache hint: no architectural effect, nothing to interpret. */
    return 1;
  case IR_OP_MEMCPY_INLINE: {
    unsigned long long dst, src;
    long long n;
    if (!ii_fetch_addr(machine, frame, &insn->dest, &dst) ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n)) {
      return 0;
    }
    long long dst_off = 0, src_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, n, &dst_off);
    IIBuffer *sbuf = ii_addr_to_buffer(machine, src, n, &src_off);
    if (!dbuf || !sbuf || n < 0) {
      ii_fail(machine, IR_INTERP_TRAP, "memcpy_inline out of bounds");
      return 0;
    }
    memmove(dbuf->data + dst_off, sbuf->data + src_off, (size_t)n);
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_COUNT_WORD_STARTS: {
    unsigned long long base;
    long long n;
    IRInterpValue acc;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long count = 0;
    int in_word = 0;
    for (long long i = 0; i < n; i++) {
      unsigned long long byte;
      if (!ii_mem_read(machine, base + (unsigned long long)i, 1, &byte)) {
        return 0;
      }
      int ws = byte == 0x20 || byte == 0x09 || byte == 0x0A || byte == 0x0D;
      if (ws) {
        in_word = 0;
      } else {
        if (!in_word) {
          count++;
        }
        in_word = 1;
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + count);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_slp_mac(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8: {
    /* K parallel int32 MAC reductions sharing a broadcast scalar:
     * out[out_off+j] = sum_k a[a_off+k] * b[b_off + k*b_stride + j].
     * The I8 variant reads bytes zero-extended, the way the kernel's
     * movzx/vpmovzxbd widen them. */
    unsigned long long out_base, a_base, b_base;
    long long K, count, a_off, b_off, b_stride, out_off;
    if (insn->argument_count < 6 ||
        !ii_fetch_addr(machine, frame, &insn->dest, &out_base) ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &a_base) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &b_base) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &K) ||
        !ii_fetch_int(machine, frame, &insn->arguments[1], &count) ||
        !ii_fetch_int(machine, frame, &insn->arguments[2], &a_off) ||
        !ii_fetch_int(machine, frame, &insn->arguments[3], &b_off) ||
        !ii_fetch_int(machine, frame, &insn->arguments[4], &b_stride) ||
        !ii_fetch_int(machine, frame, &insn->arguments[5], &out_off)) {
      return 0;
    }
    if (K < 1 || K > 8) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "slp_mac lane count");
      return 0;
    }
    int is_i8 = insn->op == IR_OP_SIMD_SLP_MAC_I8;
    int elem = is_i8 ? 1 : 4;
    for (long long j = 0; j < K; j++) {
      unsigned int sum = 0;
      for (long long k = 0; k < count; k++) {
        unsigned long long araw, braw;
        unsigned long long a_addr =
            a_base + (unsigned long long)((a_off + k) * elem);
        unsigned long long b_addr =
            b_base + (unsigned long long)((b_off + k * b_stride + j) * elem);
        if (!ii_mem_read(machine, a_addr, elem, &araw) ||
            !ii_mem_read(machine, b_addr, elem, &braw)) {
          return 0;
        }
        unsigned int av = is_i8 ? (unsigned int)(unsigned char)araw
                                : (unsigned int)araw;
        unsigned int bv = is_i8 ? (unsigned int)(unsigned char)braw
                                : (unsigned int)braw;
        sum += av * bv;
      }
      if (!ii_write_i32(machine,
                        out_base + (unsigned long long)((out_off + j) * 4),
                        (int)sum)) {
        return 0;
      }
    }
    machine->fuel -= K * count;
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_int_reduce(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8: {
    unsigned long long base;
    long long n;
    IRInterpValue acc;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long sum = 0;
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_SUM_I32) {
        int v;
        if (!ii_read_i32(machine, base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        sum += v;
      } else {
        unsigned long long v;
        if (!ii_mem_read(machine, base + (unsigned long long)i, 1, &v)) {
          return 0;
        }
        sum += (long long)v;
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_BYTE_MAP: {
    unsigned long long base;
    long long n;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n)) {
      return 0;
    }
    for (long long i = 0; i < n; i++) {
      unsigned long long raw;
      if (!ii_mem_read(machine, base + (unsigned long long)i, 1, &raw)) {
        return 0;
      }
      unsigned int b = (unsigned int)raw & 0xFF;
      for (size_t s = 0; s + 1 < insn->argument_count; s += 2) {
        long long code = insn->arguments[s].int_value;
        unsigned int k = (unsigned int)insn->arguments[s + 1].int_value & 0xFF;
        switch (code) {
        case IR_BYTE_MAP_ADD: b = (b + k) & 0xFF; break;
        case IR_BYTE_MAP_SUB: b = (b - k) & 0xFF; break;
        case IR_BYTE_MAP_MUL: b = (b * k) & 0xFF; break;
        case IR_BYTE_MAP_XOR: b = (b ^ k) & 0xFF; break;
        case IR_BYTE_MAP_AND: b = (b & k) & 0xFF; break;
        case IR_BYTE_MAP_OR:  b = (b | k) & 0xFF; break;
        default:
          ii_fail(machine, IR_INTERP_UNSUPPORTED, "byte_map op");
          return 0;
        }
      }
      if (!ii_mem_write(machine, base + (unsigned long long)i, 1, b)) {
        return 0;
      }
    }
    machine->fuel -= n;
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_fill_indexed(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, long long elem_size,
                      unsigned long long fill_bits) {
  unsigned long long base;
  long long bound, start = 0, offset = 0;
  if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
      !ii_fetch_int(machine, frame, &insn->rhs, &bound)) {
    return 0;
  }
  if (insn->argument_count > 3 &&
      !ii_fetch_int(machine, frame, &insn->arguments[3], &start)) {
    return 0;
  }
  if (insn->argument_count > 4 &&
      !ii_fetch_int(machine, frame, &insn->arguments[4], &offset)) {
    return 0;
  }
  for (long long i = start; i < bound; i++) {
    int idx32 = (int)(offset + i); /* 32-bit index math, like the loop */
    unsigned long long addr =
        base + (unsigned long long)((long long)idx32 * elem_size);
    if (!ii_mem_write(machine, addr, (int)elem_size, fill_bits)) {
      return 0;
    }
  }
  machine->fuel -= bound > start ? bound - start : 0;
  if (insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name) {
    long long final_value = bound > start ? bound : start;
    int wide = insn->argument_count > 5 &&
               insn->arguments[5].kind == IR_OPERAND_INT &&
               insn->arguments[5].int_value == 64;
    IRInterpValue out;
    if (!wide) {
      final_value = (long long)(int)final_value;
    }
    out = ii_int_value(final_value);
    if (!ii_store_dest(machine, frame, &insn->dest, &out)) {
      return 0;
    }
  }
  return 1;
}

static int ii_fill_range(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, long long elem_size,
                      unsigned long long fill_bits) {
  unsigned long long begin, end;
  if (!ii_fetch_addr(machine, frame, &insn->lhs, &begin) ||
      !ii_fetch_addr(machine, frame, &insn->rhs, &end)) {
    return 0;
  }
  long long steps = 0;
  for (unsigned long long p = begin; p < end; p += (unsigned long long)elem_size) {
    if (!ii_mem_write(machine, p, (int)elem_size, fill_bits)) {
      return 0;
    }
    steps++;
  }
  machine->fuel -= steps;
  return 1;
}

static int ii_fill_offset(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, long long elem_size,
                      unsigned long long fill_bits) {
  unsigned long long base;
  long long bound, start = 0;
  if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
      !ii_fetch_int(machine, frame, &insn->rhs, &bound)) {
    return 0;
  }
  if (insn->argument_count > 3 &&
      !ii_fetch_int(machine, frame, &insn->arguments[3], &start)) {
    return 0;
  }
  long long steps = 0;
  long long off = start;
  for (; off < bound; off += elem_size) {
    if (!ii_mem_write(machine, base + (unsigned long long)off,
                      (int)elem_size, fill_bits)) {
      return 0;
    }
    steps++;
  }
  machine->fuel -= steps;
  if (insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name) {
    IRInterpValue out = ii_int_value(off);
    if (!ii_store_dest(machine, frame, &insn->dest, &out)) {
      return 0;
    }
  }
  return 1;
}

static int ii_exec_fill(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_FILL: {
    if (insn->argument_count < 3) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_fill arity");
      return 0;
    }
    long long elem_size = insn->arguments[0].int_value;
    long long mode = insn->arguments[1].int_value;
    if (elem_size != 1 && elem_size != 2 && elem_size != 4 && elem_size != 8) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_fill element size");
      return 0;
    }
    unsigned long long fill_bits = 0;
    if (insn->arguments[2].kind == IR_OPERAND_INT) {
      fill_bits = (unsigned long long)insn->arguments[2].int_value;
    } else {
      IRInterpValue v;
      if (!ii_fetch(machine, frame, &insn->arguments[2], &v)) {
        return 0;
      }
      if (v.is_float) {
        if (elem_size == 4) {
          float f = (float)v.f;
          unsigned int bits;
          memcpy(&bits, &f, 4);
          fill_bits = bits;
        } else {
          memcpy(&fill_bits, &v.f, 8);
        }
      } else {
        fill_bits = (unsigned long long)v.i;
      }
    }
    if (mode == 0) {
      return ii_fill_indexed(machine, frame, insn, elem_size, fill_bits);
    }
    if (mode == 1) {
      return ii_fill_range(machine, frame, insn, elem_size, fill_bits);
    }
    if (mode == 2) {
      return ii_fill_offset(machine, frame, insn, elem_size, fill_bits);
    }
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_fill mode");
    return 0;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_sort(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_INSERTION_SORT_I32: {
    unsigned long long base;
    long long n;
    if (!ii_fetch_addr(machine, frame, &insn->dest, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n)) {
      return 0;
    }
    for (long long i = 1; i < n; i++) {
      int key;
      if (!ii_read_i32(machine, base + (unsigned long long)i * 4, &key)) {
        return 0;
      }
      long long j = i - 1;
      while (j >= 0) {
        int v;
        if (!ii_read_i32(machine, base + (unsigned long long)j * 4, &v)) {
          return 0;
        }
        if (v <= key) {
          break;
        }
        if (!ii_write_i32(machine, base + (unsigned long long)(j + 1) * 4, v)) {
          return 0;
        }
        j--;
        machine->fuel--;
      }
      if (!ii_write_i32(machine, base + (unsigned long long)(j + 1) * 4, key)) {
        return 0;
      }
    }
    machine->fuel -= n;
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_int_dot(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_DOT_I32: {
    unsigned long long a, b;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &a) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &b) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long sum = 0;
    for (long long i = 0; i < n; i++) {
      int x, y;
      if (!ii_read_i32(machine, a + (unsigned long long)i * 4, &x) ||
          !ii_read_i32(machine, b + (unsigned long long)i * 4, &y)) {
        return 0;
      }
      sum += (long long)x * (long long)y;
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_DOT_I8: {
    /* byte x byte -> int32 dot: each byte widens the way the source's loads
     * did, which insn->is_unsigned records, accumulated with int32 wraparound
     * into the dest sum. Widening every byte zero-extended read a negative
     * int8 as its unsigned value. */
    unsigned long long a, b;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &a) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &b) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    unsigned int sum = 0;
    for (long long i = 0; i < n; i++) {
      unsigned long long x, y;
      if (!ii_mem_read(machine, a + (unsigned long long)i, 1, &x) ||
          !ii_mem_read(machine, b + (unsigned long long)i, 1, &y)) {
        return 0;
      }
      {
        int xw = insn->is_unsigned ? (int)(unsigned char)x
                                   : (int)(signed char)(unsigned char)x;
        int yw = insn->is_unsigned ? (int)(unsigned char)y
                                   : (int)(signed char)(unsigned char)y;
        sum += (unsigned int)(xw * yw);
      }
    }
    machine->fuel -= n;
    IRInterpValue out =
        ii_int_value((long long)(int)((unsigned int)ii_as_int(&acc) + sum));
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_PREFIX_SUM_I32: {
    /* Inclusive int32 prefix sum: dst[i] = sum(src[0..i]) wrapping at 32
     * bits; dest accumulates the int64 running sum of the outputs. */
    unsigned long long src, dst;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &dst) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    /* The kernel seeds the running sum from dest's prior value, adds each
     * sign-extended src element into the 64-bit accumulator, and stores its
     * low 32 bits per element; dest keeps the full 64-bit sum. */
    long long run = ii_as_int(&acc);
    for (long long i = 0; i < n; i++) {
      int v;
      if (!ii_read_i32(machine, src + (unsigned long long)i * 4, &v)) {
        return 0;
      }
      run += (long long)v;
      if (!ii_write_i32(machine, dst + (unsigned long long)i * 4, (int)run)) {
        return 0;
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(run);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_int_map(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32: {
    unsigned long long src, dst;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &dst) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long p1 = 0, p2 = 0;
    if (insn->op != IR_OP_SIMD_REVERSE_COPY_I32) {
      if (insn->argument_count < 3 ||
          !ii_fetch_int(machine, frame, &insn->arguments[1], &p1) ||
          !ii_fetch_int(machine, frame, &insn->arguments[2], &p2)) {
        return 0;
      }
    }
    long long sum = 0;
    for (long long i = 0; i < n; i++) {
      long long src_index = insn->op == IR_OP_SIMD_REVERSE_COPY_I32 ? n - 1 - i : i;
      int v;
      if (!ii_read_i32(machine, src + (unsigned long long)src_index * 4, &v)) {
        return 0;
      }
      int outv;
      if (insn->op == IR_OP_SIMD_SCALE_I32) {
        outv = (int)((unsigned int)v * (unsigned int)(int)p1 +
                     (unsigned int)(int)p2);
      } else if (insn->op == IR_OP_SIMD_CLAMP_I32) {
        int lo = (int)p1, hi = (int)p2;
        outv = v < lo ? lo : v > hi ? hi : v;
      } else {
        outv = v;
      }
      if (!ii_write_i32(machine, dst + (unsigned long long)i * 4, outv)) {
        return 0;
      }
      sum += outv;
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_int_search(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_LOWER_BOUND_I32: {
    unsigned long long base;
    long long n, key, lo0;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &key) ||
        !ii_fetch_int(machine, frame, &insn->dest, &lo0)) {
      return 0;
    }
    /* dest is in/out: codegen seeds the running lo from dest's prior value
     * (the recognizer guarantees the source loop starts it at 0). Reading 0
     * here regardless would hide a transform that deletes the init. */
    long long lo = lo0, hi = n;
    while (lo < hi) {
      long long mid = lo + (hi - lo) / 2;
      int v;
      if (!ii_read_i32(machine, base + (unsigned long long)mid * 4, &v)) {
        return 0;
      }
      if ((long long)v < key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
      machine->fuel--;
    }
    IRInterpValue out = ii_int_value(lo);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_MINMAX_I32: {
    unsigned long long base;
    long long n;
    IRInterpValue minv, maxv;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &minv) ||
        !ii_fetch(machine, frame, &insn->arguments[0], &maxv)) {
      return 0;
    }
    long long mn = ii_as_int(&minv), mx = ii_as_int(&maxv);
    for (long long i = 1; i < n; i++) {
      int v;
      if (!ii_read_i32(machine, base + (unsigned long long)i * 4, &v)) {
        return 0;
      }
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    machine->fuel -= n;
    IRInterpValue out_min = ii_int_value(mn), out_max = ii_int_value(mx);
    return ii_store_dest(machine, frame, &insn->dest, &out_min) &&
           ii_store_dest(machine, frame, &insn->arguments[0], &out_max);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_float_reduce(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32: {
    unsigned long long base;
    long long n;
    IRInterpValue acc;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    double sum = ii_as_float(&acc);
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_SUM_F64) {
        double v;
        if (!ii_read_f64(machine, base + (unsigned long long)i * 8, &v)) {
          return 0;
        }
        sum += v;
      } else {
        float v;
        if (!ii_read_f32(machine, base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        sum = (double)(float)((float)sum + v);
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_float_value(sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32: {
    unsigned long long a, b;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &a) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &b) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    double sum = ii_as_float(&acc);
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_DOT_F64) {
        double x, y;
        if (!ii_read_f64(machine, a + (unsigned long long)i * 8, &x) ||
            !ii_read_f64(machine, b + (unsigned long long)i * 8, &y)) {
          return 0;
        }
        sum += x * y;
      } else {
        float x, y;
        if (!ii_read_f32(machine, a + (unsigned long long)i * 4, &x) ||
            !ii_read_f32(machine, b + (unsigned long long)i * 4, &y)) {
          return 0;
        }
        sum = (double)(float)((float)sum + x * y);
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_float_value(sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32: {
    unsigned long long src, dst;
    long long n;
    if (insn->argument_count < 4 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &dst) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n)) {
      return 0;
    }
    IRInterpValue va, vb, vc;
    if (!ii_fetch(machine, frame, &insn->arguments[1], &va) ||
        !ii_fetch(machine, frame, &insn->arguments[2], &vb) ||
        !ii_fetch(machine, frame, &insn->arguments[3], &vc)) {
      return 0;
    }
    double ka = ii_as_float(&va), kb = ii_as_float(&vb), kc = ii_as_float(&vc);
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_AFFINE_MAP_F64) {
        double x, y;
        if (!ii_read_f64(machine, src + (unsigned long long)i * 8, &x) ||
            !ii_read_f64(machine, dst + (unsigned long long)i * 8, &y)) {
          return 0;
        }
        if (!ii_write_f64(machine, dst + (unsigned long long)i * 8,
                          ka * x + kb * y + kc)) {
          return 0;
        }
      } else {
        float x, y;
        if (!ii_read_f32(machine, src + (unsigned long long)i * 4, &x) ||
            !ii_read_f32(machine, dst + (unsigned long long)i * 4, &y)) {
          return 0;
        }
        float r = (float)ka * x + (float)kb * y + (float)kc;
        if (!ii_write_f32(machine, dst + (unsigned long long)i * 4, r)) {
          return 0;
        }
      }
    }
    machine->fuel -= n;
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_float_map(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32: {
    unsigned long long out_base, g_base = 0, u_base = 0;
    long long n;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->dest, &out_base) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n)) {
      return 0;
    }
    int has_mul = 0;
    if (insn->op == IR_OP_SIMD_SILU_F32) {
      if (!ii_fetch_addr(machine, frame, &insn->lhs, &g_base)) {
        return 0;
      }
      if (insn->rhs.kind == IR_OPERAND_STRING &&
          (!insn->rhs.name || insn->rhs.name[0] == '\0')) {
        has_mul = 0;
      } else if (insn->rhs.kind == IR_OPERAND_NONE) {
        has_mul = 0;
      } else {
        if (!ii_fetch_addr(machine, frame, &insn->rhs, &u_base)) {
          return 0;
        }
        has_mul = 1;
      }
    }
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_EXP_F32) {
        float v;
        if (!ii_read_f32(machine, out_base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        if (!ii_write_f32(machine, out_base + (unsigned long long)i * 4,
                          expf(v))) {
          return 0;
        }
      } else {
        float g;
        if (!ii_read_f32(machine, g_base + (unsigned long long)i * 4, &g)) {
          return 0;
        }
        float silu = g / (1.0f + expf(-g));
        float r = silu;
        if (has_mul) {
          float u;
          if (!ii_read_f32(machine, u_base + (unsigned long long)i * 4, &u)) {
            return 0;
          }
          r = silu * u;
        }
        if (!ii_write_f32(machine, out_base + (unsigned long long)i * 4, r)) {
          return 0;
        }
      }
    }
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_SIMD_I2F_REDUCE_F64: {
    if (insn->argument_count < 1) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "i2f_reduce arity");
      return 0;
    }
    long long trip = insn->arguments[0].int_value;
    IRInterpValue acc;
    if (!ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long sum = ii_as_int(&acc);
    for (long long i = 0; i < trip; i++) {
      double x = (double)i;
      for (size_t s = 1; s + 1 < insn->argument_count; s += 2) {
        long long code = insn->arguments[s].int_value;
        double k = insn->arguments[s + 1].float_value;
        switch (code) {
        case 0: x = x * k; break; /* I2F_STEP_MUL */
        case 1: x = x + k; break; /* I2F_STEP_ADD */
        case 2: x = x - k; break; /* I2F_STEP_SUBR */
        case 3: x = k - x; break; /* I2F_STEP_SUBL */
        case 4: x = x / k; break; /* I2F_STEP_DIVR */
        default:
          ii_fail(machine, IR_INTERP_UNSUPPORTED, "i2f step code");
          return 0;
        }
      }
      sum += (long long)x; /* trunc toward zero; range proven by the pass */
    }
    machine->fuel -= trip;
    IRInterpValue out = ii_int_value(sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_outer_lane(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_OUTER_LANE_F64: {
    enum { OL_ADD = 0, OL_SUB = 1, OL_MUL = 2, OL_DIV = 3 };
    if (insn->argument_count < 8) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "outer_lane arity");
      return 0;
    }
    long long outer_trips = 0, inner_bound = 0;
    IRInterpValue total_v;
    if (!ii_fetch_int(machine, frame, &insn->lhs, &outer_trips) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &inner_bound) ||
        !ii_fetch(machine, frame, &insn->dest, &total_v)) {
      return 0;
    }
    long long inner_cmp = insn->arguments[0].int_value;
    long long istep = insn->arguments[1].int_value;
    long long n_chain = insn->arguments[2].int_value;
    long long n_unif = insn->arguments[3].int_value;
    long long n_fconst = insn->arguments[4].int_value;
    long long i0 = insn->arguments[5].int_value;
    long long seed_mode = insn->arguments[6].int_value;
    double seed_const = insn->arguments[7].float_value;
    if (istep == 0 || n_chain < 0 || n_unif < 0 || n_fconst < 0) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "outer_lane encoding");
      return 0;
    }
    size_t at = 8;
    size_t chain_at = at;
    at += (size_t)(4 * n_chain);
    size_t unif_at = at;
    for (long long u = 0; u < n_unif; u++) {
      if (at >= insn->argument_count) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "outer_lane encoding");
        return 0;
      }
      at += 1 + (size_t)(2 * insn->arguments[at].int_value);
    }
    size_t seed_at = at;
    if (seed_mode == 1) {
      if (at >= insn->argument_count) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "outer_lane encoding");
        return 0;
      }
      at += 1 + (size_t)(2 * insn->arguments[at].int_value);
    }
    size_t fconst_at = at;
    if (fconst_at + (size_t)n_fconst > insn->argument_count) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "outer_lane encoding");
      return 0;
    }
    double total = ii_as_float(&total_v);
    for (long long p = 0; p < outer_trips; p++) {
      double iacc = seed_const;
      if (seed_mode == 1) {
        iacc = ii_outer_lane_uniform(insn, seed_at, fconst_at, n_fconst, p);
      }
      for (long long i = i0;
           inner_cmp ? (i <= inner_bound) : (i < inner_bound); i += istep) {
        size_t step = chain_at;
        for (long long sidx = 0; sidx < n_chain; sidx++, step += 4) {
          long long op = insn->arguments[step].int_value;
          long long side = insn->arguments[step + 1].int_value;
          long long kind = insn->arguments[step + 2].int_value;
          long long idx = insn->arguments[step + 3].int_value;
          double term;
          if (kind == 0) {
            if (idx < 0 || idx >= n_fconst) {
              ii_fail(machine, IR_INTERP_UNSUPPORTED, "outer_lane fconst");
              return 0;
            }
            term = insn->arguments[fconst_at + (size_t)idx].float_value;
          } else {
            size_t prog = unif_at;
            long long skip = idx;
            while (skip-- > 0) {
              prog += 1 + (size_t)(2 * insn->arguments[prog].int_value);
            }
            term = ii_outer_lane_uniform(insn, prog, fconst_at, n_fconst, i);
          }
          double lhs = side ? term : iacc;
          double rhs = side ? iacc : term;
          iacc = op == OL_ADD   ? lhs + rhs
                 : op == OL_SUB ? lhs - rhs
                 : op == OL_MUL ? lhs * rhs
                                : lhs / rhs;
        }
        machine->fuel--;
      }
      total += iacc;
    }
    {
      IRInterpValue out = ii_float_value(total);
      return ii_store_dest(machine, frame, &insn->dest, &out);
    }
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_find(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  if (insn->argument_count < 4) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_find arity");
    return 0;
  }
  long long n;
  unsigned long long a_base;
  if (!ii_fetch_int(machine, frame, &insn->lhs, &n) ||
      !ii_fetch_addr(machine, frame, &insn->rhs, &a_base)) {
    return 0;
  }
  long long pred = insn->arguments[0].int_value;
  long long elem_kind = insn->arguments[1].int_value;
  long long rhs_kind = insn->arguments[2].int_value;
  long long first = 0;
  long long rhs_scalar = 0;
  unsigned long long b_base = 0;
  if (rhs_kind == 2) {
    if (!ii_fetch_addr(machine, frame, &insn->arguments[3], &b_base)) {
      return 0;
    }
  } else if (!ii_fetch_int(machine, frame, &insn->arguments[3], &rhs_scalar)) {
    return 0;
  }
  if (insn->argument_count > 4 &&
      !ii_fetch_int(machine, frame, &insn->arguments[4], &first)) {
    return 0;
  }
  /* The kernel only moves the counter forward to where the scalar loop would
   * have arrived, so with nothing to scan it must leave the counter where it
   * started. Answering `n` unconditionally was right for the not-found case
   * and wrong for an empty range: a negative length made this model hand back
   * that negative number, the scalar loop then exited on the first test with
   * it, and validation read a kernel that agrees with the source as a
   * divergence. It quarantined simd_find on every counted search under
   * --verify. */
  long long hit = n > first ? n : first;
  for (long long i = first; i < n; i++) {
    long long av, bv;
    if (elem_kind == 0) {
      int v;
      if (!ii_read_i32(machine, a_base + (unsigned long long)i * 4, &v)) {
        return 0;
      }
      av = v;
    } else {
      unsigned long long v;
      if (!ii_mem_read(machine, a_base + (unsigned long long)i, 1, &v)) {
        return 0;
      }
      av = (long long)v;
    }
    if (rhs_kind == 2) {
      if (elem_kind == 0) {
        int v;
        if (!ii_read_i32(machine, b_base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        bv = v;
      } else {
        unsigned long long v;
        if (!ii_mem_read(machine, b_base + (unsigned long long)i, 1, &v)) {
          return 0;
        }
        bv = (long long)v;
      }
    } else {
      bv = rhs_scalar;
    }
    int match;
    switch (pred) {
    case 0: match = av == bv; break;
    case 1: match = av != bv; break;
    case 2: match = av < bv; break;
    case 3: match = av > bv; break;
    case 4: match = av <= bv; break;
    case 5: match = av >= bv; break;
    case 6:
      match = !((av >= 'a' && av <= 'z') ||
                (av >= 'A' && av <= 'Z') ||
                (av >= '0' && av <= '9') || av == '_');
      break;
    default:
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_find predicate");
      return 0;
    }
    if (match) {
      hit = i;
      break;
    }
  }
  machine->fuel -= n > first ? n - first : 0;
  IRInterpValue out = ii_int_value(hit);
  return ii_store_dest(machine, frame, &insn->dest, &out);
}

static int ii_exec_random(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_LCG_U32: {
    if (insn->argument_count < 3) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "lcg arity");
      return 0;
    }
    long long n;
    IRInterpValue state_v, acc;
    if (!ii_fetch_int(machine, frame, &insn->lhs, &n) ||
        !ii_fetch(machine, frame, &insn->rhs, &state_v) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    unsigned int state = (unsigned int)ii_as_int(&state_v);
    unsigned int A = (unsigned int)insn->arguments[0].int_value;
    unsigned int C = (unsigned int)insn->arguments[1].int_value;
    unsigned int mask = (unsigned int)insn->arguments[2].int_value;
    long long sum = ii_as_int(&acc);
    for (long long i = 0; i < n; i++) {
      state = state * A + C;
      sum += (long long)(state & mask);
    }
    machine->fuel -= n;
    IRInterpValue out_sum = ii_int_value(sum);
    IRInterpValue out_state = ii_int_value((long long)state);
    return ii_store_dest(machine, frame, &insn->dest, &out_sum) &&
           ii_store_dest(machine, frame, &insn->rhs, &out_state);
  }

  case IR_OP_SIMD_FIND:
    return ii_exec_find(machine, frame, insn);

  default:
    *handled = 0;
    break;
  }
  return 0;
}

typedef struct {
  IRInterpMachine *machine;
  const unsigned long long *arrays;
  const double *scalars_f;
  const long long *scalars_i;
  const double *consts_f;
  const long long *consts_i;
  double *vals_f;
  long long *vals_i;
  long long n_arrays;
  long long n_scalars;
  long long n_consts;
  long long n_nodes;
  long long elem_size;
  int elem8;
  int elem8_unsigned;
} IIVloopEnv;

static int ii_vloop_node_i32(const IIVloopEnv *env, long long tag,
                             long long op0, long long op1, long long i,
                             long long *out) {
  long long v = 0;
  switch (tag) {
  case 0: { /* LOAD */
    int e;
    unsigned long long at;
    if (op0 < 0 || op0 >= env->n_arrays) {
      ii_fail(env->machine, IR_INTERP_UNSUPPORTED, "vloop load idx");
      return 0;
    }
    at = env->arrays[op0] +
         (unsigned long long)i * (unsigned long long)env->elem_size;
    if (env->elem8) {
      if (!ii_read_byte_as_i32(env->machine, at, env->elem8_unsigned, &e))
        return 0;
    } else if (!ii_read_i32(env->machine, at, &e)) {
      return 0;
    }
    v = e;
    break;
  }
  case 1: v = i; break;
  case 2: v = op0 >= 0 && op0 < env->n_consts ? env->consts_i[op0] : 0; break;
  case 3: v = (long long)(int)((unsigned int)(int)env->vals_i[op0] + (unsigned int)(int)env->vals_i[op1]); break;
  case 4: v = (long long)(int)((unsigned int)(int)env->vals_i[op0] - (unsigned int)(int)env->vals_i[op1]); break;
  case 5: v = (long long)(int)((unsigned int)(int)env->vals_i[op0] * (unsigned int)(int)env->vals_i[op1]); break;
  case 7: v = op0 >= 0 && op0 < env->n_scalars ? env->scalars_i[op0] : 0; break;
  case 8: v = (long long)(int)((int)env->vals_i[op0] & (int)env->vals_i[op1]); break;
  case 9: v = (long long)(int)((int)env->vals_i[op0] | (int)env->vals_i[op1]); break;
  case 10: v = (long long)(int)((int)env->vals_i[op0] ^ (int)env->vals_i[op1]); break;
  case 11: v = (long long)(int)((unsigned int)(int)env->vals_i[op0] << (op1 & 31)); break;
  case 12: v = (long long)(int)((int)env->vals_i[op0] >> (op1 & 31)); break;
  case 13: v = (long long)(int)((unsigned int)(int)env->vals_i[op0] >> (op1 & 31)); break;
  default:
    ii_fail(env->machine, IR_INTERP_UNSUPPORTED, "vloop int node tag");
    return 0;
  }
  *out = v;
  return 1;
}

static int ii_vloop_node_f32(const IIVloopEnv *env, long long tag,
                             long long op0, long long op1, long long i,
                             double *out) {
  float v = 0;
  switch (tag) {
  case 0: {
    float e;
    if (op0 < 0 || op0 >= env->n_arrays) {
      ii_fail(env->machine, IR_INTERP_UNSUPPORTED, "vloop load idx");
      return 0;
    }
    if (!ii_read_f32(env->machine,
                     env->arrays[op0] + (unsigned long long)i * 4, &e))
      return 0;
    v = e;
    break;
  }
  case 1: v = (float)i; break;
  case 2: v = op0 >= 0 && op0 < env->n_consts ? (float)env->consts_f[op0] : 0; break;
  case 3: v = (float)env->vals_f[op0] + (float)env->vals_f[op1]; break;
  case 4: v = (float)env->vals_f[op0] - (float)env->vals_f[op1]; break;
  case 5: v = (float)env->vals_f[op0] * (float)env->vals_f[op1]; break;
  case 6: v = (float)env->vals_f[op0] / (float)env->vals_f[op1]; break;
  case 7: v = op0 >= 0 && op0 < env->n_scalars ? (float)env->scalars_f[op0] : 0; break;
  default:
    ii_fail(env->machine, IR_INTERP_UNSUPPORTED, "vloop f32 node tag");
    return 0;
  }
  *out = (double)v;
  return 1;
}

static int ii_vloop_node_f64(const IIVloopEnv *env, long long tag,
                             long long op0, long long op1, long long i,
                             double *out) {
  double v = 0;
  switch (tag) {
  case 0: {
    if (op0 < 0 || op0 >= env->n_arrays) {
      ii_fail(env->machine, IR_INTERP_UNSUPPORTED, "vloop load idx");
      return 0;
    }
    if (!ii_read_f64(env->machine,
                     env->arrays[op0] + (unsigned long long)i * 8, &v))
      return 0;
    break;
  }
  case 1: v = (double)i; break;
  case 2: v = op0 >= 0 && op0 < env->n_consts ? env->consts_f[op0] : 0; break;
  case 3: v = env->vals_f[op0] + env->vals_f[op1]; break;
  case 4: v = env->vals_f[op0] - env->vals_f[op1]; break;
  case 5: v = env->vals_f[op0] * env->vals_f[op1]; break;
  case 6: v = env->vals_f[op0] / env->vals_f[op1]; break;
  case 7: v = op0 >= 0 && op0 < env->n_scalars ? env->scalars_f[op0] : 0; break;
  default:
    ii_fail(env->machine, IR_INTERP_UNSUPPORTED, "vloop f64 node tag");
    return 0;
  }
  *out = v;
  return 1;
}

static int ii_vloop_eval(const IIVloopEnv *env, const IRInstruction *insn,
                         size_t nodes_at, long long i, int is_int, int is_f32) {
  for (long long node = 0; node < env->n_nodes; node++) {
    long long tag = insn->arguments[nodes_at + (size_t)node * 3].int_value;
    long long op0 = insn->arguments[nodes_at + (size_t)node * 3 + 1].int_value;
    long long op1 = insn->arguments[nodes_at + (size_t)node * 3 + 2].int_value;
    if (is_int) {
      if (!ii_vloop_node_i32(env, tag, op0, op1, i, &env->vals_i[node])) {
        return 0;
      }
    } else if (is_f32) {
      if (!ii_vloop_node_f32(env, tag, op0, op1, i, &env->vals_f[node])) {
        return 0;
      }
    } else if (!ii_vloop_node_f64(env, tag, op0, op1, i, &env->vals_f[node])) {
      return 0;
    }
  }
  return 1;
}

static int ii_vloop_reduce(const IIVloopEnv *env, long long reduce_op,
                           long long root, long long i, int is_int, int is_f32,
                           unsigned long long dest_base, long long *acc_i,
                           double *acc_f) {
  if (reduce_op == 1) {
    if (is_int) {
      *acc_i = (long long)(int)((unsigned int)(int)*acc_i +
                                (unsigned int)(int)env->vals_i[root]);
    } else if (is_f32) {
      *acc_f = (double)(float)((float)*acc_f + (float)env->vals_f[root]);
    } else {
      *acc_f += env->vals_f[root];
    }
    return 1;
  }
  if (reduce_op == 2 || reduce_op == 3) {
    /* `if (v > acc) { acc = v; }`: the element only wins an ordered compare,
     * so a NaN leaves the accumulator alone -- the same rule the kernel gets
     * from MAXPS/MINPS returning src2 when unordered. */
    if (is_int) {
      long long v = (long long)(int)env->vals_i[root];
      long long a = (long long)(int)*acc_i;
      if (reduce_op == 2 ? v > a : v < a) {
        *acc_i = v;
      }
    } else {
      double v = is_f32 ? (double)(float)env->vals_f[root] : env->vals_f[root];
      if (reduce_op == 2 ? v > *acc_f : v < *acc_f) {
        *acc_f = v;
      }
    }
    return 1;
  }
  {
    unsigned long long addr =
        dest_base + (unsigned long long)i * (unsigned long long)env->elem_size;
    if (env->elem8) {
      return ii_write_byte(env->machine, addr, (int)env->vals_i[root]);
    }
    if (is_int) {
      return ii_write_i32(env->machine, addr, (int)env->vals_i[root]);
    }
    if (is_f32) {
      return ii_write_f32(env->machine, addr, (float)env->vals_f[root]);
    }
    return ii_write_f64(env->machine, addr, env->vals_f[root]);
  }
}


static int ii_vloop_read_operands(IRInterpMachine *machine, IIFrame *frame,
                                  const IRInstruction *insn, long long n_arrays,
                                  long long n_scalars, long long n_consts,
                                  long long n_nodes,
                                  unsigned long long *arrays, double *scalars_f,
                                  long long *scalars_i, double *consts_f,
                                  long long *consts_i, size_t *nodes_at) {
  size_t at = 7;
  if (n_arrays > 32 || n_scalars > 16 || n_consts > 32) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop widths");
    return 0;
  }
  for (long long k = 0; k < n_arrays; k++) {
    if (!ii_fetch_addr(machine, frame, &insn->arguments[at++], &arrays[k])) {
      return 0;
    }
  }
  for (long long k = 0; k < n_scalars; k++) {
    IRInterpValue v;
    if (!ii_fetch(machine, frame, &insn->arguments[at++], &v)) {
      return 0;
    }
    scalars_f[k] = ii_as_float(&v);
    scalars_i[k] = ii_as_int(&v);
  }
  *nodes_at = at;
  at += (size_t)(n_nodes * 3);
  for (long long k = 0; k < n_consts; k++) {
    const IROperand *c = &insn->arguments[at++];
    consts_f[k] =
        c->kind == IR_OPERAND_FLOAT ? c->float_value : (double)c->int_value;
    consts_i[k] =
        c->kind == IR_OPERAND_FLOAT ? (long long)c->float_value : c->int_value;
  }
  return 1;
}

static int ii_exec_vloop(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn, int *handled) {
  *handled = 1;
  switch (insn->op) {
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32: {
    /* Replay the serialized straight-line DAG per element (see ir.h). */
    if (insn->argument_count < 7) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop header");
      return 0;
    }
    long long reduce_op = insn->arguments[0].int_value;
    long long n_arrays = insn->arguments[1].int_value;
    long long n_nodes = insn->arguments[2].int_value;
    long long root = insn->arguments[3].int_value;
    long long n_consts = insn->arguments[4].int_value;
    long long n_scalars = insn->arguments[5].int_value;
    size_t need = (size_t)(7 + n_arrays + n_scalars + n_nodes * 3 + n_consts);
    if (n_arrays < 0 || n_nodes <= 0 || n_nodes > 64 || n_consts < 0 ||
        n_scalars < 0 || root < 0 || root >= n_nodes ||
        insn->argument_count < need) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop layout");
      return 0;
    }
    int is_int = insn->op == IR_OP_SIMD_VLOOP_I32;
    int is_f32 = !is_int && insn->float_bits == 32;
    /* Byte elements with int32 lanes: only the memory traffic narrows. */
    int elem8 = is_int && insn->float_bits == 8;
    int elem8_unsigned = elem8 && insn->is_unsigned;
    long long elem_size = elem8 ? 1 : (is_int || is_f32 ? 4 : 8);

    unsigned long long arrays[32];
    double scalars_f[16];
    long long scalars_i[16];
    double consts_f[32];
    long long consts_i[32];
    size_t nodes_at = 0;
    if (!ii_vloop_read_operands(machine, frame, insn, n_arrays, n_scalars,
                                n_consts, n_nodes, arrays, scalars_f, scalars_i,
                                consts_f, consts_i, &nodes_at)) {
      return 0;
    }

    long long trip;
    if (!ii_fetch_int(machine, frame, &insn->lhs, &trip)) {
      return 0;
    }
    unsigned long long dest_base = 0;
    IRInterpValue acc;
    /* 0 = map (dest is a base address), 1 = '+', 2 = max, 3 = min (dest is the
     * accumulator's current value). */
    int is_acc = (reduce_op >= 1 && reduce_op <= 3);
    if (is_acc) {
      if (!ii_fetch(machine, frame, &insn->dest, &acc)) {
        return 0;
      }
    } else {
      if (!ii_fetch_addr(machine, frame, &insn->dest, &dest_base)) {
        return 0;
      }
    }

    double acc_f = is_acc ? ii_as_float(&acc) : 0.0;
    long long acc_i = is_acc ? ii_as_int(&acc) : 0;

    double vals_f[64];
    long long vals_i[64];
    for (long long i = 0; i < trip; i++) {
      IIVloopEnv env;
      env.machine = machine;
      env.arrays = arrays;
      env.scalars_f = scalars_f;
      env.scalars_i = scalars_i;
      env.consts_f = consts_f;
      env.consts_i = consts_i;
      env.vals_f = vals_f;
      env.vals_i = vals_i;
      env.n_arrays = n_arrays;
      env.n_scalars = n_scalars;
      env.n_consts = n_consts;
      env.n_nodes = n_nodes;
      env.elem_size = elem_size;
      env.elem8 = elem8;
      env.elem8_unsigned = elem8_unsigned;
      if (!ii_vloop_eval(&env, insn, nodes_at, i, is_int, is_f32) ||
          !ii_vloop_reduce(&env, reduce_op, root, i, is_int, is_f32, dest_base,
                           &acc_i, &acc_f)) {
        return 0;
      }
      machine->fuel -= n_nodes;
      if (machine->fuel < 0) {
        ii_fail(machine, IR_INTERP_FUEL, "vloop fuel");
        return 0;
      }
    }
    if (is_acc) {
      IRInterpValue out = is_int ? ii_int_value(acc_i) : ii_float_value(acc_f);
      return ii_store_dest(machine, frame, &insn->dest, &out);
    }
    return 1;
  }

  default:
    *handled = 0;
    break;
  }
  return 0;
}

static int ii_exec_simd(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  static int (*const KERNELS[])(IRInterpMachine *, IIFrame *,
                                const IRInstruction *, int *) = {
      ii_exec_memory,       ii_exec_slp_mac,    ii_exec_int_reduce,
      ii_exec_fill,         ii_exec_sort,       ii_exec_int_dot,
      ii_exec_int_map,      ii_exec_int_search, ii_exec_float_reduce,
      ii_exec_float_map,    ii_exec_outer_lane, ii_exec_random,
      ii_exec_vloop};
  size_t kernel;

  for (kernel = 0; kernel < sizeof(KERNELS) / sizeof(KERNELS[0]); kernel++) {
    int handled = 0;
    int executed = KERNELS[kernel](machine, frame, insn, &handled);
    if (handled) {
      return executed;
    }
  }
  ii_fail(machine, IR_INTERP_UNSUPPORTED, ir_opcode_name(insn->op));
  return 0;
}

/* ---------------- main execution loop ---------------- */

/* `%d = c ? a : b`. Both arms are evaluated, so neither may trap. */
static int ii_op_select(IRInterpMachine *machine, IIFrame *frame,
                         const IRInstruction *insn) {
  /* Fused form (text != NULL): cond = (lhs <cmp> arguments[1]). Plain
   * form: cond = (lhs != 0). Then dest = cond ? rhs : arguments[0]. */
  long long truth = 0;
  if (insn->text && insn->argument_count > 1) {
    IRInterpValue a, b;
    if (!ii_fetch(machine, frame, &insn->lhs, &a) ||
        !ii_fetch(machine, frame, &insn->arguments[1], &b)) {
      return 0;
    }
    long long x = ii_as_int(&a), y = ii_as_int(&b);
    const char *op = insn->text;
    if (insn->is_unsigned) {
      unsigned long long ux = (unsigned long long)x, uy = (unsigned long long)y;
      truth = strcmp(op, "<") == 0    ? ux < uy
              : strcmp(op, "<=") == 0 ? ux <= uy
              : strcmp(op, ">") == 0  ? ux > uy
              : strcmp(op, ">=") == 0 ? ux >= uy
              : strcmp(op, "==") == 0 ? ux == uy
                                      : ux != uy;
    } else {
      truth = strcmp(op, "<") == 0    ? x < y
              : strcmp(op, "<=") == 0 ? x <= y
              : strcmp(op, ">") == 0  ? x > y
              : strcmp(op, ">=") == 0 ? x >= y
              : strcmp(op, "==") == 0 ? x == y
                                      : x != y;
    }
  } else {
    IRInterpValue cond;
    if (!ii_fetch(machine, frame, &insn->lhs, &cond)) {
      return 0;
    }
    truth = ii_as_int(&cond) != 0;
  }
  IRInterpValue out;
  const IROperand *chosen =
      truth ? &insn->rhs
            : (insn->argument_count > 0 ? &insn->arguments[0] : NULL);
  if (!chosen || !ii_fetch(machine, frame, chosen, &out) ||
      !ii_store_dest(machine, frame, &insn->dest, &out)) {
    return 0;
  }
  return 1;
  return 1;
}

/* A direct call: an interpreted function, or one of the runtime and libc
 * entries the machine models. */
static int ii_op_call(IRInterpMachine *machine, IIFrame *frame,
                       const IRInstruction *insn) {
  if (!insn->text) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "call without callee");
    return 0;
  }
  if (strcmp(insn->text, IR_SYSCALL_CALL_NAME) == 0) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "syscall");
    return 0;
  }
  IRInterpValue call_args[32];
  size_t call_arg_count = insn->argument_count;
  if (call_arg_count > 32) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "call arity > 32");
    return 0;
  }
  for (size_t i = 0; i < call_arg_count; i++) {
    const IROperand *arg = &insn->arguments[i];
    if (arg->kind == IR_OPERAND_STRING) {
      unsigned long long chars = 0, record = 0;
      if (!ii_string_literal(machine, arg->name,
                             ir_operand_string_length(arg), &chars,
                             &record)) {
        return 0;
      }
      call_args[i] = ii_int_value(
          (long long)(ii_callee_param_is_cstring(machine, insn->text, i)
                          ? chars
                          : record));
      continue;
    }
    if (!ii_fetch(machine, frame, arg, &call_args[i])) {
      return 0;
    }
  }
  if (!ii_copy_aggregate_args(machine, frame, insn->text, call_args,
                              call_arg_count)) {
    return 0;
  }
  IRFunction *callee = ii_find_function(machine, insn->text);
  IRInterpValue call_result = ii_int_value(0);
  if (callee && callee->instruction_count > 0) {
    if (!ii_exec_function(machine, callee, call_args, call_arg_count,
                          &call_result)) {
      return 0;
    }
  } else {
    machine->current_call_loc = insn->location;
    size_t buffers_before = machine->buffer_count;
    int handled =
        ii_extern_call(machine, insn->text, call_args, call_arg_count,
                       &call_result);
    /* Attribute any heap allocation the extern model made (malloc,
     * calloc, realloc) to this call site for leak reporting. String
     * runtime records stay unattributed: string storage has no free story
     * yet, exactly like the concat records ii_binary makes, so reporting
     * one and not the other would flag every interpolation as a leak. */
    if (strncmp(insn->text, "mettle_string_", 14) != 0) {
      for (size_t bi = buffers_before; bi < machine->buffer_count; bi++) {
        machine->buffers[bi].alloc_line = insn->location.line;
      }
    }
    if (handled < 0 || machine->status != IR_INTERP_OK) {
      if (machine->status == IR_INTERP_OK) {
        ii_fail(machine, IR_INTERP_TRAP, "extern call trap");
      }
      return 0;
    }
  }
  if (!ii_store_dest(machine, frame, &insn->dest, &call_result)) {
    return 0;
  }
  return 1;
  return 1;
}

/* Heap allocation, with the poison and bounds a later access checks against. */
static int ii_op_new(IRInterpMachine *machine, IIFrame *frame,
                      const IRInstruction *insn) {
  long long size = 8;
  if (insn->rhs.kind == IR_OPERAND_INT && insn->rhs.int_value > 0) {
    size = insn->rhs.int_value;
  } else if (insn->rhs.kind != IR_OPERAND_NONE) {
    if (!ii_fetch_int(machine, frame, &insn->rhs, &size)) {
      return 0;
    }
    if (size <= 0) {
      size = 8;
    }
  }
  unsigned long long addr = ir_interp_add_buffer(machine, NULL, size);
  if (!addr) {
    ii_fail(machine, IR_INTERP_TRAP, "new allocation");
    return 0;
  }
  machine->buffers[machine->buffer_count - 1].alloc_line =
      insn->location.line;
  IRInterpValue value = ii_int_value((long long)addr);
  if (!ii_store_dest(machine, frame, &insn->dest, &value)) {
    return 0;
  }
  return 1;
  return 1;
}

/* A call through a value: a function pointer or a closure. */
static int ii_op_call_indirect(IRInterpMachine *machine, IIFrame *frame,
                                const IRInstruction *insn) {
  IRInterpValue target;
  if (!ii_fetch(machine, frame, &insn->lhs, &target)) {
    return 0;
  }
  IRInterpValue call_args[32];
  size_t call_arg_count = insn->argument_count;
  if (call_arg_count > 32) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "call arity > 32");
    return 0;
  }
  for (size_t i = 0; i < call_arg_count; i++) {
    const IROperand *arg = &insn->arguments[i];
    if (arg->kind == IR_OPERAND_STRING) {
      unsigned long long chars = 0, record = 0;
      if (!ii_string_literal(machine, arg->name,
                             ir_operand_string_length(arg), &chars,
                             &record)) {
        return 0;
      }
      call_args[i] = ii_int_value((long long)record);
      continue;
    }
    if (!ii_fetch(machine, frame, arg, &call_args[i])) {
      return 0;
    }
  }
  const char *extern_name = NULL;
  IRFunction *callee = ii_token_function(
      machine, (unsigned long long)ii_as_int(&target), &extern_name);
  IRInterpValue call_result = ii_int_value(0);
  if (callee && callee->instruction_count > 0) {
    if (callee->name &&
        !ii_copy_aggregate_args(machine, frame, callee->name, call_args,
                                call_arg_count)) {
      return 0;
    }
    if (!ii_exec_function(machine, callee, call_args, call_arg_count,
                          &call_result)) {
      return 0;
    }
  } else if (extern_name) {
    machine->current_call_loc = insn->location;
    size_t buffers_before = machine->buffer_count;
    int handled = ii_extern_call(machine, extern_name, call_args,
                                 call_arg_count, &call_result);
    for (size_t bi = buffers_before; bi < machine->buffer_count; bi++) {
      machine->buffers[bi].alloc_line = insn->location.line;
    }
    if (handled < 0 || machine->status != IR_INTERP_OK) {
      if (machine->status == IR_INTERP_OK) {
        ii_fail(machine, IR_INTERP_TRAP, "extern call trap");
      }
      return 0;
    }
  } else {
    ii_fail(machine, IR_INTERP_TRAP, "indirect call to a non-function");
    return 0;
  }
  if (!ii_store_dest(machine, frame, &insn->dest, &call_result)) {
    return 0;
  }
  return 1;
  return 1;
}

/* `*addr <- value [size]`, including the block-copy form for anything wider
 * than a machine word. */
static int ii_op_store(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  unsigned long long addr;
  long long size;
  IRInterpValue value;
  if (!ii_fetch_addr(machine, frame, &insn->dest, &addr) ||
      !ii_fetch_int(machine, frame, &insn->rhs, &size) ||
      !ii_fetch(machine, frame, &insn->lhs, &value)) {
    return 0;
  }
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    /* Wider than a machine word: this is the block-copy form, where the
     * value operand is the SOURCE ADDRESS rather than a value (whole-struct
     * assignment, and the copy of an aggregate literal's constant image).
     * Both regions are checked before either is touched. */
    unsigned long long source = (unsigned long long)ii_as_int(&value);
    long long dest_offset = 0;
    long long source_offset = 0;
    IIBuffer *dest_buffer =
        ii_addr_to_buffer(machine, addr, size, &dest_offset);
    IIBuffer *source_buffer =
        ii_addr_to_buffer(machine, source, size, &source_offset);
    if (!dest_buffer || !source_buffer) {
      ii_fail(machine, IR_INTERP_TRAP,
              "block copy out of bounds / after free");
      return 0;
    }
    memmove(dest_buffer->data + dest_offset,
            source_buffer->data + source_offset, (size_t)size);
    return 1;
  }
  /* An aggregate at or below 8 bytes is stored by a WORD-SIZED store, because
   * the backend keeps its bytes in a register and the lvalue path declines the
   * whole-struct memcpy at that size. The interpreter holds every aggregate as
   * a buffer and hands out its ADDRESS, so both shapes wrote the low bytes of
   * an address: `smalls[1] = one`, whose value operand is the aggregate symbol
   * itself, and `smalls[2] = make_small(2, -2)`, whose value is the callee's
   * buffer marked escaped_local. Only the interpreter's own bookkeeping can
   * name either, so an ordinary pointer store is never taken for one. */
  if (!value.is_float && !insn->is_float && value.i != 0) {
    long long source_offset = 0;
    IIBuffer *source_buffer = NULL;
    int is_aggregate_source = 0;
    if (insn->lhs.kind == IR_OPERAND_SYMBOL && insn->lhs.name) {
      IIVar *var = ii_env_find(&frame->env, insn->lhs.name);
      if (!var) {
        var = ii_env_find(&machine->globals, insn->lhs.name);
      }
      is_aggregate_source = var && (long long)var->agg_size == size;
    }
    source_buffer = ii_addr_to_buffer(machine, (unsigned long long)value.i,
                                      size, &source_offset);
    if (source_buffer && (is_aggregate_source || source_buffer->escaped_local)) {
      long long dest_offset = 0;
      IIBuffer *dest_buffer =
          ii_addr_to_buffer(machine, addr, size, &dest_offset);
      if (!dest_buffer) {
        ii_fail(machine, IR_INTERP_TRAP,
                "block copy out of bounds / after free");
        return 0;
      }
      memmove(dest_buffer->data + dest_offset,
              source_buffer->data + source_offset, (size_t)size);
      /* A consumed aggregate return has no other reader; a named local does. */
      if (source_buffer->escaped_local && source_buffer != dest_buffer) {
        ii_reclaim_buffer(machine,
                          (size_t)((source_buffer->base - II_ADDR_BASE) /
                                   II_ADDR_STRIDE));
      }
      return 1;
    }
  }
  unsigned long long raw;
  if (value.is_float || insn->is_float) {
    if (size == 4) {
      float f = (float)ii_as_float(&value);
      unsigned int bits;
      memcpy(&bits, &f, 4);
      raw = bits;
    } else if (size == 8) {
      double d = ii_as_float(&value);
      memcpy(&raw, &d, 8);
    } else if (size == 2 && insn->alias_class == IR_ALIAS_CLASS_F16) {
      float f = (float)ii_as_float(&value);
      uint32_t b;
      memcpy(&b, &f, (size_t)4);
      raw = (unsigned long long)mettle_f32bits_to_f16bits(b);
    } else if (size == 2 && insn->alias_class == IR_ALIAS_CLASS_BF16) {
      float f = (float)ii_as_float(&value);
      uint32_t b;
      memcpy(&b, &f, (size_t)4);
      raw = (unsigned long long)mettle_f32bits_to_bf16bits(b);
    } else if (size == 2) {
      float f = (float)ii_as_float(&value);
      uint32_t b;
      memcpy(&b, &f, (size_t)4);
      raw = (unsigned long long)mettle_f32bits_to_f16bits(b);
    } else {
      raw = (unsigned long long)ii_as_int(&value);
    }
    /* An int value stored with an int-typed instruction keeps int bits. */
    if (!value.is_float && !insn->is_float) {
      raw = (unsigned long long)value.i;
    }
  } else {
    raw = (unsigned long long)value.i;
  }
  if (!ii_mem_write(machine, addr, (int)size, raw)) {
    return 0;
  }
  return 1;
  return 1;
}

/* Negation, complement and the not that yields 0 or 1. */
static int ii_op_unary(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  IRInterpValue a, out;
  if (!ii_fetch(machine, frame, &insn->lhs, &a)) {
    return 0;
  }
  const char *op = insn->text ? insn->text : "?";
  if (strcmp(op, "-") == 0) {
    if (insn->is_float || a.is_float) {
      double v = ii_as_float(&a);
      out = ii_float_value(insn->float_bits == 32 ? (double)(-(float)v) : -v);
    } else {
      out = ii_int_value((long long)(0ULL - (unsigned long long)a.i));
    }
  } else if (strcmp(op, "!") == 0) {
    out = ii_int_value(a.is_float ? a.f == 0.0 : a.i == 0);
  } else if (strcmp(op, "~") == 0) {
    out = ii_int_value(~ii_as_int(&a));
  } else if (strcmp(op, "+") == 0) {
    out = a;
  } else {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "unary op");
    return 0;
  }
  out.undefined = a.undefined;
  if (!ii_store_dest(machine, frame, &insn->dest, &out)) {
    return 0;
  }
  return 1;
  return 1;
}

/* `dest <- src` between a temp and a local, either way round. */
static int ii_op_assign(IRInterpMachine *machine, IIFrame *frame,
                         const IRInstruction *insn) {
  IRInterpValue value;
  if (insn->lhs.kind == IR_OPERAND_STRING && insn->lhs.name &&
      insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name) {
    IIVar *dest = ii_env_find(&frame->env, insn->dest.name);
    if (dest && dest->is_cstring) {
      unsigned long long chars = 0, record = 0;
      if (!ii_string_literal(machine, insn->lhs.name,
                             ir_operand_string_length(&insn->lhs), &chars,
                             &record)) {
        return 0;
      }
      value = ii_int_value((long long)chars);
      if (!ii_store_dest(machine, frame, &insn->dest, &value)) {
        return 0;
      }
      return 1;
    }
  }
  if (!ii_fetch(machine, frame, &insn->lhs, &value) ||
      !ii_store_dest(machine, frame, &insn->dest, &value)) {
    return 0;
  }
  return 1;
  return 1;
}

/* `%t <- *addr [size]`, widened the way the element type reads. */
static int ii_op_load(IRInterpMachine *machine, IIFrame *frame,
                       const IRInstruction *insn) {
  unsigned long long addr;
  long long size;
  if (!ii_fetch_addr(machine, frame, &insn->lhs, &addr) ||
      !ii_fetch_int(machine, frame, &insn->rhs, &size)) {
    return 0;
  }
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "load size");
    return 0;
  }
  unsigned long long raw;
  if (!ii_mem_read(machine, addr, (int)size, &raw)) {
    return 0;
  }
  IRInterpValue value;
  if (insn->is_float) {
    if (size == 4) {
      float f;
      unsigned int bits = (unsigned int)raw;
      memcpy(&f, &bits, 4);
      value = ii_float_value((double)f);
    } else if (size == 2 && insn->alias_class == IR_ALIAS_CLASS_BF16) {
      uint16_t h = (uint16_t)raw;
      value = ii_float_value((double)mettle_bf16bits_to_f32(h));
    } else if (size == 2) {
      uint16_t h = (uint16_t)raw;
      value = ii_float_value((double)mettle_f16bits_to_f32(h));
    } else {
      double d;
      memcpy(&d, &raw, 8);
      value = ii_float_value(d);
    }
  } else {
    value = ii_int_value(
        ii_widen_loaded_int(raw, (int)size, insn->is_unsigned));
  }
  value.undefined = machine->last_read_undefined;
  if (!ii_store_dest(machine, frame, &insn->dest, &value)) {
    return 0;
  }
  return 1;
  return 1;
}

/* `local @name : type`. The widest single opcode the interpreter has: it
 * decides between a slot-backed register home, real storage for anything whose
 * address is taken, and the string value convention, and it seeds each with
 * the poison a read-before-write has to be able to see. Its own function
 * because it was a third of ii_exec_function on its own.
 *
 * Returns 1 with the local established, 0 having failed the machine. The
 * caller advances pc either way it used to. */
/* Storage for a local that needs a real address: an array, or a scalar whose
 * address is taken. Poisoned the way a stack slot is, in contrast to heap
 * `new`, which stays zeroed to match HEAP_ZERO_MEMORY in codegen. A
 * re-executed declaration (a loop body's local) reuses the storage it already
 * owns and poisons it again, the way a reused stack slot behaves. */
static int ii_give_local_storage(IRInterpMachine *machine, IIFrame *frame,
                                 IIVar *var, long long count, int elem_size,
                                 int is_float, int is_unsigned) {
  unsigned long long addr = 0;
  if (var->has_local_storage && var->value.i) {
    long long offset = 0;
    IIBuffer *buf =
        ii_addr_to_buffer(machine, (unsigned long long)var->value.i,
                          count * elem_size, &offset);
    if (buf) {
      ii_mark_bytes(buf, offset, count * elem_size, 0);
      memset(buf->data + offset, II_POISON_BYTE, (size_t)(count * elem_size));
      addr = (unsigned long long)var->value.i;
    }
  }
  if (!addr) {
    addr = ii_add_buffer_ex(machine, NULL, count * elem_size, 1);
    if (!addr) {
      ii_fail(machine, IR_INTERP_TRAP, "local storage allocation");
      return 0;
    }
    if (!ii_frame_own(frame,
                      (size_t)((addr - II_ADDR_BASE) / II_ADDR_STRIDE))) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    memset(machine->buffers[(addr - II_ADDR_BASE) / II_ADDR_STRIDE].data,
           II_POISON_BYTE, (size_t)(count * elem_size));
    ii_mark_bytes(&machine->buffers[(addr - II_ADDR_BASE) / II_ADDR_STRIDE], 0,
                  count * elem_size, 0);
    var->has_local_storage = 1;
  }
  var->value = ii_int_value((long long)addr);
  var->slotted = count == 1; /* arrays are accessed via &, not by name */
  var->slot_size = elem_size;
  var->slot_is_float = is_float;
  var->slot_is_unsigned = is_unsigned;
  return 1;
}

/* An aggregate local: storage of the type's size, reached through address-of,
 * with whole-value assignment a block copy. Poisoned so a read before the
 * first write is visible as one. A frame re-entering its own declaration
 * reuses the storage it already owns. */
static int ii_declare_aggregate_local(IRInterpMachine *machine, IIFrame *frame,
                                      IIVar *var, long long agg_size) {
  if (var->has_local_storage && var->value.i) {
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(
        machine, (unsigned long long)var->value.i, agg_size, &offset);
    if (buf) {
      memset(buf->data + offset, II_POISON_BYTE, (size_t)agg_size);
      ii_mark_bytes(buf, offset, agg_size, 0);
      return 1;
    }
  }
  unsigned long long addr = ii_add_buffer_ex(machine, NULL, agg_size, 1);
  if (!addr) {
    ii_fail(machine, IR_INTERP_TRAP, "local storage allocation");
    return 0;
  }
  if (!ii_frame_own(frame,
                    (size_t)((addr - II_ADDR_BASE) / II_ADDR_STRIDE))) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    return 0;
  }
  memset(machine->buffers[(addr - II_ADDR_BASE) / II_ADDR_STRIDE].data,
         II_POISON_BYTE, (size_t)agg_size);
  ii_mark_bytes(&machine->buffers[(addr - II_ADDR_BASE) / II_ADDR_STRIDE], 0,
                agg_size, 0);
  var->value = ii_int_value((long long)addr);
  var->slotted = 0;
  var->agg_size = agg_size;
  var->has_local_storage = 1;
  return 1;
}

/* The string value convention: the local's value IS the address of a
 * { chars, length } record, and address-of yields that value. Never
 * slot-backed.
 *
 * The record has to exist from the declaration on. `var s: string;`
 * followed by `s.chars = buf` stores through the local's value, and
 * leaving that value poisoned made the store land nowhere: read_line and
 * read_line_stdin -- which every program importing std/io carries -- trapped
 * on every generated input, and 250-odd files reported them as unvalidated.
 * A declaration with no initializer gives them a zeroed record, which is the
 * storage the compiled program gets. An initializer overwrites the value
 * with its own record's address on the next instruction, exactly as before. */
static int ii_declare_string_local(IRInterpMachine *machine, IIFrame *frame,
                                   IIVar *var) {
  var->slotted = 0;
  var->agg_size = 0;
  var->value_size = 8;
  var->value_is_unsigned = 1;
  if (var->string_record) {
    long long offset = 0;
    IIBuffer *buf =
        ii_addr_to_buffer(machine, var->string_record, 16, &offset);
    if (buf) {
      memset(buf->data + offset, 0, 16);
      ii_mark_bytes(buf, offset, 16, 1);
      var->value = ii_int_value((long long)var->string_record);
      return 1;
    }
  }
  {
    unsigned long long addr = ii_add_buffer_ex(machine, NULL, 16, 1);
    if (!addr) {
      ii_fail(machine, IR_INTERP_TRAP, "string record allocation");
      return 0;
    }
    if (!ii_frame_own(frame, (size_t)((addr - II_ADDR_BASE) /
                                       II_ADDR_STRIDE))) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    memset(machine->buffers[(addr - II_ADDR_BASE) / II_ADDR_STRIDE].data,
           0, 16);
    var->value = ii_int_value((long long)addr);
    var->has_local_storage = 1;
    var->string_record = addr;
  }
  return 1;
}

static int ii_op_declare_local(IRInterpMachine *machine, IIFrame *frame,
                               IRFunction *fn, const IRInstruction *insn) {
  if (insn->dest.kind != IR_OPERAND_SYMBOL || !insn->dest.name) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "local declaration form");
    return 0;
  }
  IIVar *var = ii_env_upsert(&frame->env, insn->dest.name);
  if (!var) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    return 0;
  }
  const MtlcType *vt = insn->value_type;
  if (!vt && insn->text && machine->program) {
    vt = ir_program_lookup_type(machine->program, insn->text);
  }
  if ((insn->text && strcmp(insn->text, "string") == 0) ||
      (vt && vt->kind == MTLC_TYPE_STRING)) {
    return ii_declare_string_local(machine, frame, var);
  }
  int elem_size = 8, is_float = 0, is_unsigned = 0;
  long long count = 1;
  int parsed = ii_parse_local_type(insn->text, &elem_size, &count,
                                   &is_float, &is_unsigned);
  var->is_cstring = (insn->text && (strcmp(insn->text, "cstring") == 0 ||
                                    strcmp(insn->text, "rawptr") == 0))
                        ? 1
                        : 0;
  if (!parsed && vt &&
      ii_scalar_from_mtlc(vt, &elem_size, &is_float, &is_unsigned)) {
    /* Enum, pointer, or closure local behind a named type. */
    parsed = 1;
  }
  if (!parsed && vt &&
      (vt->kind == MTLC_TYPE_STRUCT || vt->kind == MTLC_TYPE_ARRAY ||
       vt->kind == MTLC_TYPE_TAGGED_ENUM) &&
      vt->size > 0) {
    return ii_declare_aggregate_local(machine, frame, var,
                                      (long long)vt->size);
  }
  if (!parsed) {
    char what[96];
    snprintf(what, sizeof(what), "local type '%s'",
             insn->text ? insn->text : "?");
    ii_fail(machine, IR_INTERP_UNSUPPORTED, what);
    return 0;
  }
  /* Arrays always get storage; scalars only when their address is
   * taken somewhere in the function (aliasing must be observable). */
  int need_slot = count > 1;
  if (!need_slot) {
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *scan = &fn->instructions[i];
      if (scan->op == IR_OP_ADDRESS_OF &&
          scan->lhs.kind == IR_OPERAND_SYMBOL && scan->lhs.name &&
          strcmp(scan->lhs.name, insn->dest.name) == 0) {
        need_slot = 1;
        break;
      }
    }
  }
  if (need_slot) {
    if (!ii_give_local_storage(machine, frame, var, count, elem_size, is_float,
                               is_unsigned)) {
      return 0;
    }
    var->slot_alias = 0;
    if (insn->text && strncmp(insn->text, "bfloat16", 8) == 0) {
      var->slot_alias = IR_ALIAS_CLASS_BF16;
    } else if (insn->text && strncmp(insn->text, "float16", 7) == 0) {
      var->slot_alias = IR_ALIAS_CLASS_F16;
    } else if (vt && vt->kind == MTLC_TYPE_BFLOAT16) {
      var->slot_alias = IR_ALIAS_CLASS_BF16;
    } else if (vt && vt->kind == MTLC_TYPE_FLOAT16) {
      var->slot_alias = IR_ALIAS_CLASS_F16;
    }
  } else {
    var->value = ii_poison_value();
    var->slotted = 0;
    if (is_float) {
      var->value.is_float = 1;
      var->value.f = -6510615.5; /* deterministic float poison */
      var->value.i = 0;
    }
  }
  /* Remember the declared integer width even when the local lives in a
   * "register" here, so a write wraps at the width the program asked for.
   * Pointers are already 8 bytes; a float32 home rounds writes to single
   * precision (the -4 marker). */
  var->value_size = is_float ? (elem_size == 4 ? -4 : 0) : elem_size;
  var->value_is_unsigned = is_unsigned;
  return 1;
}

/* Every label in the function, so a jump resolves by name in one lookup
 * rather than a scan. Built once per frame. */
static int ii_build_label_table(IRInterpMachine *machine, IIFrame *frame,
                                 IRFunction *fn) {
  size_t label_capacity = 8;
  frame->labels = (IILabel *)malloc(label_capacity * sizeof(IILabel));
  if (!frame->labels) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    return 0;
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (insn->op == IR_OP_LABEL && insn->text) {
      if (frame->label_count >= label_capacity) {
        label_capacity *= 2;
        IILabel *grown =
            (IILabel *)realloc(frame->labels, label_capacity * sizeof(IILabel));
        if (!grown) {
          ii_fail(machine, IR_INTERP_TRAP, "out of memory");
          return 0;
        }
        frame->labels = grown;
      }
      frame->labels[frame->label_count].label = insn->text;
      frame->labels[frame->label_count].index = i;
      frame->label_count++;
    }
  }
  return 1;
}

/* A parameter whose address is taken needs real storage, not a register
 * home: give it a slot and move the incoming value into it, so a write
 * through the pointer and a read by name see the same bytes. */
static int ii_home_addressed_parameters(IRInterpMachine *machine, IIFrame *frame,
                                         IRFunction *fn) {
  /* A scalar parameter whose address is taken gets a slot home now, so &p is
   * a real address the way the backend homes it (before this, &p handed back
   * the parameter's VALUE as an address). String and aggregate parameters
   * stay register-resident: their values already ARE the addresses &p means. */
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *scan = &fn->instructions[i];
    if (scan->op != IR_OP_ADDRESS_OF || scan->lhs.kind != IR_OPERAND_SYMBOL ||
        !scan->lhs.name) {
      continue;
    }
    IIVar *var = ii_env_find(&frame->env, scan->lhs.name);
    if (!var || var->slotted) {
      continue; /* only parameters live in the frame this early */
    }
    const char *ptype = NULL;
    for (size_t p = 0; p < fn->parameter_count; p++) {
      if (fn->parameter_names[p] &&
          strcmp(fn->parameter_names[p], scan->lhs.name) == 0) {
        ptype = fn->parameter_types ? fn->parameter_types[p] : NULL;
        break;
      }
    }
    int psize = 8, pfloat = 0, punsigned = 0;
    long long pcount = 1;
    if (!ptype || strcmp(ptype, "string") == 0 ||
        !ii_parse_local_type(ptype, &psize, &pcount, &pfloat, &punsigned) ||
        pcount != 1) {
      continue;
    }
    unsigned long long addr = ii_add_buffer_ex(machine, NULL, psize, 1);
    if (!addr ||
        !ii_frame_own(frame, (size_t)((addr - II_ADDR_BASE) /
                                       II_ADDR_STRIDE))) {
      ii_fail(machine, IR_INTERP_TRAP, "parameter home allocation");
      return 0;
    }
    IRInterpValue incoming = var->value;
    var->slotted = 1;
    var->slot_size = psize;
    var->slot_is_float = pfloat;
    var->slot_is_unsigned = punsigned;
    var->slot_alias = 0;
    if (ptype && strncmp(ptype, "bfloat16", 8) == 0) {
      var->slot_alias = IR_ALIAS_CLASS_BF16;
    } else if (ptype && strncmp(ptype, "float16", 7) == 0) {
      var->slot_alias = IR_ALIAS_CLASS_F16;
    }
    var->has_local_storage = 1;
    var->value = ii_int_value((long long)addr);
    if (!ii_var_write(machine, var, &incoming)) {
      return 0;
    }
  }
  return 1;
}

/* Bind each parameter into the frame, narrowed to its declared width the way
 * the callee's home for it is. A parameter reassigned in the body wraps there
 * too. A string parameter gets its own copy of the 16-byte record, owned by
 * the frame, so a callee writing through it cannot reach the caller's. */
static int ii_bind_parameters(IRInterpMachine *machine, IIFrame *frame,
                              IRFunction *fn, const IRInterpValue *args,
                              size_t arg_count) {
  for (size_t i = 0; i < fn->parameter_count && i < arg_count; i++) {
    IIVar *var = ii_env_upsert(&frame->env, fn->parameter_names[i]);
    if (!var) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    {
      int psize = 8, pfloat = 0, punsigned = 0;
      long long pcount = 1;
      const char *ptype = fn->parameter_types ? fn->parameter_types[i] : NULL;
      if (ptype && ii_parse_local_type(ptype, &psize, &pcount, &pfloat,
                                       &punsigned) &&
          pcount == 1) {
        if (!pfloat) {
          var->value_size = psize;
          var->value_is_unsigned = punsigned;
        } else if (psize == 4) {
          /* A float32 parameter's home is 4 bytes: round the incoming value
           * to single precision the way the ABI transfer does. */
          var->value_size = -4;
        }
      }
    }
    var->value = args[i];
    /* An aggregate parameter holds the ADDRESS of the caller's copy, the same
     * as an aggregate local, so record its size. Without it a word-sized store
     * of the parameter -- which is how a closure constructor writes a captured
     * struct into its environment -- wrote the low bytes of that address
     * instead of the struct. A closure capturing a two-int32 struct read back
     * as pointer bits under `mettle test` while the backend had it right. */
    if (fn->parameter_types && fn->parameter_types[i] && machine->program) {
      const MtlcType *pt =
          ir_program_lookup_type(machine->program, fn->parameter_types[i]);
      if (pt && pt->size > 0 &&
          (pt->kind == MTLC_TYPE_STRUCT || pt->kind == MTLC_TYPE_ARRAY ||
           pt->kind == MTLC_TYPE_TAGGED_ENUM)) {
        var->agg_size = (long long)pt->size;
      }
    }
    if (fn->parameter_types && fn->parameter_types[i] &&
        strcmp(fn->parameter_types[i], "string") == 0 &&
        !var->value.is_float && var->value.i != 0) {
      long long src_off = 0;
      IIBuffer *src = ii_addr_to_buffer(
          machine, (unsigned long long)var->value.i, 16, &src_off);
      if (src) {
        unsigned long long copy = ii_add_buffer_ex(machine, NULL, 16, 1);
        if (!copy ||
            !ii_frame_own(frame,
                          (size_t)((copy - II_ADDR_BASE) / II_ADDR_STRIDE))) {
          ii_fail(machine, IR_INTERP_TRAP, "string parameter copy");
          return 0;
        }
        memcpy(machine->buffers[(copy - II_ADDR_BASE) / II_ADDR_STRIDE].data,
               src->data + src_off, 16);
        var->value = ii_int_value((long long)copy);
        var->has_local_storage = 1;
      }
    }
    if (!var->value.is_float && var->value_size > 0 && var->value_size < 8) {
      var->value.i =
          ii_narrow_int(var->value.i, var->value_size, var->value_is_unsigned);
    }
    if (var->value.is_float && var->value_size == -4) {
      var->value.f = (double)(float)var->value.f;
    }
  }
  return 1;
}

static int ii_exec_function(IRInterpMachine *machine, IRFunction *fn,
                            const IRInterpValue *args, size_t arg_count,
                            IRInterpValue *result) {
  if (machine->depth >= II_MAX_DEPTH) {
    ii_fail(machine, IR_INTERP_DEPTH, fn->name ? fn->name : "?");
    return 0;
  }
  machine->depth++;

  IIFrame frame;
  memset(&frame, 0, sizeof(frame));
  frame.fn = fn;

  int ok = 0;

  if (!ii_bind_parameters(machine, &frame, fn, args, arg_count)) {
    goto done;
  }

  if (!ii_build_label_table(machine, &frame, fn)) {
    goto done;
  }
  if (!ii_home_addressed_parameters(machine, &frame, fn)) {
    goto done;
  }

  long long *exec_counts = ii_counts_for(machine, fn);

  size_t pc = 0;
  while (pc < fn->instruction_count) {
    if (--machine->fuel < 0) {
      ii_fail(machine, IR_INTERP_FUEL, fn->name ? fn->name : "?");
      goto done;
    }
    const IRInstruction *insn = &fn->instructions[pc];
    size_t executed_pc = pc;

    switch (insn->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
      pc++;
      break;

    case IR_OP_JUMP: {
      const IILabel *label = insn->text ? ii_find_label(&frame, insn->text) : NULL;
      if (!label) {
        ii_fail(machine, IR_INTERP_TRAP, "jump to unknown label");
        goto done;
      }
      pc = label->index;
      break;
    }

    case IR_OP_BRANCH_ZERO: {
      IRInterpValue cond;
      if (!ii_fetch(machine, &frame, &insn->lhs, &cond)) {
        goto done;
      }
      long long v = cond.is_float ? (cond.f != 0.0) : cond.i;
      if (cond.undefined) {
        machine->branched_on_undefined = 1;
      }
      if (v == 0) {
        const IILabel *label = insn->text ? ii_find_label(&frame, insn->text) : NULL;
        if (!label) {
          ii_fail(machine, IR_INTERP_TRAP, "branch to unknown label");
          goto done;
        }
        pc = label->index;
      } else {
        pc++;
      }
      break;
    }

    case IR_OP_BRANCH_EQ: {
      IRInterpValue a, b;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_fetch(machine, &frame, &insn->rhs, &b)) {
        goto done;
      }
      int equal;
      if (a.is_float || b.is_float) {
        equal = ii_as_float(&a) == ii_as_float(&b);
      } else {
        equal = a.i == b.i;
      }
      if (a.undefined || b.undefined) {
        machine->branched_on_undefined = 1;
      }
      if (equal) {
        const IILabel *label = insn->text ? ii_find_label(&frame, insn->text) : NULL;
        if (!label) {
          ii_fail(machine, IR_INTERP_TRAP, "branch to unknown label");
          goto done;
        }
        pc = label->index;
      } else {
        pc++;
      }
      break;
    }

    case IR_OP_DECLARE_LOCAL: {
      if (!ii_op_declare_local(machine, &frame, fn, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_ASSIGN: {
      if (!ii_op_assign(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_ADDRESS_OF: {
      if (insn->lhs.kind != IR_OPERAND_SYMBOL || !insn->lhs.name) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "address-of non-symbol");
        goto done;
      }
      IIVar *var = ii_env_find(&frame.env, insn->lhs.name);
      long long base = 0;
      if (var) {
        /* Slot-backed local or array: value.i is the base address. A string
         * or aggregate parameter's VALUE is the address &p means. */
        base = var->value.i;
      } else {
        /* Function: a deterministic address token an indirect call maps
         * back. Global: materialize its storage. */
        unsigned long long token =
            ii_function_token(machine, insn->lhs.name);
        if (!token) {
          token = ii_global_storage(machine, insn->lhs.name);
        }
        if (!token) {
          if (machine->status == IR_INTERP_OK) {
            ii_fail(machine, IR_INTERP_UNSUPPORTED, "address-of global");
          }
          goto done;
        }
        base = (long long)token;
      }
      IRInterpValue addr = ii_int_value(base);
      if (!ii_store_dest(machine, &frame, &insn->dest, &addr)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_LOAD: {
      if (!ii_op_load(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_STORE: {
      if (!ii_op_store(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_BINARY: {
      IRInterpValue a, b, out;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_fetch(machine, &frame, &insn->rhs, &b) ||
          !ii_binary(machine, insn, &a, &b, &out)) {
        goto done;
      }
      out.undefined = a.undefined || b.undefined;
      if (!ii_store_dest(machine, &frame, &insn->dest, &out)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_UNARY: {
      if (!ii_op_unary(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_ROTATE_ADD: {
      /* next = a + b; a = b; b = next  (dest=next, lhs=a, rhs=b) */
      IRInterpValue a, b;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_fetch(machine, &frame, &insn->rhs, &b)) {
        goto done;
      }
      IRInterpValue next = ii_int_value(
          (long long)((unsigned long long)ii_as_int(&a) +
                      (unsigned long long)ii_as_int(&b)));
      if (!ii_store_dest(machine, &frame, &insn->dest, &next) ||
          !ii_store_dest(machine, &frame, &insn->lhs, &b) ||
          !ii_store_dest(machine, &frame, &insn->rhs, &next)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_CAST: {
      IRInterpValue a, out;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_cast(machine, insn, &a, &out)) {
        goto done;
      }
      out.undefined = a.undefined;
      if (!ii_store_dest(machine, &frame, &insn->dest, &out)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_SELECT: {
      if (!ii_op_select(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_NEW: {
      if (!ii_op_new(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_CALL: {
      if (!ii_op_call(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_RETURN: {
      IRInterpValue value = ii_int_value(0);
      if (insn->lhs.kind != IR_OPERAND_NONE &&
          !ii_fetch(machine, &frame, &insn->lhs, &value)) {
        goto done;
      }
      {
        int rsize = 8, rfloat = 0, runsigned = 0;
        long long rcount = 1;
        if (fn->return_type_name &&
            ii_parse_local_type(fn->return_type_name, &rsize, &rcount, &rfloat,
                                &runsigned) &&
            rcount == 1) {
          if (!rfloat && !value.is_float && rsize > 0 && rsize < 8) {
            value.i = ii_narrow_int(value.i, rsize, runsigned);
          } else if (rfloat && rsize == 4 && value.is_float) {
            value.f = (double)(float)value.f;
          }
        }
      }
      *result = value;
      ok = 1;
      goto done;
    }

    case IR_OP_CALL_INDIRECT: {
      if (!ii_op_call_indirect(machine, &frame, insn)) {
        goto done;
      }
      pc++;
      break;
    }
    case IR_OP_INLINE_ASM:
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "inline_asm");
      goto done;

    default:
      if (!ii_exec_simd(machine, &frame, insn)) {
        goto done;
      }
      if (machine->fuel < 0) {
        ii_fail(machine, IR_INTERP_FUEL, fn->name ? fn->name : "?");
        goto done;
      }
      pc++;
      break;
    }

    if (exec_counts) {
      exec_counts[executed_pc]++;
    }

    /* Value tracing: report the executed instruction's named result. */
    if (machine->value_hook && frame.fn == machine->value_hook_fn &&
        insn->location.line > 0 &&
        (insn->dest.kind == IR_OPERAND_TEMP ||
         insn->dest.kind == IR_OPERAND_SYMBOL) &&
        insn->dest.name) {
      switch (insn->op) {
      case IR_OP_ASSIGN:
      case IR_OP_BINARY:
      case IR_OP_UNARY:
      case IR_OP_CAST:
      case IR_OP_LOAD:
      case IR_OP_CALL:
      case IR_OP_ROTATE_ADD:
      case IR_OP_NEW: {
        IIVar *var = ii_env_find(&frame.env, insn->dest.name);
        if (!var && insn->dest.kind == IR_OPERAND_SYMBOL) {
          var = ii_env_find(&machine->globals, insn->dest.name);
        }
        IRInterpValue value;
        if (var && ii_var_read(machine, var, &value)) {
          machine->value_hook(machine->value_hook_ctx, insn->location.line,
                              insn->dest.name, value, insn->expansion_note);
        }
        break;
      }
      default:
        break;
      }
    }
  }

  /* Fell off the end: void return. */
  *result = ii_int_value(0);
  ok = 1;

done:
  /* This frame's local storage dies with it, except a buffer the function
   * returned the address of: an aggregate return travels that way and stays
   * alive until the caller's aggregate assignment consumes it. */
  {
    unsigned long long kept =
        (ok && !result->is_float) ? (unsigned long long)result->i : 0;
    for (size_t i = 0; i < frame.owned_count; i++) {
      size_t index = frame.owned[i];
      IIBuffer *buf = &machine->buffers[index];
      if (buf->freed) {
        continue;
      }
      if (kept && buf->base == kept) {
        buf->escaped_local = 1;
        continue;
      }
      ii_reclaim_buffer(machine, index);
    }
  }
  free(frame.owned);
  free(frame.labels);
  ii_env_free(&frame.env);
  machine->depth--;
  return ok;
}

IRInterpStatus ir_interp_run(IRInterpMachine *machine, IRFunction *function,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result, long long fuel) {
  if (!machine || !function || !result) {
    return IR_INTERP_UNSUPPORTED;
  }
  machine->status = IR_INTERP_OK;
  machine->detail[0] = '\0';
  machine->fuel = fuel;
  machine->depth = 0;
  machine->assert_failed = 0;
  IRInterpValue local_result = ii_int_value(0);
  int ok = ii_exec_function(machine, function, args, arg_count, &local_result);
  if (ok && machine->status == IR_INTERP_OK) {
    *result = local_result;
    return IR_INTERP_OK;
  }
  return machine->status == IR_INTERP_OK ? IR_INTERP_TRAP : machine->status;
}

/* ---------------- observation accessors ---------------- */

size_t ir_interp_buffer_count(const IRInterpMachine *machine) {
  return machine ? machine->buffer_count : 0;
}

const unsigned char *ir_interp_buffer_data(const IRInterpMachine *machine,
                                           size_t index, long long *size) {
  if (!machine || index >= machine->buffer_count) {
    return NULL;
  }
  if (size) {
    *size = machine->buffers[index].size;
  }
  return machine->buffers[index].data;
}

int ir_interp_branched_on_undefined(const IRInterpMachine *machine) {
  return machine ? machine->branched_on_undefined : 0;
}

size_t ir_interp_extern_trace_count(const IRInterpMachine *machine) {
  return machine ? machine->trace_count : 0;
}

const IRInterpExternCall *ir_interp_extern_trace(const IRInterpMachine *machine,
                                                 size_t index) {
  if (!machine || index >= machine->trace_count) {
    return NULL;
  }
  return &machine->trace[index];
}

size_t ir_interp_global_count(const IRInterpMachine *machine) {
  return machine ? machine->globals.capacity : 0;
}

const char *ir_interp_global_name(const IRInterpMachine *machine,
                                  size_t index) {
  if (!machine || index >= machine->globals.capacity) {
    return NULL;
  }
  return machine->globals.vars[index].key;
}

IRInterpValue ir_interp_global_value(const IRInterpMachine *machine,
                                     size_t index) {
  IRInterpValue zero = {0, 0, 0};
  if (!machine || index >= machine->globals.capacity ||
      !machine->globals.vars[index].key) {
    return zero;
  }
  return machine->globals.vars[index].value;
}

const char *ir_interp_status_detail(const IRInterpMachine *machine) {
  return machine ? machine->detail : "";
}

int ir_interp_assert_info(const IRInterpMachine *machine, size_t *line,
                          size_t *column, IRInterpValue *left,
                          IRInterpValue *right, int *is_eq) {
  if (!machine || !machine->assert_failed) {
    return 0;
  }
  if (line) *line = machine->assert_line;
  if (column) *column = machine->assert_column;
  if (left) *left = machine->assert_left;
  if (right) *right = machine->assert_right;
  if (is_eq) *is_eq = machine->assert_is_eq;
  return 1;
}

size_t ir_interp_buffer_alloc_line(const IRInterpMachine *machine,
                                   size_t index) {
  if (!machine || index >= machine->buffer_count) {
    return 0;
  }
  return machine->buffers[index].alloc_line;
}

int ir_interp_buffer_freed(const IRInterpMachine *machine, size_t index) {
  if (!machine || index >= machine->buffer_count) {
    return 1;
  }
  return machine->buffers[index].freed;
}

void ir_interp_set_value_hook(IRInterpMachine *machine, IRInterpValueHook hook,
                              void *ctx, const IRFunction *only_in) {
  if (!machine) {
    return;
  }
  machine->value_hook = hook;
  machine->value_hook_ctx = ctx;
  machine->value_hook_fn = only_in;
}
