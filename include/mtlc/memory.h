/* mtlc/memory.h - target-neutral GPU memory model vocabulary. */
#ifndef MTLC_MEMORY_H
#define MTLC_MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

/* DEFAULT is reserved for compatibility at IR boundaries. New frontends
 * should state an address space explicitly for device-visible pointers. */
typedef enum {
  MTLC_ADDRESS_SPACE_DEFAULT = 0,
  MTLC_ADDRESS_SPACE_GENERIC,
  MTLC_ADDRESS_SPACE_GLOBAL,
  MTLC_ADDRESS_SPACE_WORKGROUP,
  MTLC_ADDRESS_SPACE_CONSTANT,
  MTLC_ADDRESS_SPACE_PRIVATE
} MtlcAddressSpace;

/* How a device view stores its elements. A layout is part of a view's type: an
 * MMA operand, an asynchronous copy, or a tensor transfer states the layout it
 * requires, and a view carrying another is refused rather than reinterpreted.
 * ROW and COL are the ordinary dense orders; SWIZZLE64 and SWIZZLE128 permute
 * the elements of a workgroup tile so a subgroup's addresses land in distinct
 * banks; INTERLEAVE(k) groups k elements of each row before moving on; the
 * FRAGMENT forms are the per-lane register layouts the target's MMA takes. */
typedef enum {
  MTLC_VIEW_LAYOUT_NONE = 0,
  MTLC_VIEW_LAYOUT_ROW,
  MTLC_VIEW_LAYOUT_COL,
  MTLC_VIEW_LAYOUT_SWIZZLE64,
  MTLC_VIEW_LAYOUT_SWIZZLE128,
  MTLC_VIEW_LAYOUT_INTERLEAVE,
  MTLC_VIEW_LAYOUT_FRAGMENT_A,
  MTLC_VIEW_LAYOUT_FRAGMENT_B,
  MTLC_VIEW_LAYOUT_FRAGMENT_C
} MtlcViewLayout;

/* These orders have their C/C++ meanings. DEFAULT is not an order; legacy GPU
 * atomics normalize to RELAXED when they enter a device backend. */
typedef enum {
  MTLC_MEMORY_ORDER_DEFAULT = 0,
  MTLC_MEMORY_ORDER_RELAXED,
  MTLC_MEMORY_ORDER_ACQUIRE,
  MTLC_MEMORY_ORDER_RELEASE,
  MTLC_MEMORY_ORDER_ACQ_REL,
  MTLC_MEMORY_ORDER_SEQ_CST
} MtlcMemoryOrder;

/* Scope names describe topology, not a vendor ISA. A backend may safely
 * strengthen an unavailable narrow scope, but must never weaken one. */
typedef enum {
  MTLC_MEMORY_SCOPE_DEFAULT = 0,
  MTLC_MEMORY_SCOPE_WORK_ITEM,
  MTLC_MEMORY_SCOPE_SUBGROUP,
  MTLC_MEMORY_SCOPE_WORKGROUP,
  MTLC_MEMORY_SCOPE_DEVICE,
  MTLC_MEMORY_SCOPE_SYSTEM
} MtlcMemoryScope;

/* Memory regions affected by a collective barrier. These are bit flags because
 * a barrier commonly publishes both workgroup scratch and global results. */
typedef enum {
  MTLC_MEMORY_REGION_NONE = 0,
  MTLC_MEMORY_REGION_WORKGROUP = 1u << 0,
  MTLC_MEMORY_REGION_GLOBAL = 1u << 1
} MtlcMemoryRegion;

/* Performance hint for a neutral global-to-workgroup asynchronous copy.
 * ALL permits caching at every available level; GLOBAL requests that the
 * implementation prefer only the device-wide/global cache. A backend may
 * ignore either hint without changing program semantics. */
typedef enum {
  MTLC_ASYNC_CACHE_DEFAULT = 0,
  MTLC_ASYNC_CACHE_ALL,
  MTLC_ASYNC_CACHE_GLOBAL
} MtlcAsyncCache;

#ifdef __cplusplus
}
#endif

#endif /* MTLC_MEMORY_H */
