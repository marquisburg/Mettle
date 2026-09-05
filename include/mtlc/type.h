/* mtlc/type.h - backend-owned type descriptor (the "type IR").
 *
 * libmtlc is a standalone compiler backend, usable by any frontend. It must not
 * depend on any particular frontend's type system, so the backend owns its own
 * type descriptor here. A frontend lowers its own types into MtlcType at the IR
 * boundary (see mtlc_type_from_frontend in the reference Mettle frontend).
 *
 * This struct deliberately mirrors the shape the native code generators need:
 * kind + byte size/alignment, pointer/array element types, aggregate (struct)
 * layout, function-pointer signatures, and tagged-enum layout. It is a plain
 * value type with no methods; construct instances with the mtlc_type_* helpers
 * or fill the fields directly.
 */
#ifndef MTLC_TYPE_H
#define MTLC_TYPE_H

#include <stddef.h>
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scalar/aggregate classification. Mirrors the categories every native backend
 * must distinguish for ABI classification, load/store widths, and layout. */
typedef enum {
  MTLC_TYPE_INT8,
  MTLC_TYPE_INT16,
  MTLC_TYPE_INT32,
  MTLC_TYPE_INT64,
  MTLC_TYPE_UINT8,
  MTLC_TYPE_UINT16,
  MTLC_TYPE_UINT32,
  MTLC_TYPE_UINT64,
  MTLC_TYPE_BOOL,
  MTLC_TYPE_FLOAT32,
  MTLC_TYPE_FLOAT64,
  MTLC_TYPE_FLOAT16,
  MTLC_TYPE_BFLOAT16,
  MTLC_TYPE_STRING,
  MTLC_TYPE_FUNCTION_POINTER,
  MTLC_TYPE_POINTER,
  MTLC_TYPE_ARRAY,
  MTLC_TYPE_STRUCT,
  MTLC_TYPE_ENUM,
  MTLC_TYPE_TAGGED_ENUM,
  MTLC_TYPE_VOID
} MtlcTypeKind;

typedef struct MtlcType {
  MtlcTypeKind kind;
  const char *name;      /* optional; interned/owned by the frontend adapter */
  size_t size;           /* size in bytes */
  size_t alignment;      /* alignment in bytes */

  /* Meaningful for MTLC_TYPE_POINTER. DEFAULT/GENERIC retain ordinary host
   * pointer behavior; device frontends should use mtlc_type_pointer_in(). */
  MtlcAddressSpace address_space;

  /* Byte alignment the pointed-to address is proven to have, beyond the
   * element type's own. Zero means nothing extra is known. A GPU backend uses
   * it to widen a run of adjacent element loads into one vector access. */
  size_t pointee_align;

  /* How a static device view stores its elements, and the parameter the
   * parameterised forms carry. MTLC_VIEW_LAYOUT_NONE is an ordinary
   * host-shaped view. */
  MtlcViewLayout view_layout;
  unsigned view_layout_param;
  /* Static extents of a device view, outermost first. A zero extent is a
   * runtime one. */
  size_t view_extents[4];
  unsigned view_extent_count;

  struct MtlcType *base_type; /* pointer/array element type */
  size_t array_size;          /* element count for MTLC_TYPE_ARRAY */

  /* Function-pointer signature (MTLC_TYPE_FUNCTION_POINTER). */
  struct MtlcType **fn_param_types;
  size_t fn_param_count;
  struct MtlcType *fn_return_type;
  /* Synthesized closure-environment struct type for a capturing closure, else
   * NULL. The closure value is an 8-byte pointer to a heap record whose field 0
   * is the code pointer and whose remaining fields are the captures. */
  struct MtlcType *closure_env;

  /* Aggregate layout (MTLC_TYPE_STRUCT). */
  const char **field_names;
  struct MtlcType **field_types;
  size_t *field_offsets;
  size_t field_count;

  /* Tagged-enum layout (MTLC_TYPE_TAGGED_ENUM). */
  const char **tagged_variant_names;
  int *tagged_variant_tags;                 /* discriminant per variant */
  struct MtlcType **tagged_variant_payloads;/* payload type per variant (NULL=none) */
  size_t tagged_variant_count;
  size_t tagged_data_offset;                /* offset of the data union */
  size_t tagged_data_size;                  /* size of the data union */
} MtlcType;

/* Canonical descriptor for a scalar/primitive kind: a shared, immortal singleton
 * with the right size/alignment and canonical name. Intended for frontends that
 * build IR through mtlc/build.h -- the returned pointer never needs freeing and
 * outlives codegen. Returns NULL for kinds that carry their own layout --
 * build those with mtlc_type_pointer, mtlc_type_array, mtlc_type_struct, or
 * mtlc_type_function_pointer below, which compute the layout and are interned
 * and immortal on the same terms. */
const MtlcType *mtlc_type_scalar(MtlcTypeKind kind);

/* Canonical pointer-to-`base` descriptor. Interned and immortal like
 * mtlc_type_scalar: calling it twice with the same base returns the same
 * pointer, and the result never needs freeing. `base` must itself be a
 * canonical descriptor (from mtlc_type_scalar or mtlc_type_pointer), so
 * pointer-to-pointer chains work. Returns NULL on NULL base or OOM. */
const MtlcType *mtlc_type_pointer(const MtlcType *base);

/* Canonical pointer descriptor with an explicit device address space. The
 * descriptor is interned and immortal like mtlc_type_pointer(). */
const MtlcType *mtlc_type_pointer_in(const MtlcType *base,
                                     MtlcAddressSpace address_space);

/* Canonical `element[count]` descriptor: size is count * element size, and the
 * alignment is the element's. Interned and immortal like mtlc_type_pointer, so
 * the same (element, count) pair always yields the same pointer and the result
 * never needs freeing. `element` must itself be a canonical descriptor, so
 * arrays of arrays and arrays of structs work. Returns NULL on a NULL element,
 * a zero count, a size that would overflow, or OOM. */
const MtlcType *mtlc_type_array(const MtlcType *element, size_t count);

/* Canonical struct descriptor with the layout computed for you under the
 * standard C rule: each field is placed at the next offset that satisfies its
 * own alignment, the struct's alignment is the widest field's, and the total
 * size is rounded up to that alignment. This is what mtlc_field_offset and
 * mtlc_field_address (mtlc/build.h) report and address, so a frontend never
 * hand-computes offsets.
 *
 * `name` is copied and becomes the descriptor's registered type name; it must
 * be unique within the process for a given layout. Structs are interned by
 * name: declaring the same name twice returns the first descriptor, and a
 * second declaration with a conflicting layout returns NULL. `field_names` and
 * `field_types` each hold `field_count` entries; the names are copied and the
 * types must be canonical descriptors. Returns NULL on invalid input or OOM. */
const MtlcType *mtlc_type_struct(const char *name,
                                 const char *const *field_names,
                                 const MtlcType *const *field_types,
                                 size_t field_count);

/* Byte offset of field `index` within a struct descriptor, and the number of
 * fields it has. mtlc_type_field_count returns 0 for a non-struct;
 * mtlc_type_field_offset returns (size_t)-1 for a non-struct or an index that
 * is out of range. */
size_t mtlc_type_field_count(const MtlcType *t);
size_t mtlc_type_field_offset(const MtlcType *t, size_t index);

/* Index of the field named `name`, or (size_t)-1 when the type is not a struct
 * or has no such field. */
size_t mtlc_type_field_index(const MtlcType *t, const char *name);

/* Canonical function-pointer descriptor for a callee returning `return_type`
 * and taking `param_count` parameters. Interned and immortal; the parameter
 * array is copied. Use it as the type of a value produced by
 * mtlc_function_address (mtlc/build.h). `param_types` may be NULL when
 * `param_count` is 0. Returns NULL on a NULL return type or OOM. */
const MtlcType *mtlc_type_function_pointer(const MtlcType *return_type,
                                           const MtlcType *const *param_types,
                                           size_t param_count);

/* Queries used across the backend. Implemented in src/ir/mtlc_type.c. */
int mtlc_type_is_integer(const MtlcType *t);
int mtlc_type_is_unsigned(const MtlcType *t);
int mtlc_type_is_float(const MtlcType *t);
int mtlc_type_is_aggregate(const MtlcType *t);
size_t mtlc_type_size(const MtlcType *t);
size_t mtlc_type_alignment(const MtlcType *t);
const char *mtlc_type_kind_name(MtlcTypeKind kind);

#ifdef __cplusplus
}
#endif

#endif /* MTLC_TYPE_H */
