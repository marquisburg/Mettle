// AST->IR lowering: type / float-width / string-coercion utilities.
#include "ir_lowering_internal.h"
#include <limits.h>

int ir_type_is_cstring(Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "cstring") == 0;
}

/* A string flowing to an untyped address wants the same `.chars` load a
 * cstring destination gets: the record itself is not the address. */
int ir_type_is_rawptr(Type *type) {
  return type && type->kind == TYPE_POINTER && type->name &&
         strcmp(type->name, "rawptr") == 0;
}

int ir_expression_is_string(IRLoweringContext *context,
                                   ASTNode *expression) {
  Type *type = ir_infer_expression_type(context, expression);
  return type && type->kind == TYPE_STRING;
}

int ir_should_coerce_string_to_cstring(IRLoweringContext *context,
                                              Type *target_type,
                                              ASTNode *value_expression) {
  return (ir_type_is_cstring(target_type) || ir_type_is_rawptr(target_type)) &&
         ir_expression_is_string(context, value_expression);
}

/* An array flowing into a pointer is its base address. The type checker decays
 * `T[N]` into a `T*` parameter or binding, but a bare array name lowers to the
 * symbol naming the storage, so the consumer read the array's first eight
 * bytes and used them as the pointer: `fgets(buf, 64, get_stdin())` wrote
 * through whatever those bytes spelled, and a zeroed buffer spells null, which
 * reads as the call silently doing nothing.
 *
 * The test is on the DESTINATION. A bare array name anywhere else still means
 * the storage: the right side of `var b: T[N] = a;` copies bytes, and the GPU
 * emitters reject an address-of on a device local. */
int ir_should_decay_array_to_address(Type *target_type,
                                     ASTNode *value_expression) {
  return target_type && target_type->kind == TYPE_POINTER &&
         value_expression && value_expression->resolved_type &&
         value_expression->resolved_type->kind == TYPE_ARRAY;
}

/* An array flowing into a slice keeps its extent: the value becomes the pair
 * `{ &a[0], N }`, which is the whole difference between a slice and a pointer.
 * The length is the one the array's type carried, so nothing has to be trusted
 * about it afterwards. */
int ir_should_build_slice_from_array(Type *target_type,
                                     ASTNode *value_expression) {
  return target_type && target_type->kind == TYPE_SLICE &&
         value_expression && value_expression->resolved_type &&
         value_expression->resolved_type->kind == TYPE_ARRAY;
}

int ir_build_slice_operand_from_array(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      Type *array_type, Type *slice_type,
                                      SourceLocation location) {
  char *slice_name = NULL;
  IROperand array_address = ir_operand_none();
  IROperand slice_address = ir_operand_none();
  IROperand slot = ir_operand_none();
  IRInstruction store = {0};

  if (!context || !function || !value || !array_type || !slice_type ||
      value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }

  slice_name = ir_new_label_name(context, "slice");
  if (!slice_name ||
      !ir_emit_local_declaration(context, function, slice_name,
                                 slice_type->name, location)) {
    free(slice_name);
    return 0;
  }
  if (!ir_emit_address_of_symbol(context, function, value->name, location,
                                 &array_address) ||
      !ir_emit_address_of_symbol(context, function, slice_name, location,
                                 &slice_address)) {
    ir_operand_destroy(&array_address);
    ir_operand_destroy(&slice_address);
    free(slice_name);
    return 0;
  }

  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = ir_clone_operand_local(&slice_address);
  store.lhs = array_address;
  store.rhs = ir_operand_int(8);
  if (!ir_emit(context, function, &store)) {
    ir_operand_destroy(&store.dest);
    ir_operand_destroy(&array_address);
    ir_operand_destroy(&slice_address);
    free(slice_name);
    return 0;
  }
  ir_operand_destroy(&store.dest);
  ir_operand_destroy(&array_address);

  if (type_view_rank(slice_type) > 1) {
    size_t rank = type_view_rank(slice_type);
    Type *level = array_type;
    long long extents[16];
    long long stride = 1;
    if (rank > 16) {
      ir_operand_destroy(&slice_address);
      free(slice_name);
      return 0;
    }
    for (size_t k = 0; k < rank; k++) {
      if (!level || level->kind != TYPE_ARRAY) {
        ir_operand_destroy(&slice_address);
        free(slice_name);
        return 0;
      }
      extents[k] = (long long)level->array_size;
      level = level->base_type;
    }
    for (size_t k = 0; k < rank; k++) {
      IROperand extent = ir_operand_int(extents[k]);
      if (!ir_emit_store_word(context, function, &slice_address, 8 + 8 * k,
                              &extent, location)) {
        ir_operand_destroy(&slice_address);
        free(slice_name);
        return 0;
      }
    }
    for (size_t k = rank - 1; k > 0; k--) {
      IROperand lead;
      stride *= extents[k];
      lead = ir_operand_int(stride);
      if (!ir_emit_store_word(context, function, &slice_address,
                              8 + 8 * rank + 8 * (k - 1), &lead, location)) {
        ir_operand_destroy(&slice_address);
        free(slice_name);
        return 0;
      }
    }
    ir_operand_destroy(&slice_address);
    ir_operand_destroy(value);
    *value = ir_operand_symbol(slice_name);
    free(slice_name);
    return value->name != NULL;
  }

  if (!ir_emit_address_with_offset(context, function, &slice_address, 8,
                                   location, &slot)) {
    ir_operand_destroy(&slice_address);
    free(slice_name);
    return 0;
  }
  {
    IRInstruction length = {0};
    length.op = IR_OP_STORE;
    length.location = location;
    length.dest = slot;
    length.lhs = ir_operand_int((long long)array_type->array_size);
    length.rhs = ir_operand_int(8);
    if (!ir_emit(context, function, &length)) {
      ir_operand_destroy(&slot);
      ir_operand_destroy(&slice_address);
      free(slice_name);
      return 0;
    }
  }
  ir_operand_destroy(&slot);
  ir_operand_destroy(&slice_address);

  ir_operand_destroy(value);
  *value = ir_operand_symbol(slice_name);
  free(slice_name);
  return value->name != NULL;
}

/* A device storage binding already holds the address of its storage: the
   allocation produced a pointer, and there is no separate object to take the
   address of. Decaying it is therefore the identity. */
static int ir_symbol_is_address_space_allocation(const IRFunction *function,
                                                 const char *name) {
  if (!function || !name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_ADDRESS_SPACE_ALLOC &&
        instruction->dest.kind == IR_OPERAND_SYMBOL && instruction->dest.name &&
        strcmp(instruction->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ir_decay_array_operand_to_address(IRLoweringContext *context,
                                      IRFunction *function, IROperand *value,
                                      SourceLocation location) {
  IROperand address = ir_operand_none();

  if (!context || !function || !value ||
      value->kind != IR_OPERAND_SYMBOL || !value->name) {
    return 0;
  }
  if (ir_symbol_is_address_space_allocation(function, value->name)) {
    return 1;
  }
  if (!ir_emit_address_of_symbol(context, function, value->name, location,
                                 &address)) {
    return 0;
  }
  ir_operand_destroy(value);
  *value = address;
  return 1;
}

int ir_coerce_string_operand_to_cstring(IRLoweringContext *context,
                                               IRFunction *function,
                                               IROperand *value,
                                               SourceLocation location) {
  if (!context || !function || !value || value->kind == IR_OPERAND_NONE) {
    return 0;
  }

  IROperand destination = ir_operand_none();
  if (!ir_make_temp_operand(context, &destination)) {
    return 0;
  }

  IRInstruction load_chars = {0};
  load_chars.op = IR_OP_LOAD;
  load_chars.location = location;
  load_chars.dest = destination;
  load_chars.lhs = *value;
  load_chars.rhs = ir_operand_int(8);
  if (!ir_emit(context, function, &load_chars)) {
    ir_operand_destroy(&destination);
    return 0;
  }

  ir_operand_destroy(value);
  *value = destination;
  return 1;
}

/* Resolve a named type via the type_checker (works even after scope pop). */
Type *ir_resolve_named_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->type_checker || !name) {
    return NULL;
  }
  return type_checker_get_type_by_name(context->type_checker, name);
}

/* Look up a symbol's type from the symbol's name; falls back to NULL once the
 * scope is gone. Callers must handle NULL. */
Type *ir_lookup_symbol_type(IRLoweringContext *context,
                                   const char *name) {
  if (!context || !context->symbol_table || !name) {
    return NULL;
  }
  Symbol *sym = symbol_table_lookup(context->symbol_table, name);
  return sym ? sym->type : NULL;
}


int ir_expression_is_floating(IRLoweringContext *context,
                                     ASTNode *expression) {
  if (!context || !context->type_checker || !expression) {
    return 0;
  }

  Type *type = type_checker_infer_type(context->type_checker, expression);
  if (!type) {
    return 0;
  }

  return type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT64 ||
         type->kind == TYPE_FLOAT16 || type->kind == TYPE_BFLOAT16;
}

/* True only for a true 8-byte float64. The backend's "known float64" path
 * reinterprets the loaded 64 bits via `movq xmm, r64`; that is correct for
 * float64 but wrong for float32 or integer-width types, so gate strictly. */
int ir_type_is_float64(Type *type) {
  return type && type->kind == TYPE_FLOAT64 && type->size == 8;
}

/* IEEE-754 width for a floating type: 32 for float32, otherwise 64. Callers
 * must already know the type is floating (use ir_type_is_float* / the type
 * checker). Returns 64 for NULL so non-float contexts get the safe default. */
int ir_type_float_bits(Type *type) {
  if (type && (type->kind == TYPE_FLOAT32 || type->kind == TYPE_FLOAT16 ||
               type->kind == TYPE_BFLOAT16)) {
    return 32;
  }
  return 64;
}

/* Float width for a named type (e.g. a declared variable / parameter type).
 * Returns 0 when the name does not resolve to a floating type, else 32/64. */
int ir_named_type_float_bits(IRLoweringContext *context,
                                    const char *type_name) {
  Type *type = NULL;
  if (!context || !context->type_checker || !type_name) {
    return 0;
  }
  type = type_checker_get_type_by_name(context->type_checker, type_name);
  if (!type || (type->kind != TYPE_FLOAT32 && type->kind != TYPE_FLOAT64 &&
                type->kind != TYPE_FLOAT16 && type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  return ir_type_float_bits(type);
}

/* Stamp a freshly produced float operand with the requested IEEE-754 width.
 * No-op for non-float operands or when bits is 0. When narrowing a float64
 * literal to float32, round the constant through float so the stored bits are
 * the true single-precision value, not a truncated double pattern. */
void ir_operand_apply_float_bits(IROperand *operand, int bits) {
  if (!operand || operand->kind != IR_OPERAND_FLOAT ||
      (bits != 32 && bits != 64)) {
    return;
  }
  if (bits == 32) {
    operand->float_value = (double)(float)operand->float_value;
  }
  operand->float_bits = bits;
}

/* Float width of a declared symbol (variable/parameter). 0 if not floating. */
int ir_symbol_float_bits(IRLoweringContext *context, const char *name) {
  Symbol *symbol = NULL;
  if (!context || !context->symbol_table || !name) {
    return 0;
  }
  symbol = symbol_table_lookup(context->symbol_table, name);
  if (!symbol || !symbol->type ||
      (symbol->type->kind != TYPE_FLOAT32 &&
       symbol->type->kind != TYPE_FLOAT64 &&
       symbol->type->kind != TYPE_FLOAT16 &&
       symbol->type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  return ir_type_float_bits(symbol->type);
}

/* Recover a local's declared float width (0/32/64) from the DECLARE_LOCAL the
 * lowering already emitted for it. The function-body symbol-table scope is
 * usually popped by lowering time, so symbol_table_lookup misses locals; the
 * emitted IR is the reliable record of a local's declared type name. Caller
 * should gate this on a floating RHS to avoid an O(n) scan on every assign. */
int ir_local_declared_float_bits(IRLoweringContext *context,
                                        const IRFunction *function,
                                        const char *name) {
  if (!context || !function || !name) {
    return 0;
  }
  for (size_t i = function->instruction_count; i-- > 0;) {
    const IRInstruction *insn = &function->instructions[i];
    if (insn->op == IR_OP_DECLARE_LOCAL &&
        insn->dest.kind == IR_OPERAND_SYMBOL && insn->dest.name &&
        insn->text && strcmp(insn->dest.name, name) == 0) {
      return ir_named_type_float_bits(context, insn->text);
    }
  }
  return 0;
}

/* Record, on an ASSIGN/STORE, the TARGET float precision (bits = 32/64) of the
 * destination. instruction->float_bits is the destination width; the source
 * value operand keeps its own width so the backend can detect a precision
 * mismatch (e.g. a float64 expression assigned to a float32 variable) and
 * emit the cvtsd2ss / cvtss2sd it needs. A bare float literal has no runtime
 * width, so re-round it to the target precision in place, no conversion is
 * required for it. No-op when bits is 0 (target is not floating). */
void ir_assign_apply_float_bits(IRInstruction *instruction,
                                       IROperand *value, int bits) {
  if (!instruction || bits == 0) {
    return;
  }
  instruction->is_float = 1;
  instruction->float_bits = (bits == 32) ? 32 : 64;
  if (value && value->kind == IR_OPERAND_FLOAT) {
    ir_operand_apply_float_bits(value, instruction->float_bits);
    instruction->lhs.float_bits = value->float_bits;
  } else if (value) {
    /* Preserve the value's own width; the backend converts if it differs
     * from instruction->float_bits. */
    instruction->lhs.float_bits = value->float_bits;
  }
}

/* Mark a LOAD instruction (and its destination temp) as floating when the
 * loaded type is float32/float64, recording the width. Backends key off this
 * to pick movss/cvtss* vs movsd/cvtsd* and 4- vs 8-byte memory access. */
/* Record WHICH class of value a load or store moves, alongside the float and
 * unsigned flags that only say how wide it is and how to extend it. The
 * whole-program alias analysis reads this to tell a slot holding a pointer
 * from a slot holding an integer of the same width, which no size can tell
 * apart. It goes in its own field: value_type feeds ABI classification and the
 * GPU emitters, which read it as the instruction's result type, and a load's
 * pointee type is a different thing. */
void ir_access_apply_alias_class(IRInstruction *access, Type *accessed_type) {
  if (!access || !accessed_type) {
    return;
  }
  if (accessed_type->is_volatile) {
    access->is_volatile = 1;
  }
  switch (accessed_type->kind) {
  case TYPE_POINTER:
  case TYPE_FUNCTION_POINTER:
  case TYPE_STRING:
    access->alias_class = IR_ALIAS_CLASS_POINTER;
    break;
  case TYPE_INT8:
  case TYPE_UINT8:
  case TYPE_BOOL:
    access->alias_class = IR_ALIAS_CLASS_I8;
    break;
  case TYPE_INT16:
  case TYPE_UINT16:
    access->alias_class = IR_ALIAS_CLASS_I16;
    break;
  case TYPE_INT32:
  case TYPE_UINT32:
    access->alias_class = IR_ALIAS_CLASS_I32;
    break;
  case TYPE_INT64:
  case TYPE_UINT64:
    access->alias_class = IR_ALIAS_CLASS_I64;
    break;
  case TYPE_FLOAT32:
    access->alias_class = IR_ALIAS_CLASS_F32;
    break;
  case TYPE_FLOAT64:
    access->alias_class = IR_ALIAS_CLASS_F64;
    break;
  case TYPE_FLOAT16:
    access->alias_class = IR_ALIAS_CLASS_F16;
    break;
  case TYPE_BFLOAT16:
    access->alias_class = IR_ALIAS_CLASS_BF16;
    break;
  default:
    /* Aggregates carry their members' storage; a whole-aggregate move is not
     * a typed scalar access and never disambiguates. */
    access->alias_class = IR_ALIAS_CLASS_NONE;
    break;
  }
}

void ir_load_apply_float_type(IRInstruction *load, Type *loaded_type) {
  if (!load || !loaded_type) {
    return;
  }
  if (loaded_type->kind != TYPE_FLOAT32 && loaded_type->kind != TYPE_FLOAT64 &&
      loaded_type->kind != TYPE_FLOAT16 && loaded_type->kind != TYPE_BFLOAT16) {
    return;
  }
  load->is_float = 1;
  load->float_bits = ir_type_float_bits(loaded_type);
  load->dest.float_bits = load->float_bits;
}

/* Record that a load reads an UNSIGNED integer, so the backend zero-extends it
 * (instead of the default sign-extension for a 4-byte load into a temp). Without
 * this a uint32 loaded from a uint32* lands in the register sign-extended, and
 * later 64-bit ops (compare/divide/(int64) widening) read the wrong value. */
void ir_load_apply_unsigned(IRInstruction *load, Type *loaded_type) {
  if (!load || !loaded_type) {
    return;
  }
  /* `char` and `bool` are unsigned bytes, so they belong here too. Leaving
   * char out sign-extended every byte from 0x80 up, but only when the load fed
   * an expression directly: `var c: char = s[1]` went through the declared
   * local and zero-extended, while `s[1] == 195` and `(int32)s[1]` answered
   * -61. Every non-ASCII byte of a UTF-8 string read the wrong way round
   * depending on whether it was assigned first. */
  if (loaded_type->kind == TYPE_UINT8 || loaded_type->kind == TYPE_UINT16 ||
      loaded_type->kind == TYPE_UINT32 || loaded_type->kind == TYPE_UINT64 ||
      loaded_type->kind == TYPE_CHAR || loaded_type->kind == TYPE_BOOL) {
    load->is_unsigned = 1;
  }
}

/* Resolve the float width of an expression via the type checker. Returns 0
 * when the expression is not floating, else 32 or 64. */
int ir_expression_float_bits(IRLoweringContext *context,
                                    ASTNode *expression) {
  Type *type = NULL;
  if (!context || !context->type_checker || !expression) {
    return 0;
  }
  type = type_checker_infer_type(context->type_checker, expression);
  if (!type || (type->kind != TYPE_FLOAT32 && type->kind != TYPE_FLOAT64 &&
                type->kind != TYPE_FLOAT16 && type->kind != TYPE_BFLOAT16)) {
    return 0;
  }
  return ir_type_float_bits(type);
}

int ir_binary_operator_is_comparison(const char *op) {
  return op && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0);
}

int ir_binary_expression_operation_float_bits(IRLoweringContext *context,
                                                    ASTNode *expression,
                                                    BinaryExpression *binary) {
  int expression_bits = ir_expression_float_bits(context, expression);
  int left_bits = 0;
  int right_bits = 0;

  if (expression_bits != 0) {
    return expression_bits;
  }
  if (!binary || !ir_binary_operator_is_comparison(binary->operator)) {
    return 0;
  }

  left_bits = ir_expression_float_bits(context, binary->left);
  right_bits = ir_expression_float_bits(context, binary->right);
  if (left_bits == 64 || right_bits == 64) {
    return 64;
  }
  if (left_bits == 32 || right_bits == 32) {
    return 32;
  }
  return 0;
}

int ir_type_storage_size(Type *type) {
  if (!type || type->size == 0) {
    return 8;
  }

  if (type->size == 1 || type->size == 2 || type->size == 4 ||
      type->size == 8) {
    return (int)type->size;
  }

  return 8;
}

/* Memory stride between consecutive elements in an array, must match
 * laid-out sizeof(element), including structs > 8 bytes. Prefer this over
 * ir_type_storage_size() for base + index * stride address math only. */
int ir_type_array_element_stride(Type *element_type) {
  if (!element_type || element_type->size == 0 ||
      element_type->size > (size_t)INT_MAX) {
    return 8;
  }
  return (int)element_type->size;
}

/* An unsigned integer type: `/`, `%`, `>>`, and the four orderings mean
 * something different on one than the signed evaluation the optimizer's
 * constant folder performs. */
int ir_type_is_unsigned_integer(Type *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

/* The type name a narrow integer result has to be brought back to, or NULL
 * when the value is already canonical. Only these operators can leave a
 * 64-bit register holding something outside the declared width: the rest
 * either cannot leave it (`/`, `%`, `>>`, `&`, `|`, `^`, the comparisons) or
 * do not produce an integer at all. The unary forms of `~` and `-` are here
 * for the same reason: complementing a uint8 in a 64-bit register sets 56 bits
 * the type does not have, and negating int8 -128 gives 128, which is not an
 * int8 either. */
/* Bit width of a narrow integer type, or 0 for anything else. A shift on one
 * of these runs in a 64-bit register, where the hardware masks the count to 6
 * bits and not to the type's own width. int64 shifts already read that way,
 * and the M0115 warning describes it, so a narrow shift masks its count to the
 * width the type actually has. Without it `x >> 32` on an int32 answered 0
 * while the same shift on an int64 answered x. */
int ir_narrow_integer_shift_bits(Type *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_UINT8:
    return 8;
  case TYPE_INT16:
  case TYPE_UINT16:
    return 16;
  case TYPE_INT32:
  case TYPE_UINT32:
    return 32;
  default:
    return 0;
  }
}

/* Does this unary applied to this constant land back inside `type_name`? Then
 * the value is already what the type says and no truncation is emitted, which
 * leaves a negative literal reading as the constant it is. */
int ir_unary_constant_fits(const char *type_name, const char *op,
                           long long value) {
  long long folded;
  if (!type_name || !op) {
    return 0;
  }
  if (strcmp(op, "-") == 0) {
    if (value == LLONG_MIN) {
      return 0;
    }
    folded = -value;
  } else if (strcmp(op, "~") == 0) {
    folded = ~value;
  } else {
    return 0;
  }
  if (strcmp(type_name, "int8") == 0) {
    return folded >= -128 && folded <= 127;
  }
  if (strcmp(type_name, "int16") == 0) {
    return folded >= -32768 && folded <= 32767;
  }
  if (strcmp(type_name, "int32") == 0) {
    return folded >= -2147483648LL && folded <= 2147483647LL;
  }
  if (strcmp(type_name, "uint8") == 0) {
    return folded >= 0 && folded <= 255;
  }
  if (strcmp(type_name, "uint16") == 0) {
    return folded >= 0 && folded <= 65535;
  }
  if (strcmp(type_name, "uint32") == 0) {
    return folded >= 0 && folded <= 4294967295LL;
  }
  return 0;
}

const char *ir_narrow_integer_result_type(Type *type, const char *op) {
  if (!type || !op) {
    return NULL;
  }
  if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0 &&
      strcmp(op, "<<") != 0 && strcmp(op, "~") != 0) {
    return NULL;
  }
  switch (type->kind) {
  case TYPE_INT8:
    return "int8";
  case TYPE_INT16:
    return "int16";
  case TYPE_INT32:
    return "int32";
  case TYPE_UINT8:
    return "uint8";
  case TYPE_UINT16:
    return "uint16";
  case TYPE_UINT32:
    return "uint32";
  default:
    return NULL;
  }
}

int ir_type_is_pointer(Type *type) {
  return type && type->kind == TYPE_POINTER && type->base_type;
}

Type *ir_infer_expression_type(IRLoweringContext *context,
                                      ASTNode *expression) {
  if (!context || !context->type_checker || !expression) {
    return NULL;
  }
  return type_checker_infer_type(context->type_checker, expression);
}
