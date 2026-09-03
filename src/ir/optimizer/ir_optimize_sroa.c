#include "ir_optimize_internal.h"

/* ---- Scalar Replacement of Aggregates (SROA) --------------------------------
 *
 * An aggregate local whose address is taken only to load/store its own fields
 * at constant offsets never escapes, yet the address-of marks it non-promotable
 * so every field access is a memory round-trip. SROA rewrites such a local into
 * one scalar local per accessed (offset,size) slot. Those scalars are not
 * address-taken, so the register allocator can promote them directly, turning
 * the struct into registers (the struct_byval 42x case).
 *
 * The IR optimizer has no type information, so eligibility and field shapes are
 * derived purely from the IR: ADDRESS_OF gives the base, `base + CONST` gives
 * offsets, and the LOAD/STORE size operand gives each slot's width. The local
 * is eligible only when EVERY use fits the disciplined field-access pattern; any
 * other use of the symbol or an address temp (escape, whole-struct copy, call
 * argument, dynamic offset) disqualifies it and it is left untouched. */

static const char *ir_builtin_scalar_type_for_slot(int size, int is_float,
                                                    int float_bits,
                                                    int is_unsigned,
                                                    int alias_class) {
  if (is_float) {
    return float_bits == 32 ? "float32" : "float64";
  }
  switch (size) {
  case 1:
    return is_unsigned ? "uint8" : "int8";
  case 2:
    return is_unsigned ? "uint16" : "int16";
  case 4:
    return is_unsigned ? "uint32" : "int32";
  default:
    return is_unsigned ? "uint64" : "int64";
  }
}

static IRSroaSlot *ir_sroa_find_slot(IRSroaSlot *slots, size_t slot_count,
                                     long long offset) {
  for (size_t i = 0; i < slot_count; i++) {
    if (slots[i].offset == offset) {
      return &slots[i];
    }
  }
  return NULL;
}

static const IRSroaSlot *ir_sroa_find_const_slot(const IRSroaSlot *slots,
                                                 size_t slot_count,
                                                 long long offset) {
  for (size_t i = 0; i < slot_count; i++) {
    if (slots[i].offset == offset) {
      return &slots[i];
    }
  }
  return NULL;
}

/* A LOAD/STORE width that names one scalar field. Anything else is the
 * block-copy form, where the value operand is a source address and the access
 * spans the whole record -- splitting a record accessed that way would shrink
 * the copy to a single word. */
static int ir_sroa_scalar_access_width(int size) {
  return size == 1 || size == 2 || size == 4 || size == 8;
}

static int ir_sroa_note_slot(IRSroaSlot *slots, size_t *slot_count,
                             long long offset, int size, int is_float,
                             int float_bits, int is_unsigned, int alias_class) {
  IRSroaSlot *slot = ir_sroa_find_slot(slots, *slot_count, offset);
  if (!slot) {
    if (*slot_count >= IR_SROA_MAX_SLOTS) {
      return 0;
    }
    slot = &slots[(*slot_count)++];
    slot->offset = offset;
    slot->size = size;
    slot->is_float = is_float;
    slot->float_bits = float_bits;
    slot->is_unsigned = is_unsigned;
    slot->alias_class = alias_class;
    slot->name = NULL;
    return 1;
  }

  if (is_unsigned) {
    slot->is_unsigned = 1;
  }
  return slot->size == size && slot->is_float == is_float &&
         slot->float_bits == float_bits && slot->alias_class == alias_class;
}

/* Slots are appended as they are first encountered, so two aggregates with the
 * same fields can still record them in different orders -- the struct being
 * WRITTEN is usually touched in declaration order, while the struct being READ
 * is touched in whatever order the expressions need it. Sorting by offset makes
 * the layout canonical, which is what lets ir_sroa_slots_match below compare
 * two members position by position. Slot counts are bounded by
 * IR_SROA_MAX_SLOTS, so an insertion sort is the right shape. */
static void ir_sroa_sort_slots(IRSroaSlot *slots, size_t count) {
  for (size_t i = 1; i < count; i++) {
    IRSroaSlot key = slots[i];
    size_t j = i;
    while (j > 0 && slots[j - 1].offset > key.offset) {
      slots[j] = slots[j - 1];
      j--;
    }
    slots[j] = key;
  }
}

/* True when two slot layouts are identical (same offsets/sizes/float class in
 * the same order). Group members must match exactly so a whole-aggregate copy
 * can be rewritten field-for-field. */
static int ir_sroa_slots_match(const IRSroaSlot *a, size_t an,
                               const IRSroaSlot *b, size_t bn) {
  if (an != bn) {
    return 0;
  }
  for (size_t i = 0; i < an; i++) {
    if (a[i].offset != b[i].offset || a[i].size != b[i].size ||
        a[i].is_float != b[i].is_float ||
        a[i].float_bits != b[i].float_bits ||
        a[i].alias_class != b[i].alias_class) {
      return 0;
    }
  }
  return 1;
}

/* All transformable groups discovered this round, flattened. Each member
 * carries the index of its group's shared slot layout. Name lookups go through
 * two open-addressing hashes (member symbol names, address temp names): the
 * transform consults them per instruction, and linear scans here were a
 * measured strcmp hotspot on large inlined functions. */
typedef struct {
  IRSroaSlot slots[IR_SROA_MAX_SLOTS];
  size_t slot_count;
} IRSroaLayout;

typedef struct {
  const char *name;
  size_t decl_index;
  size_t layout;
  IRSroaAddr addrs[IR_SROA_MAX_SLOTS * 2];
  size_t addr_count;
} IRSroaFlatMember;

typedef struct {
  const char *key; /* borrowed name; NULL = empty */
  size_t member;
  long long offset; /* addr hash only */
} IRSroaHashEnt;

typedef struct {
  IRSroaHashEnt *ents;
  size_t bucket_count; /* power of two, 0 = empty table */
} IRSroaHash;

static int ir_sroa_hash_init(IRSroaHash *h, size_t expected) {
  size_t nb = 64;
  while (nb < expected * 2) {
    nb *= 2;
  }
  h->ents = (IRSroaHashEnt *)calloc(nb, sizeof(IRSroaHashEnt));
  if (!h->ents) {
    h->bucket_count = 0;
    return 0;
  }
  h->bucket_count = nb;
  return 1;
}

/* Insert; first insert for a key wins (mirrors the old first-match scan). */
static void ir_sroa_hash_put(IRSroaHash *h, const char *key, size_t member,
                             long long offset) {
  size_t b = mettle_fnv1a_hash(key) & (h->bucket_count - 1);
  while (h->ents[b].key) {
    if (strcmp(h->ents[b].key, key) == 0) {
      return;
    }
    b = (b + 1) & (h->bucket_count - 1);
  }
  h->ents[b].key = key;
  h->ents[b].member = member;
  h->ents[b].offset = offset;
}

static const IRSroaHashEnt *ir_sroa_hash_get(const IRSroaHash *h,
                                             const char *key) {
  if (!h->bucket_count || !key) {
    return NULL;
  }
  size_t b = mettle_fnv1a_hash(key) & (h->bucket_count - 1);
  while (h->ents[b].key) {
    if (strcmp(h->ents[b].key, key) == 0) {
      return &h->ents[b];
    }
    b = (b + 1) & (h->bucket_count - 1);
  }
  return NULL;
}

/* Build every member's address records in two shared sweeps (ADDRESS_OF
 * bases, then `base + CONST` derivations in stream order), filling `addr_hash`
 * (addr temp name -> member + offset) as it goes. Mirrors passes 1-2 of the
 * analyzer. */
static void ir_sroa_collect_all_addrs(IRFunction *function,
                                      IRSroaFlatMember *members,
                                      const IRSroaHash *member_hash,
                                      IRSroaHash *addr_hash) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_ADDRESS_OF && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name && insn->dest.kind == IR_OPERAND_TEMP &&
        insn->dest.name) {
      const IRSroaHashEnt *m = ir_sroa_hash_get(member_hash, insn->lhs.name);
      if (!m) {
        continue;
      }
      IRSroaFlatMember *fm = &members[m->member];
      if (fm->addr_count >= IR_ARRAY_COUNT(fm->addrs)) {
        continue;
      }
      fm->addrs[fm->addr_count].temp = insn->dest.name;
      fm->addrs[fm->addr_count].offset = 0;
      fm->addrs[fm->addr_count].valid = 1;
      fm->addr_count++;
      ir_sroa_hash_put(addr_hash, insn->dest.name, m->member, 0);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_BINARY && insn->text &&
        strcmp(insn->text, "+") == 0 && !insn->is_float &&
        insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name &&
        insn->lhs.kind == IR_OPERAND_TEMP && insn->lhs.name &&
        insn->rhs.kind == IR_OPERAND_INT) {
      const IRSroaHashEnt *base = ir_sroa_hash_get(addr_hash, insn->lhs.name);
      if (!base) {
        continue;
      }
      IRSroaFlatMember *fm = &members[base->member];
      if (fm->addr_count >= IR_ARRAY_COUNT(fm->addrs)) {
        continue;
      }
      long long off = base->offset + insn->rhs.int_value;
      fm->addrs[fm->addr_count].temp = insn->dest.name;
      fm->addrs[fm->addr_count].offset = off;
      fm->addrs[fm->addr_count].valid = 1;
      fm->addr_count++;
      ir_sroa_hash_put(addr_hash, insn->dest.name, base->member, off);
    }
  }
}

/* Build a scalar name "<member>$<offset>" into a fresh allocation. */
static char *ir_sroa_scalar_name(const char *member, long long offset) {
  int len = snprintf(NULL, 0, "%s$%lld", member, offset);
  char *s = (char *)malloc((size_t)len + 1);
  if (s) {
    snprintf(s, (size_t)len + 1, "%s$%lld", member, offset);
  }
  return s;
}

/* Transform every collected group in a single rebuild of the instruction
 * stream. Returns 1 on success, 0 on allocation failure (stream left intact
 * on failure). */
static int ir_sroa_transform_all(IRFunction *function,
                                 const IRSroaFlatMember *members,
                                 size_t member_count,
                                 const IRSroaLayout *layouts,
                                 const IRSroaHash *member_hash,
                                 const IRSroaHash *addr_hash) {
  IRInstructionVector vec = {0};
  int ok = 1;
  (void)member_count;
  if (!ir_instruction_vector_reserve(&vec, function->instruction_count)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count && ok; i++) {
    IRInstruction *insn = &function->instructions[i];

    /* Replace each member's aggregate decl with per-slot scalar decls. */
    if (insn->op == IR_OP_DECLARE_LOCAL &&
        insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name) {
      const IRSroaHashEnt *me = ir_sroa_hash_get(member_hash, insn->dest.name);
      if (me && members[me->member].decl_index == i) {
        const IRSroaFlatMember *fm = &members[me->member];
        const IRSroaSlot *slots = layouts[fm->layout].slots;
        size_t slot_count = layouts[fm->layout].slot_count;
        for (size_t s = 0; s < slot_count && ok; s++) {
          IRInstruction decl = {0};
          decl.op = IR_OP_DECLARE_LOCAL;
          decl.location = insn->location;
          char *nm = ir_sroa_scalar_name(fm->name, slots[s].offset);
          decl.dest = nm ? ir_operand_symbol(nm) : ir_operand_none();
          {
            const MtlcType *field = NULL;
            const MtlcType *aggregate = insn->value_type;
            for (size_t f = 0; aggregate && aggregate->field_types &&
                               aggregate->field_offsets &&
                               f < aggregate->field_count;
                 f++) {
              if ((long long)aggregate->field_offsets[f] == slots[s].offset &&
                  aggregate->field_types[f] &&
                  aggregate->field_types[f]->kind == MTLC_TYPE_POINTER &&
                  aggregate->field_types[f]->base_type &&
                  aggregate->field_types[f]->base_type->kind <= MTLC_TYPE_BFLOAT16 &&
                  aggregate->field_types[f]->name &&
                  (long long)aggregate->field_types[f]->size == slots[s].size) {
                field = aggregate->field_types[f];
                break;
              }
            }
            if (field) {
              decl.text = mettle_strdup(field->name);
              decl.value_type = (MtlcType *)field;
            } else {
              decl.text = mettle_strdup(ir_builtin_scalar_type_for_slot(
                  slots[s].size, slots[s].is_float, slots[s].float_bits,
                  slots[s].is_unsigned, slots[s].alias_class));
            }
          }
          free(nm);
          if (!decl.dest.name || !decl.text ||
              !ir_instruction_vector_append_move(&vec, &decl)) {
            ir_instruction_destroy_storage(&decl);
            ok = 0;
          }
        }
        continue;
      }
    }

    /* Drop address-temp producers belonging to any member. */
    if ((insn->op == IR_OP_ADDRESS_OF || insn->op == IR_OP_BINARY) &&
        insn->dest.kind == IR_OPERAND_TEMP) {
      if (ir_sroa_hash_get(addr_hash, insn->dest.name)) {
        continue;
      }
    }

    /* Whole-aggregate copy between two members -> per-slot scalar assigns. */
    if (insn->op == IR_OP_ASSIGN && insn->dest.kind == IR_OPERAND_SYMBOL &&
        insn->dest.name && insn->lhs.kind == IR_OPERAND_SYMBOL &&
        insn->lhs.name) {
      const IRSroaHashEnt *de = ir_sroa_hash_get(member_hash, insn->dest.name);
      const IRSroaHashEnt *se = ir_sroa_hash_get(member_hash, insn->lhs.name);
      /* Copy partners always land in the same group (a whole-aggregate copy
       * makes them partners, and a group only transforms when every partner
       * analyzed clean), so both sides share one layout. */
      if (de && se &&
          members[de->member].layout == members[se->member].layout) {
        const char *dst_m = members[de->member].name;
        const char *src_m = members[se->member].name;
        const IRSroaSlot *slots = layouts[members[de->member].layout].slots;
        size_t slot_count = layouts[members[de->member].layout].slot_count;
        for (size_t s = 0; s < slot_count && ok; s++) {
          IRInstruction a = {0};
          a.op = IR_OP_ASSIGN;
          a.location = insn->location;
          a.is_float = slots[s].is_float;
          a.float_bits = slots[s].float_bits;
          char *dn = ir_sroa_scalar_name(dst_m, slots[s].offset);
          char *sn = ir_sroa_scalar_name(src_m, slots[s].offset);
          a.dest = dn ? ir_operand_symbol(dn) : ir_operand_none();
          a.lhs = sn ? ir_operand_symbol(sn) : ir_operand_none();
          if (a.dest.name) {
            a.dest.float_bits = slots[s].float_bits;
          }
          if (a.lhs.name) {
            a.lhs.float_bits = slots[s].float_bits;
          }
          free(dn);
          free(sn);
          if (!a.dest.name || !a.lhs.name ||
              !ir_instruction_vector_append_move(&vec, &a)) {
            ir_instruction_destroy_storage(&a);
            ok = 0;
          }
        }
        continue;
      }
    }

    /* LOAD via a member address temp -> ASSIGN dest <- scalar. */
    if (insn->op == IR_OP_LOAD && insn->lhs.kind == IR_OPERAND_TEMP) {
      const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->lhs.name);
      if (ae) {
        long long off = ae->offset;
        const IRSroaFlatMember *owner = &members[ae->member];
        const IRSroaSlot *slot =
            ir_sroa_find_const_slot(layouts[owner->layout].slots,
                                    layouts[owner->layout].slot_count, off);
        if (!slot) {
          ok = 0;
          continue;
        }
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = insn->location;
        assign.is_float = slot->is_float;
        assign.float_bits = slot->float_bits;
        char *nm = ir_sroa_scalar_name(owner->name, off);
        if (!nm || !ir_operand_clone(&insn->dest, &assign.dest)) {
          free(nm);
          ir_instruction_destroy_storage(&assign);
          ok = 0;
        } else {
          assign.lhs = ir_operand_symbol(nm);
          assign.lhs.float_bits = slot->float_bits;
          free(nm);
          if (!assign.lhs.name ||
              !ir_instruction_vector_append_move(&vec, &assign)) {
            ir_instruction_destroy_storage(&assign);
            ok = 0;
          }
        }
        continue;
      }
    }

    /* STORE via a member address temp -> ASSIGN scalar <- value. */
    if (insn->op == IR_OP_STORE && insn->dest.kind == IR_OPERAND_TEMP) {
      const IRSroaHashEnt *ae = ir_sroa_hash_get(addr_hash, insn->dest.name);
      if (ae) {
        long long off = ae->offset;
        const IRSroaFlatMember *owner = &members[ae->member];
        const IRSroaSlot *slot =
            ir_sroa_find_const_slot(layouts[owner->layout].slots,
                                    layouts[owner->layout].slot_count, off);
        if (!slot) {
          ok = 0;
          continue;
        }
        IRInstruction assign = {0};
        assign.op = IR_OP_ASSIGN;
        assign.location = insn->location;
        assign.is_float = slot->is_float;
        assign.float_bits = slot->float_bits;
        char *nm = ir_sroa_scalar_name(owner->name, off);
        assign.dest = nm ? ir_operand_symbol(nm) : ir_operand_none();
        if (assign.dest.name) {
          assign.dest.float_bits = slot->float_bits;
        }
        free(nm);
        if (!assign.dest.name || !ir_operand_clone(&insn->lhs, &assign.lhs)) {
          ir_instruction_destroy_storage(&assign);
          ok = 0;
        } else if (!ir_instruction_vector_append_move(&vec, &assign)) {
          ir_instruction_destroy_storage(&assign);
          ok = 0;
        }
        continue;
      }
    }

    /* Everything else: MOVE verbatim. append_move neutralizes the source, so
     * the final cleanup sweep only destroys replaced instructions. (On the
     * OOM failure path the function body is left gutted, but a pass failure
     * aborts compilation before anything reads it again.) */
    if (!ir_instruction_vector_append_move(&vec, insn)) {
      ok = 0;
    }
  }

  if (!ok) {
    ir_instruction_vector_destroy(&vec);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    ir_instruction_destroy_storage(&function->instructions[i]);
  }
  free(function->instructions);
  function->instructions = vec.items;
  function->instruction_count = vec.count;
  function->instruction_capacity = vec.capacity;
  return 1;
}

/* Is `t` a builtin scalar type name (so the local is not an aggregate)? */
static int ir_sroa_is_scalar_type_name(const char *t) {
  return t && (strcmp(t, "int8") == 0 || strcmp(t, "int16") == 0 ||
               strcmp(t, "int32") == 0 || strcmp(t, "int64") == 0 ||
               strcmp(t, "uint8") == 0 || strcmp(t, "uint16") == 0 ||
               strcmp(t, "uint32") == 0 || strcmp(t, "uint64") == 0 ||
               strcmp(t, "float32") == 0 || strcmp(t, "float64") == 0 ||
               strcmp(t, "float16") == 0 || strcmp(t, "bfloat16") == 0 ||
               strcmp(t, "bool") == 0);
}

/* ---- whole-function analysis -------------------------------------------
 *
 * One record per aggregate DECLARE_LOCAL; a fixed number of whole-function
 * sweeps analyzes every record simultaneously (the old per-symbol analyzer
 * swept the function three times PER aggregate, which dominated the pass on
 * heavily inlined functions). Eligibility semantics mirror the old analyzer:
 * flags only ever get cleared, so marking a record ineligible mid-sweep and
 * carrying on is equivalent to the old early-out. */
typedef struct {
  const char *name;
  size_t decl_index;
  int eligible;  /* clean field-access discipline so far */
  int comp_fail; /* self-copy / partner without an aggregate decl: the whole
                    connected component must be abandoned */
  IRSroaSlot slots[IR_SROA_MAX_SLOTS];
  size_t slot_count;
  size_t partners[IR_SROA_MAX_GROUP]; /* record indices, copy-linked */
  size_t partner_count;
  size_t addr_count;
  int visited; /* component walk state */
} IRSroaRec;

static void ir_sroa_rec_add_partner(IRSroaRec *recs, size_t owner,
                                    size_t other) {
  IRSroaRec *rec = &recs[owner];
  for (size_t p = 0; p < rec->partner_count; p++) {
    if (rec->partners[p] == other) {
      return;
    }
  }
  if (rec->partner_count >= IR_SROA_MAX_GROUP) {
    rec->comp_fail = 1;
    rec->eligible = 0;
    recs[other].comp_fail = 1;
    recs[other].eligible = 0;
    return;
  }
  rec->partners[rec->partner_count++] = other;
}

/* Register an address temp for `owner`; a duplicate temp name across records
 * cannot be attributed to a unique owner, so both records decline. */
static void ir_sroa_rec_add_addr(IRSroaRec *recs, size_t owner,
                                 IRSroaHash *addr_hash, const char *temp,
                                 long long offset) {
  IRSroaRec *rec = &recs[owner];
  if (rec->addr_count >= IR_SROA_MAX_SLOTS * 2) {
    rec->eligible = 0;
    return;
  }
  const IRSroaHashEnt *dup = ir_sroa_hash_get(addr_hash, temp);
  if (dup) {
    recs[dup->member].eligible = 0;
    rec->eligible = 0;
    return;
  }
  ir_sroa_hash_put(addr_hash, temp, owner, offset);
  rec->addr_count++;
}

int ir_sroa_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  /* Each round analyzes every aggregate at once, then rewrites every
   * transformable group in a single rebuild. A second round only finds work
   * if the first round's rewrite exposed new opportunities. */
  for (int iter = 0; iter < 32; iter++) {
    /* ---- sweep 0: one record per aggregate DECLARE_LOCAL ---- */
    IRSroaRec *recs = NULL;
    size_t rec_count = 0, rec_cap = 0;
    IRSroaHash rec_hash = {0};
    IRSroaHash addr_hash = {0};

    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *insn = &function->instructions[i];
      if (insn->op != IR_OP_DECLARE_LOCAL ||
          insn->dest.kind != IR_OPERAND_SYMBOL || !insn->dest.name ||
          !insn->text || ir_sroa_is_scalar_type_name(insn->text)) {
        continue;
      }
      if (rec_count >= rec_cap) {
        size_t nc = rec_cap ? rec_cap * 2 : 16;
        IRSroaRec *grown = (IRSroaRec *)realloc(recs, nc * sizeof(IRSroaRec));
        if (!grown) {
          free(recs);
          return 0;
        }
        recs = grown;
        rec_cap = nc;
      }
      IRSroaRec *rec = &recs[rec_count];
      memset(rec, 0, sizeof(*rec));
      rec->name = insn->dest.name;
      rec->decl_index = i;
      rec->eligible = 1;
      rec_count++;
    }
    if (rec_count == 0) {
      free(recs);
      break;
    }
    if (!ir_sroa_hash_init(&rec_hash, rec_count) ||
        !ir_sroa_hash_init(&addr_hash, rec_count * IR_SROA_MAX_SLOTS * 2)) {
      free(rec_hash.ents);
      free(addr_hash.ents);
      free(recs);
      return 0;
    }
    for (size_t r = 0; r < rec_count; r++) {
      const IRSroaHashEnt *dup = ir_sroa_hash_get(&rec_hash, recs[r].name);
      if (dup) {
        /* Duplicate declaration of the same name: the old analyzer saw the
         * other DECLARE_LOCAL as a bare-symbol reference and declined. */
        recs[dup->member].eligible = 0;
        recs[r].eligible = 0;
        continue;
      }
      ir_sroa_hash_put(&rec_hash, recs[r].name, r, 0);
    }

    /* ---- sweep 1: address-of bases, whole-aggregate copies, escapes ---- */
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *insn = &function->instructions[i];
      if (insn->op == IR_OP_ADDRESS_OF &&
          insn->lhs.kind == IR_OPERAND_SYMBOL && insn->lhs.name &&
          insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name) {
        const IRSroaHashEnt *m = ir_sroa_hash_get(&rec_hash, insn->lhs.name);
        if (m) {
          ir_sroa_rec_add_addr(recs, m->member, &addr_hash, insn->dest.name,
                               0);
          continue;
        }
      }
      if (insn->op == IR_OP_DECLARE_LOCAL &&
          insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name) {
        const IRSroaHashEnt *m = ir_sroa_hash_get(&rec_hash, insn->dest.name);
        if (m && recs[m->member].decl_index == i) {
          continue; /* the record's own declaration */
        }
      }
      /* Whole-aggregate copy: ASSIGN where both sides are bare symbols and at
       * least one is a record. */
      if (insn->op == IR_OP_ASSIGN && insn->dest.kind == IR_OPERAND_SYMBOL &&
          insn->dest.name && insn->lhs.kind == IR_OPERAND_SYMBOL &&
          insn->lhs.name) {
        const IRSroaHashEnt *de = ir_sroa_hash_get(&rec_hash, insn->dest.name);
        const IRSroaHashEnt *se = ir_sroa_hash_get(&rec_hash, insn->lhs.name);
        if (de || se) {
          if (strcmp(insn->dest.name, insn->lhs.name) == 0) {
            /* Self-copy is degenerate; decline to be safe. */
            recs[(de ? de : se)->member].eligible = 0;
            continue;
          }
          if (de && se) {
            ir_sroa_rec_add_partner(recs, de->member, se->member);
            ir_sroa_rec_add_partner(recs, se->member, de->member);
          } else {
            /* Partner is a param/return temp or scalar, not an aggregate
             * local: the old group builder abandoned the whole group. */
            recs[(de ? de : se)->member].comp_fail = 1;
          }
          continue;
        }
      }
      /* Any other reference to a record's bare symbol escapes. */
      if (insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name) {
        const IRSroaHashEnt *m = ir_sroa_hash_get(&rec_hash, insn->dest.name);
        if (m) {
          recs[m->member].eligible = 0;
        }
      }
      if (insn->lhs.kind == IR_OPERAND_SYMBOL && insn->lhs.name) {
        const IRSroaHashEnt *m = ir_sroa_hash_get(&rec_hash, insn->lhs.name);
        if (m) {
          recs[m->member].eligible = 0;
        }
      }
      if (insn->rhs.kind == IR_OPERAND_SYMBOL && insn->rhs.name) {
        const IRSroaHashEnt *m = ir_sroa_hash_get(&rec_hash, insn->rhs.name);
        if (m) {
          recs[m->member].eligible = 0;
        }
      }
      for (size_t a = 0; a < insn->argument_count; a++) {
        const IROperand *arg = &insn->arguments[a];
        if (arg->kind == IR_OPERAND_SYMBOL && arg->name) {
          const IRSroaHashEnt *m = ir_sroa_hash_get(&rec_hash, arg->name);
          if (m) {
            recs[m->member].eligible = 0;
          }
        }
      }
    }

    /* ---- sweep 2: `base + CONST` derived address temps ---- */
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *insn = &function->instructions[i];
      if (insn->op == IR_OP_BINARY && insn->text &&
          strcmp(insn->text, "+") == 0 && !insn->is_float &&
          insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name &&
          insn->lhs.kind == IR_OPERAND_TEMP && insn->lhs.name &&
          insn->rhs.kind == IR_OPERAND_INT) {
        const IRSroaHashEnt *base =
            ir_sroa_hash_get(&addr_hash, insn->lhs.name);
        if (base) {
          ir_sroa_rec_add_addr(recs, base->member, &addr_hash,
                               insn->dest.name,
                               base->offset + insn->rhs.int_value);
        }
      }
    }

    /* ---- sweep 3: validate every address-temp use, collect field slots ----
     * A use must be the producer, a LOAD address (lhs), a STORE address
     * (dest), or a recorded derivation; anything else escapes. */
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *insn = &function->instructions[i];
      if ((insn->op == IR_OP_ADDRESS_OF || insn->op == IR_OP_BINARY) &&
          insn->dest.kind == IR_OPERAND_TEMP &&
          ir_sroa_hash_get(&addr_hash, insn->dest.name)) {
        continue; /* producer */
      }
      if (insn->op == IR_OP_LOAD) {
        const IRSroaHashEnt *ae = ir_sroa_hash_get(&addr_hash, insn->lhs.name);
        if (ae) {
          IRSroaRec *rec = &recs[ae->member];
          if (ir_operand_is_temp_named(&insn->dest, insn->lhs.name) ||
              ir_operand_is_temp_named(&insn->rhs, insn->lhs.name)) {
            rec->eligible = 0;
            continue;
          }
          /* Another record's address temp in a non-address slot is a value
           * use of that address (e.g. a block copy): that record escapes. */
          if (insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name) {
            const IRSroaHashEnt *o =
                ir_sroa_hash_get(&addr_hash, insn->dest.name);
            if (o) {
              recs[o->member].eligible = 0;
            }
          }
          if (insn->rhs.kind == IR_OPERAND_TEMP && insn->rhs.name) {
            const IRSroaHashEnt *o =
                ir_sroa_hash_get(&addr_hash, insn->rhs.name);
            if (o) {
              recs[o->member].eligible = 0;
            }
          }
          int size = (insn->rhs.kind == IR_OPERAND_INT)
                         ? (int)insn->rhs.int_value
                         : 8;
          if (!ir_sroa_scalar_access_width(size)) {
            /* Wider than a machine word: a block copy, not a field access.
             * Splitting the record would turn the copy into a one-word
             * assignment and drop the rest of the value. */
            rec->eligible = 0;
            continue;
          }
          if (!ir_sroa_note_slot(rec->slots, &rec->slot_count, ae->offset,
                                 size, insn->is_float, insn->float_bits,
                                 insn->is_unsigned, insn->alias_class)) {
            /* Mixed-width / mixed-class access of one offset: decline. */
            rec->eligible = 0;
          }
          continue;
        }
      }
      if (insn->op == IR_OP_STORE) {
        const IRSroaHashEnt *ae =
            ir_sroa_hash_get(&addr_hash, insn->dest.name);
        if (ae) {
          IRSroaRec *rec = &recs[ae->member];
          if (ir_operand_is_temp_named(&insn->lhs, insn->dest.name)) {
            rec->eligible = 0; /* address stored as a value -> escapes */
            continue;
          }
          /* Storing another record's address temp as the VALUE (whole-struct
           * block copy `*%t_addr <- %s_addr [n]`) is a value use of that
           * address: that record escapes. */
          if (insn->lhs.kind == IR_OPERAND_TEMP && insn->lhs.name) {
            const IRSroaHashEnt *o =
                ir_sroa_hash_get(&addr_hash, insn->lhs.name);
            if (o) {
              recs[o->member].eligible = 0;
            }
          }
          if (insn->rhs.kind == IR_OPERAND_TEMP && insn->rhs.name) {
            const IRSroaHashEnt *o =
                ir_sroa_hash_get(&addr_hash, insn->rhs.name);
            if (o) {
              recs[o->member].eligible = 0;
            }
          }
          int size = (insn->rhs.kind == IR_OPERAND_INT)
                         ? (int)insn->rhs.int_value
                         : 8;
          if (!ir_sroa_scalar_access_width(size)) {
            /* A block copy into this record (its whole value arriving from
             * somewhere else), not a field store. */
            rec->eligible = 0;
            continue;
          }
          if (!ir_sroa_note_slot(rec->slots, &rec->slot_count, ae->offset,
                                 size, insn->is_float, insn->float_bits,
                                 insn->is_unsigned, insn->alias_class)) {
            rec->eligible = 0;
          }
          continue;
        }
      }
      /* Any other instruction must not touch an address temp at all. */
      if (insn->dest.kind == IR_OPERAND_TEMP && insn->dest.name) {
        const IRSroaHashEnt *ae =
            ir_sroa_hash_get(&addr_hash, insn->dest.name);
        if (ae) {
          recs[ae->member].eligible = 0;
        }
      }
      if (insn->lhs.kind == IR_OPERAND_TEMP && insn->lhs.name) {
        const IRSroaHashEnt *ae = ir_sroa_hash_get(&addr_hash, insn->lhs.name);
        if (ae) {
          recs[ae->member].eligible = 0;
        }
      }
      if (insn->rhs.kind == IR_OPERAND_TEMP && insn->rhs.name) {
        const IRSroaHashEnt *ae = ir_sroa_hash_get(&addr_hash, insn->rhs.name);
        if (ae) {
          recs[ae->member].eligible = 0;
        }
      }
      for (size_t a = 0; a < insn->argument_count; a++) {
        const IROperand *arg = &insn->arguments[a];
        if (arg->kind == IR_OPERAND_TEMP && arg->name) {
          const IRSroaHashEnt *ae = ir_sroa_hash_get(&addr_hash, arg->name);
          if (ae) {
            recs[ae->member].eligible = 0;
          }
        }
      }
    }

    for (size_t r = 0; r < rec_count; r++) {
      if (recs[r].addr_count == 0 || recs[r].slot_count == 0) {
        recs[r].eligible = 0;
      }
      ir_sroa_sort_slots(recs[r].slots, recs[r].slot_count);
    }
    static int sroa_debug = -1;
    if (sroa_debug < 0) {
      sroa_debug = getenv("METTLE_SROA_DEBUG") ? 1 : 0;
    }
    if (sroa_debug) {
      for (size_t r = 0; r < rec_count; r++) {
        fprintf(stderr,
                "SROA rec %zu name=%s decl=%zu elig=%d comp_fail=%d "
                "partners=%zu addrs=%zu slots=%zu\n",
                r, recs[r].name, recs[r].decl_index, recs[r].eligible,
                recs[r].comp_fail, recs[r].partner_count, recs[r].addr_count,
                recs[r].slot_count);
      }
    }
    free(addr_hash.ents);
    addr_hash.ents = NULL;
    addr_hash.bucket_count = 0;

    /* ---- connected components over copy-partner edges. Every member must be
     * eligible and share the root (lowest-decl) member's exact slot layout,
     * or the whole component is abandoned (correctness over coverage). ---- */
    IRSroaFlatMember *members = NULL;
    IRSroaLayout *layouts = NULL;
    size_t member_count = 0, member_cap = 0;
    size_t layout_count = 0, layout_cap = 0;
    size_t comp[IR_SROA_MAX_GROUP];
    size_t stack[IR_SROA_MAX_GROUP];
    int oom = 0;

    for (size_t r = 0; r < rec_count && !oom; r++) {
      if (recs[r].visited) {
        continue;
      }
      size_t comp_count = 0, stack_count = 0;
      int comp_ok = 1;
      recs[r].visited = 1;
      stack[stack_count++] = r;
      while (stack_count > 0) {
        size_t cur = stack[--stack_count];
        if (comp_count >= IR_SROA_MAX_GROUP) {
          comp_ok = 0;
          break;
        }
        comp[comp_count++] = cur;
        for (size_t p = 0; p < recs[cur].partner_count; p++) {
          size_t nxt = recs[cur].partners[p];
          if (!recs[nxt].visited) {
            recs[nxt].visited = 1;
            if (stack_count >= IR_SROA_MAX_GROUP) {
              comp_ok = 0;
              break;
            }
            stack[stack_count++] = nxt;
          }
        }
        if (!comp_ok) {
          break;
        }
      }
      if (comp_ok) {
        const IRSroaRec *seed = &recs[comp[0]];
        for (size_t c = 0; c < comp_count && comp_ok; c++) {
          const IRSroaRec *m = &recs[comp[c]];
          if (!m->eligible || m->comp_fail ||
              !ir_sroa_slots_match(seed->slots, seed->slot_count, m->slots,
                                   m->slot_count)) {
            comp_ok = 0;
          }
        }
      }
      if (!comp_ok) {
        continue;
      }

      /* Record the component: one shared layout + flattened members. */
      if (layout_count >= layout_cap) {
        size_t nc = layout_cap ? layout_cap * 2 : 8;
        IRSroaLayout *grown =
            (IRSroaLayout *)realloc(layouts, nc * sizeof(IRSroaLayout));
        if (!grown) {
          oom = 1;
          break;
        }
        layouts = grown;
        layout_cap = nc;
      }
      memcpy(layouts[layout_count].slots, recs[comp[0]].slots,
             sizeof(layouts[layout_count].slots));
      layouts[layout_count].slot_count = recs[comp[0]].slot_count;

      if (member_count + comp_count > member_cap) {
        size_t nc = member_cap ? member_cap * 2 : 16;
        while (nc < member_count + comp_count) {
          nc *= 2;
        }
        IRSroaFlatMember *grown = (IRSroaFlatMember *)realloc(
            members, nc * sizeof(IRSroaFlatMember));
        if (!grown) {
          oom = 1;
          break;
        }
        members = grown;
        member_cap = nc;
      }
      for (size_t c = 0; c < comp_count; c++) {
        IRSroaFlatMember *fm = &members[member_count++];
        fm->name = recs[comp[c]].name;
        fm->decl_index = recs[comp[c]].decl_index;
        fm->layout = layout_count;
        fm->addr_count = 0;
      }
      layout_count++;
    }

    free(rec_hash.ents);
    free(recs);
    if (oom) {
      free(members);
      free(layouts);
      return 0;
    }
    if (sroa_debug) {
      fprintf(stderr, "SROA fn=%s iter=%d members=%zu layouts=%zu\n",
              function->name ? function->name : "?", iter, member_count,
              layout_count);
    }
    if (member_count == 0) {
      free(members);
      free(layouts);
      break;
    }

    /* ---- index members, rebuild address temps, transform in one pass ---- */
    IRSroaHash member_hash = {0};
    IRSroaHash xform_addr_hash = {0};
    int rc = ir_sroa_hash_init(&member_hash, member_count) &&
             ir_sroa_hash_init(&xform_addr_hash,
                               member_count * IR_SROA_MAX_SLOTS * 2);
    if (rc) {
      for (size_t m = 0; m < member_count; m++) {
        ir_sroa_hash_put(&member_hash, members[m].name, m, 0);
      }
      ir_sroa_collect_all_addrs(function, members, &member_hash,
                                &xform_addr_hash);
      rc = ir_sroa_transform_all(function, members, member_count, layouts,
                                 &member_hash, &xform_addr_hash);
    }
    free(member_hash.ents);
    free(xform_addr_hash.ents);
    free(members);
    free(layouts);
    if (!rc) {
      return 0;
    }
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
