/* Included by safety.c. All identities and memory metadata share its lock.
 * Pointer values and public layouts remain ordinary machine values. */

uint64_t mettle_safety_identity(const void *pointer) {
  if (!pointer) return 0;
  SafetyRegion region = safety_snapshot((uintptr_t)pointer);
  return region.state == SAFETY_STATE_FREELIST ? 0 : region.identity;
}

static SafetyRegion safety_identity_snapshot(uint64_t identity) {
  SafetyRegion result = {0};
  safety_lock();
  const SafetyRegion *region = safety_region((uint32_t)(identity & 0xffffffu));
  if (region && region->identity == identity) result = *region;
  safety_unlock();
  /* Reusing a descriptor must not revive any pointer that named it. */
  if (result.state == SAFETY_STATE_FREELIST) result.state = SAFETY_STATE_DEAD;
  return result;
}

void mettle_safety_check_identity(const void *base, int64_t offset, int64_t size,
                                  uint32_t kind, uint32_t line,
                                  uint64_t identity) {
  (void)kind;
  if (!identity) {
    if (size <= 0 || safety_allocator_depth()) return;
    SafetyFailure failure = {base ? "memory access has no tracked allocation identity" :
        "null pointer dereference", line, offset, size, 0, 0};
    safety_report_and_trap(&failure, __builtin_return_address(0), __builtin_frame_address(0));
    return;
  }
  if (size <= 0) return;
  SafetyRegion region = safety_identity_snapshot(identity);
  uintptr_t start = (uintptr_t)base;
  uintptr_t address = start + (uintptr_t)offset;
  int wrapped = offset >= 0 ? address < start : address > start;
  uint64_t width = (uint64_t)size;
  if (base && !wrapped && region.state == SAFETY_STATE_LIVE &&
      width <= region.size && address >= region.start &&
      address - region.start <= region.size - width) return;
  safety_check_failed(base, offset, size, line, &region,
                      __builtin_return_address(0), __builtin_frame_address(0));
}

int64_t mettle_safety_span_identity(const void *base, uint64_t identity) {
  if (!identity) return 0;
  SafetyRegion region = safety_identity_snapshot(identity);
  uintptr_t address = (uintptr_t)base;
  if (!base || region.state != SAFETY_STATE_LIVE || address < region.start ||
      address - region.start >= region.size) return 0;
  return (int64_t)(region.size - (address - region.start));
}

/* A value can carry at most one allocation origin. Combining two different
 * origins cannot create a valid pointer, but subtracting them may form an
 * ordinary integer. Keep an invalid identity if it later reaches an access. */
uint64_t mettle_safety_merge_identity(uint64_t a, uint64_t b) {
  return !a ? b : !b || a == b ? a : UINT64_MAX;
}

uint64_t mettle_safety_subtract_identity(uint64_t a, uint64_t b) {
  /* The difference between two addresses is an integer distance. */
  return a && b ? 0 : a ? a : b;
}

#define SAFETY_VALUE_BUCKETS 4096u
typedef struct SafetyValue {
  uintptr_t address;
  uint64_t value;
  uint64_t identity;
  uint64_t owner;
  struct SafetyValue *next;
} SafetyValue;
static SafetyValue *g_safety_values[SAFETY_VALUE_BUCKETS];
static SafetyValue *g_safety_value_free;
static size_t g_safety_value_count;
typedef struct SafetyValueBlock {
  struct SafetyValueBlock *next;
} SafetyValueBlock;
static SafetyValueBlock *g_safety_value_blocks;

static size_t safety_value_bucket(uintptr_t address) {
  return (size_t)((address >> 3) ^ (address >> 17)) & (SAFETY_VALUE_BUCKETS - 1);
}

/* Caller holds the lock. */
static uint64_t safety_owner_identity(uintptr_t address) {
  uint32_t id = safety_lookup(address);
  SafetyRegion *region = id >= SAFETY_ID_FIRST ? safety_region(id) : NULL;
  return region ? region->identity : 0;
}

static SafetyValue *safety_value_find(uintptr_t address) {
  SafetyValue *entry = g_safety_values[safety_value_bucket(address)];
  while (entry && entry->address != address) entry = entry->next;
  return entry;
}

static int safety_value_set(uintptr_t address, uint64_t value, uint64_t identity) {
  SafetyValue *entry = safety_value_find(address);
  if (!entry) {
    if (!identity) return 1;
    if (!g_safety_value_free) {
      size_t count = (4096 - sizeof(SafetyValueBlock)) / sizeof(SafetyValue);
      SafetyValueBlock *page = (SafetyValueBlock *)safety_map(4096);
      if (!page) return 0;
      page->next = g_safety_value_blocks;
      g_safety_value_blocks = page;
      SafetyValue *block = (SafetyValue *)(page + 1);
      for (size_t i = 0; i < count; i++) {
        block[i].next = g_safety_value_free;
        g_safety_value_free = &block[i];
      }
    }
    entry = g_safety_value_free;
    g_safety_value_free = entry->next;
    size_t bucket = safety_value_bucket(address);
    entry->next = g_safety_values[bucket];
    g_safety_values[bucket] = entry;
    entry->address = address;
    g_safety_value_count++;
  }
  entry->value = value;
  entry->identity = identity;
  entry->owner = safety_owner_identity(address);
  return 1;
}

uint64_t mettle_safety_value_load(const void *slot, uint64_t value, uint64_t size) {
  (void)value;
  safety_lock();
  uint64_t identity = 0;
  for (uint64_t i = 0; i < size && i < 8; i++) {
    uintptr_t address = (uintptr_t)slot + (uintptr_t)i;
    SafetyValue *entry = safety_value_find(address);
    if (entry && entry->owner == safety_owner_identity(address))
      identity = mettle_safety_merge_identity(identity, entry->identity);
  }
  safety_unlock();
  return identity;
}

void mettle_safety_value_store(void *slot, uint64_t value, uint64_t identity,
                               uint64_t size) {
  uintptr_t address = (uintptr_t)slot;
  if (!size) return;
  safety_lock();
  uint64_t width = size > 8 ? 8 : size;
  int ok = 1;
  /* Byte metadata preserves pointer origins through packed fields and ordinary
   * byte copy loops as well as through native word loads and stores. */
  for (uint64_t i = 0; i < width; i++) {
    if (!safety_value_set(address + (uintptr_t)i, (value >> (8 * i)) & 255, identity)) {
      ok = 0;
      break;
    }
  }
  safety_unlock();
  if (!ok) safety_internal_failure("Fatal error: unable to allocate pointer safety metadata");
}

static void safety_values_clear_locked(uintptr_t address, uint64_t size) {
  if (!g_safety_value_count) return;
  for (size_t bucket = 0; bucket < SAFETY_VALUE_BUCKETS; bucket++) {
    SafetyValue **link = &g_safety_values[bucket];
    while (*link) {
      SafetyValue *entry = *link;
      int overlaps = entry->address >= address && entry->address - address < size;
      if (!overlaps) { link = &entry->next; continue; }
      *link = entry->next;
      entry->next = g_safety_value_free;
      g_safety_value_free = entry;
      g_safety_value_count--;
    }
  }
}

static void safety_values_reset_locked(void) {
  while (g_safety_value_blocks) {
    SafetyValueBlock *page = g_safety_value_blocks;
    g_safety_value_blocks = page->next;
    safety_unmap(page, 4096);
  }
  for (size_t i = 0; i < SAFETY_VALUE_BUCKETS; i++) g_safety_values[i] = NULL;
  g_safety_value_free = NULL;
  g_safety_value_count = 0;
}

static void safety_values_reown(void *pointer, uint64_t size, uint64_t old_identity) {
  safety_lock();
  uintptr_t address = (uintptr_t)pointer;
  uint64_t owner = safety_owner_identity(address);
  for (size_t bucket = 0; bucket < SAFETY_VALUE_BUCKETS; bucket++) {
    for (SafetyValue *entry = g_safety_values[bucket]; entry; entry = entry->next) {
      if (entry->owner == old_identity && entry->address >= address &&
          entry->address - address < size) entry->owner = owner;
    }
  }
  safety_unlock();
}

void mettle_safety_value_copy(void *destination, const void *source, uint64_t size) {
  if (!size || destination == source) return;
  uintptr_t dst = (uintptr_t)destination, src = (uintptr_t)source;
  if (size - 1 > UINTPTR_MAX - dst || size - 1 > UINTPTR_MAX - src)
    safety_internal_failure("Fatal error: pointer metadata copy range overflow");
  safety_lock();
  size_t count = 0;
  for (size_t bucket = 0; bucket < SAFETY_VALUE_BUCKETS; bucket++) {
    for (SafetyValue *entry = g_safety_values[bucket]; entry; entry = entry->next) {
      if (entry->identity && entry->address >= src &&
          entry->address - src < size &&
          entry->owner == safety_owner_identity(entry->address)) count++;
    }
  }
  SafetyValue *copy = count ? (SafetyValue *)safety_map(count * sizeof(*copy)) : NULL;
  if (count && !copy) {
    safety_unlock();
    safety_internal_failure("Fatal error: unable to copy pointer safety metadata");
  }
  size_t next = 0;
  for (size_t bucket = 0; bucket < SAFETY_VALUE_BUCKETS; bucket++) {
    for (SafetyValue *entry = g_safety_values[bucket]; entry; entry = entry->next) {
      if (entry->identity && entry->address >= src &&
          entry->address - src < size &&
          entry->owner == safety_owner_identity(entry->address)) copy[next++] = *entry;
    }
  }
  safety_values_clear_locked(dst, size);
  int ok = 1;
  for (size_t i = 0; i < count; i++) {
    if (!safety_value_set(dst + (copy[i].address - src), copy[i].value, copy[i].identity)) {
      ok = 0;
      break;
    }
  }
  safety_unlock();
  safety_unmap(copy, count * sizeof(*copy));
  if (!ok) safety_internal_failure("Fatal error: unable to store copied pointer safety metadata");
}

void mettle_safety_value_clear(void *destination, uint64_t size) {
  if (!size) return;
  safety_lock();
  safety_values_clear_locked((uintptr_t)destination, size);
  safety_unlock();
}

void mettle_safety_free_identity(void *pointer, uint64_t identity) {
  if (!pointer) return;
  if (!identity) {
    safety_internal_failure("Fatal error: free has no tracked allocation identity");
  }
  SafetyRegion region = identity ? safety_identity_snapshot(identity) :
      safety_snapshot((uintptr_t)pointer);
  if (region.state != SAFETY_STATE_LIVE || !region.heap || region.start != (uintptr_t)pointer) {
    SafetyFailure failure = {region.state == SAFETY_STATE_DEAD ?
        "free of memory after it was freed" : !region.heap ?
        "free of stack or global memory" : "free of an interior pointer",
        0, 0, 0, region.size, 1};
    safety_report_and_trap(&failure, __builtin_return_address(0), __builtin_frame_address(0));
  }
}

void mettle_safety_region_begin(void *pointer, int64_t size) {
  if (!pointer || size <= 0)
    safety_internal_failure("Fatal error: a foreign memory region needs an address and a positive size");
  SafetyRegion known = safety_snapshot((uintptr_t)pointer);
  if (known.state == SAFETY_STATE_LIVE && (uintptr_t)pointer == known.start &&
      (uint64_t)size == known.size) return;
  mettle_safety_register_static(pointer, (uint64_t)size);
}

/* The process hands main its argument vector, so nothing in the program ever
 * allocated it. Describe it here and give every string an identity of its own,
 * which is what lets an index into either one be checked rather than waved
 * through. Adjacent short arguments share a granule; the exact descriptor
 * bounds settle which one an address belongs to. */
void mettle_safety_entry_arguments(void *vector) {
  if (!vector) return;
  char **arguments = (char **)vector;
  uint64_t count = 0;
  while (arguments[count]) count++;
  mettle_safety_register_static(vector, (count + 1) * sizeof(char *));
  for (uint64_t i = 0; i < count; i++) {
    uint64_t length = 0;
    while (arguments[i][length]) length++;
    uint64_t identity = mettle_safety_literal_identity(arguments[i], length + 1);
    mettle_safety_value_store(&arguments[i], (uint64_t)(uintptr_t)arguments[i],
                              identity, sizeof(char *));
  }
}

void mettle_safety_region_end(void *pointer) {
  mettle_safety_unregister(pointer);
}

uint64_t mettle_safety_literal_identity(const void *pointer, uint64_t size) {
  SafetyRegion known = safety_snapshot((uintptr_t)pointer);
  if (known.state == SAFETY_STATE_LIVE && (uintptr_t)pointer >= known.start &&
      size <= known.size && (uintptr_t)pointer - known.start <= known.size - size)
    return known.identity;
  mettle_safety_register_static((void *)pointer, size);
  return mettle_safety_identity(pointer);
}

uint64_t mettle_safety_string_identity(const void *record, uint64_t size) {
  uint64_t identity = mettle_safety_literal_identity(record, 2 * sizeof(uint64_t));
  const void *data = *(const void *const *)record;
  uint64_t data_identity = mettle_safety_literal_identity(data, size);
  mettle_safety_value_store((void *)record, (uint64_t)(uintptr_t)data, data_identity, sizeof(void *));
  return identity;
}

/* Only the compiler calls this for a pointer relocation in a static image. */
void mettle_safety_global_pointer(void *slot, uint64_t mode, uint64_t size) {
  if (mode == 3) {
    mettle_safety_string_identity(slot, size);
    return;
  }
  const void *pointer = *(const void *const *)slot;
  uint64_t identity = mode == 2 ? mettle_safety_string_identity(pointer, size) :
      mode == 1 ? mettle_safety_literal_identity(pointer, size) : mettle_safety_identity(pointer);
  mettle_safety_value_store(slot, (uint64_t)(uintptr_t)pointer, identity, sizeof(void *));
}

void mettle_safety_buffer_check(void *pointer, int64_t size, uint32_t kind,
                                uint32_t line, uint64_t identity) {
  if (size < 0) {
    SafetyFailure failure = {"invalid memory operation size", line, 0, size, 0, 0};
    safety_report_and_trap(&failure, __builtin_return_address(0), __builtin_frame_address(0));
    return;
  }
  mettle_safety_check_identity(pointer, 0, size, kind, line, identity);
}

/* Thread scoped call metadata preserves the native calling convention. The
 * callee must claim its own frame, so a foreign callback cannot accidentally
 * take the metadata meant for the foreign function. */
typedef struct SafetyPacket {
  struct SafetyPacket *next;
  size_t size;
  unsigned char data[];
} SafetyPacket;

typedef struct SafetyCall {
  struct SafetyCall *previous;
  void *callee;
  uint64_t count;
  uint64_t result;
  size_t bytes;
  int entered;
  SafetyPacket *packets;
  uint64_t arguments[];
} SafetyCall;
typedef struct {
  SafetyCall *current;
  SafetyCall *spare;
} SafetyCalls;

static void safety_packets_clear(SafetyCall *frame) {
  while (frame->packets) {
    SafetyPacket *packet = frame->packets;
    frame->packets = packet->next;
    mettle_safety_value_clear(packet->data, packet->size);
    safety_unmap(packet, sizeof(*packet) + packet->size);
  }
}

#if defined(_WIN32)
static DWORD g_safety_calls_slot = FLS_OUT_OF_INDEXES;
static void NTAPI safety_calls_destroy(void *value) {
  SafetyCalls *calls = (SafetyCalls *)value;
  if (!calls) return;
  SafetyCall *lists[2] = {calls->current, calls->spare};
  for (int i = 0; i < 2; i++) {
    SafetyCall *frame = lists[i];
    while (frame) {
      SafetyCall *previous = frame->previous;
      safety_packets_clear(frame);
      safety_unmap(frame, frame->bytes);
      frame = previous;
    }
  }
  safety_unmap(calls, sizeof(*calls));
}
static SafetyCalls *safety_calls(void) {
  DWORD slot = __atomic_load_n(&g_safety_calls_slot, __ATOMIC_ACQUIRE);
  if (slot == FLS_OUT_OF_INDEXES) {
    safety_lock();
    slot = g_safety_calls_slot;
    if (slot == FLS_OUT_OF_INDEXES) {
      slot = FlsAlloc(safety_calls_destroy);
      __atomic_store_n(&g_safety_calls_slot, slot, __ATOMIC_RELEASE);
    }
    safety_unlock();
    if (slot == FLS_OUT_OF_INDEXES)
      safety_internal_failure("Fatal error: unable to create pointer safety thread state");
  }
  SafetyCalls *calls = (SafetyCalls *)FlsGetValue(slot);
  if (!calls) {
    calls = (SafetyCalls *)safety_map(sizeof(*calls));
    if (!calls || !FlsSetValue(slot, calls))
      safety_internal_failure("Fatal error: unable to allocate pointer safety thread state");
  }
  return calls;
}
#else
static __thread SafetyCalls g_safety_calls;
static SafetyCalls *safety_calls(void) { return &g_safety_calls; }
#endif

void *mettle_safety_call_push(void *callee, uint64_t count) {
  if (count > (SIZE_MAX - sizeof(SafetyCall)) / sizeof(uint64_t))
    safety_internal_failure("Fatal error: pointer safety argument count overflow");
  SafetyCalls *calls = safety_calls();
  size_t bytes = sizeof(SafetyCall) + (size_t)count * sizeof(uint64_t);
  SafetyCall **link = &calls->spare;
  while (*link && (*link)->bytes < bytes) link = &(*link)->previous;
  SafetyCall *frame = *link;
  if (frame) {
    *link = frame->previous;
  } else {
    frame = (SafetyCall *)safety_map(bytes);
    if (!frame) safety_internal_failure("Fatal error: unable to allocate pointer safety call frame");
    frame->bytes = bytes;
  }
  frame->previous = calls->current;
  frame->callee = callee;
  frame->count = count;
  frame->result = 0;
  frame->entered = 0;
  frame->packets = NULL;
  for (uint64_t i = 0; i < count; i++) frame->arguments[i] = 0;
  calls->current = frame;
  return frame;
}

void *mettle_safety_call_enter(void *callee) {
  SafetyCall *frame = safety_calls()->current;
  if (!frame || frame->entered || frame->callee != callee) return NULL;
  frame->entered = 1;
  return frame;
}

void mettle_safety_call_arg(void *call, uint64_t index, uint64_t identity) {
  SafetyCall *frame = (SafetyCall *)call;
  if (!frame || index >= frame->count)
    safety_internal_failure("Fatal error: invalid pointer safety argument slot");
  frame->arguments[index] = identity;
}

uint64_t mettle_safety_call_param(void *call, uint64_t index) {
  SafetyCall *frame = (SafetyCall *)call;
  return frame && index < frame->count ? frame->arguments[index] : 0;
}

void mettle_safety_call_return(void *call, uint64_t identity) {
  SafetyCall *frame = (SafetyCall *)call;
  if (frame) frame->result = identity;
}

static SafetyPacket *safety_packet_capture(SafetyCall *frame, const void *source, uint64_t size) {
  if (size > SIZE_MAX - sizeof(SafetyPacket))
    safety_internal_failure("Fatal error: aggregate safety metadata size overflow");
  SafetyPacket *packet = safety_map(sizeof(*packet) + (size_t)size);
  if (!packet) safety_internal_failure("Fatal error: unable to allocate aggregate safety metadata");
  packet->size = (size_t)size;
  packet->next = frame->packets;
  frame->packets = packet;
  mettle_safety_value_copy(packet->data, source, size);
  return packet;
}

void mettle_safety_call_arg_copy(void *call, uint64_t index, const void *source, uint64_t size) {
  SafetyCall *frame = call;
  if (!frame || index >= frame->count)
    safety_internal_failure("Fatal error: invalid aggregate safety argument slot");
  frame->arguments[index] = (uint64_t)(uintptr_t)safety_packet_capture(frame, source, size);
}

static void safety_packet_restore(SafetyCall *frame, uint64_t token, void *destination, uint64_t size) {
  SafetyPacket *packet = frame ? frame->packets : NULL;
  while (packet && (uint64_t)(uintptr_t)packet != token) packet = packet->next;
  if (!packet || packet->size != size) {
    mettle_safety_value_clear(destination, size);
    return;
  }
  mettle_safety_value_copy(destination, packet->data, size);
}

void mettle_safety_call_param_copy(void *call, uint64_t index, void *destination, uint64_t size) {
  SafetyCall *frame = call;
  safety_packet_restore(frame, mettle_safety_call_param(call, index), destination, size);
}

void mettle_safety_call_return_copy(void *call, const void *source, uint64_t size) {
  SafetyCall *frame = call;
  if (frame) frame->result = (uint64_t)(uintptr_t)safety_packet_capture(frame, source, size);
}

void mettle_safety_call_result_copy(void *call, void *destination, uint64_t size) {
  SafetyCall *frame = call;
  safety_packet_restore(frame, frame ? frame->result : 0, destination, size);
}

uint64_t mettle_safety_call_pop(void *call) {
  SafetyCall *frame = (SafetyCall *)call;
  SafetyCalls *calls = safety_calls();
  if (!frame || calls->current != frame)
    safety_internal_failure("Fatal error: mismatched pointer safety call frame");
  uint64_t result = frame->result;
  safety_packets_clear(frame);
  calls->current = frame->previous;
  frame->previous = calls->spare;
  calls->spare = frame;
  return result;
}
