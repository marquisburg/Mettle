/* mtlc_type_from_frontend.c - translate the Mettle frontend's Type into MtlcType.
 *
 * FRONTEND-side adapter (compiles into the mettle driver, NOT into libmtlc). This
 * is the single translation unit permitted to include both the frontend `Type`
 * (semantic/symbol_table.h) and the backend `MtlcType` (mtlc/type.h). Keeping the
 * translation here is what lets libmtlc stay free of any frontend's type system.
 *
 * The mapping is structural and 1:1 on the kind enum (TypeKind and MtlcTypeKind
 * intentionally share order and membership). Results are memoized by frontend
 * Type pointer so shared/recursive types translate once and cycles terminate. */
#include "frontend/mtlc_frontend.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Process-lifetime memo of (frontend Type* -> backend MtlcType*). A one-shot
 * compile never frees these; the arena leaks intentionally at exit.
 *
 * Open-addressing hash keyed on the frontend Type pointer. The memo used to be
 * a linear array scanned per call; a frontend that allocates a Type per
 * declaration pushes the memo into the hundreds of thousands, and the scan made
 * translation quadratic across a module's globals. */
typedef struct {
  const Type *from; /* NULL = empty slot */
  MtlcType *to;
} TypeMemoEntry;

static TypeMemoEntry *g_memo = NULL;
static size_t g_memo_count = 0;
static size_t g_memo_slots = 0; /* power of two */

static size_t memo_hash(const Type *from) {
  /* Fibonacci hashing spreads pointer-aligned keys across the table. */
  return (size_t)(((uintptr_t)from >> 4) * (uintptr_t)11400714819323198485ull);
}

static MtlcType *memo_lookup(const Type *from) {
  if (!g_memo) {
    return NULL;
  }
  size_t mask = g_memo_slots - 1;
  size_t h = memo_hash(from) & mask;
  while (g_memo[h].from) {
    if (g_memo[h].from == from) {
      return g_memo[h].to;
    }
    h = (h + 1) & mask;
  }
  return NULL;
}

static void memo_insert(const Type *from, MtlcType *to) {
  if (g_memo_count * 2 >= g_memo_slots) {
    size_t next = g_memo_slots ? g_memo_slots * 2 : 256;
    TypeMemoEntry *grown =
        (TypeMemoEntry *)calloc(next, sizeof(TypeMemoEntry));
    if (!grown) {
      return; /* out of memory: skip memoization; translation still proceeds */
    }
    for (size_t i = 0; i < g_memo_slots; i++) {
      if (!g_memo[i].from) {
        continue;
      }
      size_t h = memo_hash(g_memo[i].from) & (next - 1);
      while (grown[h].from) {
        h = (h + 1) & (next - 1);
      }
      grown[h] = g_memo[i];
    }
    free(g_memo);
    g_memo = grown;
    g_memo_slots = next;
  }
  size_t mask = g_memo_slots - 1;
  size_t h = memo_hash(from) & mask;
  while (g_memo[h].from && g_memo[h].from != from) {
    h = (h + 1) & mask;
  }
  if (!g_memo[h].from) {
    g_memo_count++;
  }
  g_memo[h].from = from;
  g_memo[h].to = to;
}

static MtlcTypeKind translate_kind(TypeKind kind) {
  switch (kind) {
  case TYPE_INT8:
    return MTLC_TYPE_INT8;
  case TYPE_INT16:
    return MTLC_TYPE_INT16;
  case TYPE_INT32:
    return MTLC_TYPE_INT32;
  case TYPE_INT64:
    return MTLC_TYPE_INT64;
  case TYPE_UINT8:
    return MTLC_TYPE_UINT8;
  case TYPE_UINT16:
    return MTLC_TYPE_UINT16;
  case TYPE_UINT32:
    return MTLC_TYPE_UINT32;
  case TYPE_UINT64:
    return MTLC_TYPE_UINT64;
  case TYPE_BOOL:
    return MTLC_TYPE_BOOL;
  /* A char is a byte below this line. Everything the backend does with one --
   * its load and store width, its register class, how it crosses an ABI
   * boundary -- is what it does with a uint8; only interpolation, which is
   * decided in the frontend, tells them apart. */
  case TYPE_CHAR:
    return MTLC_TYPE_UINT8;
  case TYPE_FLOAT32:
    return MTLC_TYPE_FLOAT32;
  case TYPE_FLOAT64:
    return MTLC_TYPE_FLOAT64;
  case TYPE_FLOAT16:
    return MTLC_TYPE_FLOAT16;
  case TYPE_BFLOAT16:
    return MTLC_TYPE_BFLOAT16;
  case TYPE_STRING:
    return MTLC_TYPE_STRING;
  case TYPE_FUNCTION_POINTER:
    return MTLC_TYPE_FUNCTION_POINTER;
  case TYPE_POINTER:
    return MTLC_TYPE_POINTER;
  case TYPE_ARRAY:
    return MTLC_TYPE_ARRAY;
  case TYPE_STRUCT:
    return MTLC_TYPE_STRUCT;
  case TYPE_ENUM:
    return MTLC_TYPE_ENUM;
  case TYPE_TAGGED_ENUM:
    return MTLC_TYPE_TAGGED_ENUM;
  case TYPE_VOID:
    return MTLC_TYPE_VOID;
  case TYPE_SLICE:
    /* Frontend slice descriptor; backend sees the 16-byte {ptr,len} image
     * via the frontend-computed size, not a distinct MtlcTypeKind. */
    return MTLC_TYPE_STRUCT;
  case TYPE_TYPE:
  case TYPE_FIELD:
    /* Frontend-only; these have no backend representation. */
    return MTLC_TYPE_VOID;
  }
  return MTLC_TYPE_VOID;
}

static MtlcAddressSpace translate_space(unsigned char space) {
  switch (space) {
  case DEVICE_SPACE_GENERIC:
    return MTLC_ADDRESS_SPACE_GENERIC;
  case DEVICE_SPACE_GLOBAL:
    return MTLC_ADDRESS_SPACE_GLOBAL;
  case DEVICE_SPACE_SHARED:
    return MTLC_ADDRESS_SPACE_WORKGROUP;
  case DEVICE_SPACE_CONSTANT:
    return MTLC_ADDRESS_SPACE_CONSTANT;
  case DEVICE_SPACE_LOCAL:
    return MTLC_ADDRESS_SPACE_PRIVATE;
  default:
    return MTLC_ADDRESS_SPACE_DEFAULT;
  }
}

static MtlcViewLayout translate_layout(unsigned char layout) {
  switch (layout) {
  case VIEW_LAYOUT_ROW:
    return MTLC_VIEW_LAYOUT_ROW;
  case VIEW_LAYOUT_COL:
    return MTLC_VIEW_LAYOUT_COL;
  case VIEW_LAYOUT_SWIZZLE64:
    return MTLC_VIEW_LAYOUT_SWIZZLE64;
  case VIEW_LAYOUT_SWIZZLE128:
    return MTLC_VIEW_LAYOUT_SWIZZLE128;
  case VIEW_LAYOUT_INTERLEAVE:
    return MTLC_VIEW_LAYOUT_INTERLEAVE;
  case VIEW_LAYOUT_FRAGMENT_A:
    return MTLC_VIEW_LAYOUT_FRAGMENT_A;
  case VIEW_LAYOUT_FRAGMENT_B:
    return MTLC_VIEW_LAYOUT_FRAGMENT_B;
  case VIEW_LAYOUT_FRAGMENT_C:
    return MTLC_VIEW_LAYOUT_FRAGMENT_C;
  default:
    return MTLC_VIEW_LAYOUT_NONE;
  }
}

/* Duplicate an array of frontend Type* into an array of translated MtlcType*. */
static MtlcType **translate_type_array(struct Type **in, size_t count) {
  if (!in || count == 0) {
    return NULL;
  }
  MtlcType **out = (MtlcType **)calloc(count, sizeof(MtlcType *));
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    out[i] = mtlc_type_from_frontend(in[i]);
  }
  return out;
}

MtlcType *mtlc_type_from_frontend(const Type *type) {
  if (!type) {
    return NULL;
  }
  MtlcType *existing = memo_lookup(type);
  if (existing) {
    return existing;
  }

  MtlcType *out = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!out) {
    return NULL;
  }
  /* Register BEFORE recursing so a type reachable from itself resolves to this
   * same node instead of recursing forever. */
  memo_insert(type, out);

  out->kind = translate_kind(type->kind);
  out->name = type->name; /* borrow the frontend's interned name */
  out->size = type->size;
  out->alignment = type->alignment;
  out->array_size = type->array_size;
  out->address_space = translate_space(type->device_space);
  out->pointee_align = type->declared_align;
  out->view_layout = translate_layout(type->view_layout);
  out->view_layout_param = type->view_layout_param;
  for (size_t e = 0; e < 4; e++) {
    out->view_extents[e] = type->view_extents[e];
    if (type->view_extents[e]) {
      out->view_extent_count = (unsigned)(e + 1);
    }
  }

  out->base_type = mtlc_type_from_frontend(type->base_type);
  out->fn_return_type = mtlc_type_from_frontend(type->fn_return_type);
  out->closure_env = mtlc_type_from_frontend(type->closure_env);
  out->fn_param_count = type->fn_param_count;
  out->fn_param_types =
      translate_type_array(type->fn_param_types, type->fn_param_count);

  /* Struct layout: names/offsets are borrowed; field types are translated. */
  out->field_count = type->field_count;
  out->field_names = (const char **)type->field_names;
  out->field_offsets = type->field_offsets;
  out->field_types =
      translate_type_array(type->field_types, type->field_count);

  /* Tagged-enum layout. */
  out->tagged_variant_count = type->tagged_variant_count;
  out->tagged_variant_names = (const char **)type->tagged_variant_names;
  out->tagged_variant_tags = type->tagged_variant_tags;
  out->tagged_variant_payloads = translate_type_array(
      type->tagged_variant_payloads, type->tagged_variant_count);
  out->tagged_data_offset = type->tagged_data_offset;
  out->tagged_data_size = type->tagged_data_size;

  return out;
}
