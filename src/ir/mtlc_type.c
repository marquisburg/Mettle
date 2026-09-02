/* mtlc_type.c - queries over the backend-owned type descriptor (mtlc/type.h).
 *
 * Part of libmtlc. Deliberately frontend-free: it knows nothing about how a
 * frontend's types were translated into MtlcType, only how to answer the
 * classification questions the code generators ask. */
#include "mtlc/type.h"

/* Immortal canonical singletons for the scalar/primitive kinds. Fully static so
 * the pointers are valid for the process lifetime with no ownership concerns --
 * exactly what a frontend building IR through mtlc/build.h needs. */
#define MTLC_SCALAR(k, nm, sz, al)                                             \
  {.kind = (k), .name = (nm), .size = (sz), .alignment = (al)}
static const MtlcType k_scalar_int8 = MTLC_SCALAR(MTLC_TYPE_INT8, "int8", 1, 1);
static const MtlcType k_scalar_int16 = MTLC_SCALAR(MTLC_TYPE_INT16, "int16", 2, 2);
static const MtlcType k_scalar_int32 = MTLC_SCALAR(MTLC_TYPE_INT32, "int32", 4, 4);
static const MtlcType k_scalar_int64 = MTLC_SCALAR(MTLC_TYPE_INT64, "int64", 8, 8);
static const MtlcType k_scalar_uint8 = MTLC_SCALAR(MTLC_TYPE_UINT8, "uint8", 1, 1);
static const MtlcType k_scalar_uint16 = MTLC_SCALAR(MTLC_TYPE_UINT16, "uint16", 2, 2);
static const MtlcType k_scalar_uint32 = MTLC_SCALAR(MTLC_TYPE_UINT32, "uint32", 4, 4);
static const MtlcType k_scalar_uint64 = MTLC_SCALAR(MTLC_TYPE_UINT64, "uint64", 8, 8);
static const MtlcType k_scalar_bool = MTLC_SCALAR(MTLC_TYPE_BOOL, "bool", 1, 1);
static const MtlcType k_scalar_float32 = MTLC_SCALAR(MTLC_TYPE_FLOAT32, "float32", 4, 4);
static const MtlcType k_scalar_float64 = MTLC_SCALAR(MTLC_TYPE_FLOAT64, "float64", 8, 8);
static const MtlcType k_scalar_float16 = MTLC_SCALAR(MTLC_TYPE_FLOAT16, "float16", 2, 2);
static const MtlcType k_scalar_bfloat16 = MTLC_SCALAR(MTLC_TYPE_BFLOAT16, "bfloat16", 2, 2);
static const MtlcType k_scalar_string = MTLC_SCALAR(MTLC_TYPE_STRING, "string", 8, 8);
static const MtlcType k_scalar_void = MTLC_SCALAR(MTLC_TYPE_VOID, "void", 0, 1);
#undef MTLC_SCALAR

const MtlcType *mtlc_type_scalar(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
    return &k_scalar_int8;
  case MTLC_TYPE_INT16:
    return &k_scalar_int16;
  case MTLC_TYPE_INT32:
    return &k_scalar_int32;
  case MTLC_TYPE_INT64:
    return &k_scalar_int64;
  case MTLC_TYPE_UINT8:
    return &k_scalar_uint8;
  case MTLC_TYPE_UINT16:
    return &k_scalar_uint16;
  case MTLC_TYPE_UINT32:
    return &k_scalar_uint32;
  case MTLC_TYPE_UINT64:
    return &k_scalar_uint64;
  case MTLC_TYPE_BOOL:
    return &k_scalar_bool;
  case MTLC_TYPE_FLOAT32:
    return &k_scalar_float32;
  case MTLC_TYPE_FLOAT64:
    return &k_scalar_float64;
  case MTLC_TYPE_FLOAT16:
    return &k_scalar_float16;
  case MTLC_TYPE_BFLOAT16:
    return &k_scalar_bfloat16;
  case MTLC_TYPE_STRING:
    return &k_scalar_string;
  case MTLC_TYPE_VOID:
    return &k_scalar_void;
  default:
    return NULL; /* aggregates/pointers need caller-supplied layout */
  }
}

/* Interned pointer types: pointer-to-X is created once and lives for the
 * process (same immortality contract as the scalar singletons -- the module
 * type registry stores MtlcType* by reference and never frees them). The
 * table is tiny in practice: one entry per distinct pointee. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local, upholding the backend's no-shared-mutable-global-state
 * invariant (see common.h): two frontends on separate threads each intern
 * their own (immutable, immortal) pointer descriptors. */
#include "../common.h"
static MTLC_THREAD_LOCAL const MtlcType **g_ptr_cache;
static MTLC_THREAD_LOCAL size_t g_ptr_cache_count, g_ptr_cache_cap;

const MtlcType *mtlc_type_pointer(const MtlcType *base) {
  return mtlc_type_pointer_in(base, MTLC_ADDRESS_SPACE_GENERIC);
}

static const char *mtlc_address_space_name(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_GLOBAL: return "global";
  case MTLC_ADDRESS_SPACE_WORKGROUP: return "workgroup";
  case MTLC_ADDRESS_SPACE_CONSTANT: return "constant";
  case MTLC_ADDRESS_SPACE_PRIVATE: return "private";
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
    return "generic";
  }
  return NULL;
}

const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space) {
  if (!base) {
    return NULL;
  }
  if (!mtlc_address_space_name(address_space)) {
    return NULL;
  }
  if (address_space == MTLC_ADDRESS_SPACE_DEFAULT) {
    address_space = MTLC_ADDRESS_SPACE_GENERIC;
  }
  for (size_t i = 0; i < g_ptr_cache_count; i++) {
    if (g_ptr_cache[i]->base_type == (MtlcType *)base &&
        g_ptr_cache[i]->address_space == address_space) {
      return g_ptr_cache[i];
    }
  }
  MtlcType *p = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!p) {
    return NULL;
  }
  p->kind = MTLC_TYPE_POINTER;
  p->size = 8;
  p->alignment = 8;
  p->base_type = (MtlcType *)base;
  p->address_space = address_space;
  /* Generic pointers retain the source-compatible "T*" spelling. Explicit
   * spaces are part of the canonical name so a module can register global and
   * workgroup pointers to the same element type without aliasing them. */
  {
    const char *bn = base->name ? base->name : mtlc_type_kind_name(base->kind);
    const char *asn = mtlc_address_space_name(address_space);
    int explicit_space = address_space != MTLC_ADDRESS_SPACE_GENERIC;
    size_t n = strlen(bn) + 2 + (explicit_space ? strlen(asn) + 2 : 0);
    char *nm = (char *)malloc(n);
    if (!nm) {
      free(p);
      return NULL;
    }
    if (explicit_space) {
      snprintf(nm, n, "%s:%s*", asn, bn);
    } else {
      snprintf(nm, n, "%s*", bn);
    }
    p->name = nm;
  }
  if (g_ptr_cache_count == g_ptr_cache_cap) {
    size_t next = g_ptr_cache_cap ? g_ptr_cache_cap * 2 : 8;
    const MtlcType **grown =
        (const MtlcType **)realloc(g_ptr_cache, next * sizeof(*g_ptr_cache));
    if (!grown) {
      free((char *)p->name);
      free(p);
      return NULL;
    }
    g_ptr_cache = grown;
    g_ptr_cache_cap = next;
  }
  g_ptr_cache[g_ptr_cache_count++] = p;
  return p;
}

/* Interned composite descriptors. Same immortality contract as the pointer
 * cache above: built once per distinct shape, never freed, thread-local so two
 * frontends on separate threads each intern their own. */
static MTLC_THREAD_LOCAL const MtlcType **g_composite_cache;
static MTLC_THREAD_LOCAL size_t g_composite_count, g_composite_cap;

static int composite_cache_push(const MtlcType *t) {
  if (g_composite_count == g_composite_cap) {
    size_t next = g_composite_cap ? g_composite_cap * 2 : 8;
    const MtlcType **grown = (const MtlcType **)realloc(
        g_composite_cache, next * sizeof(*g_composite_cache));
    if (!grown) {
      return 0;
    }
    g_composite_cache = grown;
    g_composite_cap = next;
  }
  g_composite_cache[g_composite_count++] = t;
  return 1;
}

const MtlcType *mtlc_type_array(const MtlcType *element, size_t count) {
  if (!element || count == 0) {
    return NULL;
  }
  size_t element_size = element->size;
  if (element_size == 0 || count > (size_t)-1 / element_size) {
    return NULL;
  }
  for (size_t i = 0; i < g_composite_count; i++) {
    const MtlcType *c = g_composite_cache[i];
    if (c->kind == MTLC_TYPE_ARRAY && c->base_type == (MtlcType *)element &&
        c->array_size == count) {
      return c;
    }
  }
  MtlcType *a = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!a) {
    return NULL;
  }
  a->kind = MTLC_TYPE_ARRAY;
  a->base_type = (MtlcType *)element;
  a->array_size = count;
  a->size = element_size * count;
  a->alignment = element->alignment ? element->alignment : 1;
  {
    const char *en =
        element->name ? element->name : mtlc_type_kind_name(element->kind);
    size_t n = strlen(en) + 32;
    char *nm = (char *)malloc(n);
    if (!nm) {
      free(a);
      return NULL;
    }
    snprintf(nm, n, "%s[%llu]", en, (unsigned long long)count);
    a->name = nm;
  }
  if (!composite_cache_push(a)) {
    free((char *)a->name);
    free(a);
    return NULL;
  }
  return a;
}

/* Does an already-interned struct describe exactly this field list? */
static int struct_layout_matches(const MtlcType *s,
                                 const char *const *field_names,
                                 const MtlcType *const *field_types,
                                 size_t field_count) {
  if (s->field_count != field_count) {
    return 0;
  }
  for (size_t i = 0; i < field_count; i++) {
    if (s->field_types[i] != (MtlcType *)field_types[i]) {
      return 0;
    }
    const char *want = field_names && field_names[i] ? field_names[i] : "";
    if (strcmp(s->field_names[i] ? s->field_names[i] : "", want) != 0) {
      return 0;
    }
  }
  return 1;
}

static void struct_destroy(MtlcType *s) {
  if (!s) {
    return;
  }
  if (s->field_names) {
    for (size_t i = 0; i < s->field_count; i++) {
      free((char *)s->field_names[i]);
    }
  }
  free((void *)s->field_names);
  free(s->field_types);
  free(s->field_offsets);
  free((char *)s->name);
  free(s);
}

const MtlcType *mtlc_type_struct(const char *name,
                                 const char *const *field_names,
                                 const MtlcType *const *field_types,
                                 size_t field_count) {
  if (!name || !name[0] || field_count == 0 || !field_types) {
    return NULL;
  }
  for (size_t i = 0; i < field_count; i++) {
    if (!field_types[i] || field_types[i]->size == 0) {
      return NULL;
    }
  }
  /* Interned by name: a repeat declaration of the same layout is the same
   * type; a conflicting one is a frontend bug, not a second type. */
  for (size_t i = 0; i < g_composite_count; i++) {
    const MtlcType *c = g_composite_cache[i];
    if (c->kind == MTLC_TYPE_STRUCT && c->name && strcmp(c->name, name) == 0) {
      return struct_layout_matches(c, field_names, field_types, field_count)
                 ? c
                 : NULL;
    }
  }

  MtlcType *s = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!s) {
    return NULL;
  }
  s->kind = MTLC_TYPE_STRUCT;
  s->field_count = field_count;
  s->name = mettle_strdup(name);
  s->field_names = (const char **)calloc(field_count, sizeof(char *));
  s->field_types = (MtlcType **)calloc(field_count, sizeof(MtlcType *));
  s->field_offsets = (size_t *)calloc(field_count, sizeof(size_t));
  if (!s->name || !s->field_names || !s->field_types || !s->field_offsets) {
    struct_destroy(s);
    return NULL;
  }

  /* Standard C layout: pad each field up to its own alignment, then round the
   * total up to the widest field alignment. */
  size_t offset = 0;
  size_t max_align = 1;
  for (size_t i = 0; i < field_count; i++) {
    const MtlcType *ft = field_types[i];
    size_t align = ft->alignment ? ft->alignment : 1;
    if (align > max_align) {
      max_align = align;
    }
    size_t pad = offset % align;
    if (pad) {
      offset += align - pad;
    }
    s->field_names[i] =
        mettle_strdup(field_names && field_names[i] ? field_names[i] : "");
    if (!s->field_names[i]) {
      struct_destroy(s);
      return NULL;
    }
    s->field_types[i] = (MtlcType *)ft;
    s->field_offsets[i] = offset;
    offset += ft->size;
  }
  size_t tail = offset % max_align;
  if (tail) {
    offset += max_align - tail;
  }
  s->size = offset;
  s->alignment = max_align;

  if (!composite_cache_push(s)) {
    struct_destroy(s);
    return NULL;
  }
  return s;
}

size_t mtlc_type_field_count(const MtlcType *t) {
  return (t && t->kind == MTLC_TYPE_STRUCT) ? t->field_count : 0;
}

size_t mtlc_type_field_offset(const MtlcType *t, size_t index) {
  if (!t || t->kind != MTLC_TYPE_STRUCT || index >= t->field_count ||
      !t->field_offsets) {
    return (size_t)-1;
  }
  return t->field_offsets[index];
}

size_t mtlc_type_field_index(const MtlcType *t, const char *name) {
  if (!t || t->kind != MTLC_TYPE_STRUCT || !name || !t->field_names) {
    return (size_t)-1;
  }
  for (size_t i = 0; i < t->field_count; i++) {
    if (t->field_names[i] && strcmp(t->field_names[i], name) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

/* The canonical spelling of a function-pointer type, used both as the cache
 * key and as the name codegen resolves: "ret(*)(p0,p1)". */
static char *function_pointer_name(const MtlcType *return_type,
                                   const MtlcType *const *param_types,
                                   size_t param_count) {
  const char *rn = return_type->name ? return_type->name
                                     : mtlc_type_kind_name(return_type->kind);
  size_t n = strlen(rn) + 8;
  for (size_t i = 0; i < param_count; i++) {
    const MtlcType *p = param_types[i];
    n += strlen(p->name ? p->name : mtlc_type_kind_name(p->kind)) + 1;
  }
  char *nm = (char *)malloc(n);
  if (!nm) {
    return NULL;
  }
  size_t used = (size_t)snprintf(nm, n, "%s(*)(", rn);
  for (size_t i = 0; i < param_count; i++) {
    const MtlcType *p = param_types[i];
    used += (size_t)snprintf(nm + used, n - used, "%s%s", i ? "," : "",
                             p->name ? p->name : mtlc_type_kind_name(p->kind));
  }
  snprintf(nm + used, n - used, ")");
  return nm;
}

const MtlcType *mtlc_type_function_pointer(const MtlcType *return_type,
                                           const MtlcType *const *param_types,
                                           size_t param_count) {
  if (!return_type || (param_count > 0 && !param_types)) {
    return NULL;
  }
  for (size_t i = 0; i < param_count; i++) {
    if (!param_types[i]) {
      return NULL;
    }
  }
  char *nm = function_pointer_name(return_type, param_types, param_count);
  if (!nm) {
    return NULL;
  }
  for (size_t i = 0; i < g_composite_count; i++) {
    const MtlcType *c = g_composite_cache[i];
    if (c->kind == MTLC_TYPE_FUNCTION_POINTER && c->name &&
        strcmp(c->name, nm) == 0) {
      free(nm);
      return c;
    }
  }

  MtlcType *f = (MtlcType *)calloc(1, sizeof(MtlcType));
  if (!f) {
    free(nm);
    return NULL;
  }
  f->kind = MTLC_TYPE_FUNCTION_POINTER;
  f->name = nm;
  f->size = 8;
  f->alignment = 8;
  f->fn_return_type = (MtlcType *)return_type;
  f->fn_param_count = param_count;
  if (param_count > 0) {
    f->fn_param_types = (MtlcType **)calloc(param_count, sizeof(MtlcType *));
    if (!f->fn_param_types) {
      free(nm);
      free(f);
      return NULL;
    }
    for (size_t i = 0; i < param_count; i++) {
      f->fn_param_types[i] = (MtlcType *)param_types[i];
    }
  }
  if (!composite_cache_push(f)) {
    free(f->fn_param_types);
    free(nm);
    free(f);
    return NULL;
  }
  return f;
}

int mtlc_type_is_integer(const MtlcType *t) {
  if (!t) {
    return 0;
  }
  switch (t->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
    return 1;
  default:
    return 0;
  }
}

int mtlc_type_is_unsigned(const MtlcType *t) {
  if (!t) {
    return 0;
  }
  switch (t->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
    return 1;
  default:
    return 0;
  }
}

int mtlc_type_is_float(const MtlcType *t) {
  return t && (t->kind == MTLC_TYPE_FLOAT32 || t->kind == MTLC_TYPE_FLOAT64 ||
               t->kind == MTLC_TYPE_FLOAT16 || t->kind == MTLC_TYPE_BFLOAT16);
}

int mtlc_type_is_aggregate(const MtlcType *t) {
  return t && (t->kind == MTLC_TYPE_STRUCT || t->kind == MTLC_TYPE_TAGGED_ENUM ||
               t->kind == MTLC_TYPE_ARRAY);
}

size_t mtlc_type_size(const MtlcType *t) { return t ? t->size : 0; }

size_t mtlc_type_alignment(const MtlcType *t) { return t ? t->alignment : 0; }

const char *mtlc_type_kind_name(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
    return "int8";
  case MTLC_TYPE_INT16:
    return "int16";
  case MTLC_TYPE_INT32:
    return "int32";
  case MTLC_TYPE_INT64:
    return "int64";
  case MTLC_TYPE_UINT8:
    return "uint8";
  case MTLC_TYPE_UINT16:
    return "uint16";
  case MTLC_TYPE_UINT32:
    return "uint32";
  case MTLC_TYPE_UINT64:
    return "uint64";
  case MTLC_TYPE_BOOL:
    return "bool";
  case MTLC_TYPE_FLOAT32:
    return "float32";
  case MTLC_TYPE_FLOAT64:
    return "float64";
  case MTLC_TYPE_FLOAT16:
    return "float16";
  case MTLC_TYPE_BFLOAT16:
    return "bfloat16";
  case MTLC_TYPE_STRING:
    return "string";
  case MTLC_TYPE_FUNCTION_POINTER:
    return "fnptr";
  case MTLC_TYPE_POINTER:
    return "pointer";
  case MTLC_TYPE_ARRAY:
    return "array";
  case MTLC_TYPE_STRUCT:
    return "struct";
  case MTLC_TYPE_ENUM:
    return "enum";
  case MTLC_TYPE_TAGGED_ENUM:
    return "tagged_enum";
  case MTLC_TYPE_VOID:
    return "void";
  }
  return "?";
}
