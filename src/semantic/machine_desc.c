#include "machine_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *string_of(const ASTNode *node) {
  if (!node || node->type != AST_STRING_LITERAL || !node->data) {
    return NULL;
  }
  return ((const StringLiteral *)node->data)->value;
}

static int int_of(const ASTNode *node, long long *out) {
  if (!node || node->type != AST_NUMBER_LITERAL || !node->data) {
    return 0;
  }
  {
    const NumberLiteral *literal = (const NumberLiteral *)node->data;
    if (literal->is_float) {
      return 0;
    }
    *out = literal->int_value;
    return 1;
  }
}

static int copy_field(char *slot, size_t capacity, const ASTNode *node) {
  const char *text = string_of(node);
  if (!text || strlen(text) + 1 > capacity) {
    return 0;
  }
  memcpy(slot, text, strlen(text) + 1);
  return 1;
}

static int hex_digit(char c, int *out) {
  if (c >= '0' && c <= '9') {
    *out = c - '0';
    return 1;
  }
  if (c >= 'a' && c <= 'f') {
    *out = c - 'a' + 10;
    return 1;
  }
  if (c >= 'A' && c <= 'F') {
    *out = c - 'A' + 10;
    return 1;
  }
  return 0;
}

static int parse_encoding(MachineInsn *insn, const char *text, char *error,
                          size_t error_size) {
  size_t at = 0;
  insn->length = 0;
  insn->prefix = 0;
  while (text[at]) {
    if (text[at] == ' ') {
      at++;
      continue;
    }
    if (insn->length >= MACHINE_MAX_BYTES) {
      snprintf(error, error_size,
               "instruction '%s' encodes into more than %d bytes", insn->name,
               MACHINE_MAX_BYTES);
      return 0;
    }
    if (text[at] == '%') {
      int which = text[at + 1] - '0';
      if (which < 0 || which >= MACHINE_OPERANDS) {
        snprintf(error, error_size,
                 "instruction '%s' names the operand slot '%c', and a slot is "
                 "%%0, %%1 or %%2",
                 insn->name, text[at + 1] ? text[at + 1] : '?');
        return 0;
      }
      insn->slot[insn->length] = which;
      insn->bytes[insn->length] = 0;
      insn->length++;
      at += 2;
      continue;
    }
    {
      int high = 0;
      int low = 0;
      if (!hex_digit(text[at], &high) || !text[at + 1] ||
          !hex_digit(text[at + 1], &low)) {
        snprintf(error, error_size,
                 "instruction '%s' has an encoding that is not pairs of hex "
                 "digits and operand slots",
                 insn->name);
        return 0;
      }
      insn->slot[insn->length] = -1;
      insn->bytes[insn->length] = (unsigned char)(high * 16 + low);
      insn->length++;
      at += 2;
    }
  }
  while (insn->prefix < insn->length && insn->slot[insn->prefix] < 0) {
    insn->prefix++;
  }
  if (insn->length == 0 || insn->prefix == 0) {
    snprintf(error, error_size,
             "instruction '%s' has to start with at least one fixed byte, "
             "which is what a decoder matches on",
             insn->name);
    return 0;
  }
  return 1;
}

static int check_slots(MachineInsn *insn, char *error, size_t error_size) {
  int seen[MACHINE_OPERANDS];
  long long used = 0;
  for (int i = 0; i < MACHINE_OPERANDS; i++) {
    seen[i] = 0;
  }
  for (size_t i = 0; i < insn->length; i++) {
    if (insn->slot[i] >= 0) {
      seen[insn->slot[i]] = 1;
    }
  }
  for (int i = 0; i < MACHINE_OPERANDS; i++) {
    used += seen[i] ? 1 : 0;
  }
  if (used != insn->operands) {
    snprintf(error, error_size,
             "instruction '%s' says it takes %lld operand%s and its encoding "
             "carries %lld",
             insn->name, insn->operands, insn->operands == 1 ? "" : "s", used);
    return 0;
  }
  for (int i = 0; i < insn->operands; i++) {
    if (!seen[i]) {
      snprintf(error, error_size,
               "instruction '%s' takes %lld operands, so its encoding has to "
               "carry %%0 up to %%%lld with none skipped",
               insn->name, insn->operands, insn->operands - 1);
      return 0;
    }
  }
  return 1;
}

static int check_uses(MachineInsn *insn, const char *list, const char *what,
                      char *error, size_t error_size) {
  size_t at = 0;
  while (list[at]) {
    if (list[at] != '%') {
      at++;
      continue;
    }
    {
      int which = list[at + 1] - '0';
      if (which < 0 || which >= insn->operands) {
        snprintf(error, error_size,
                 "instruction '%s' says it %s the operand '%%%c', and it "
                 "encodes %lld operand%s",
                 insn->name, what, list[at + 1] ? list[at + 1] : '?',
                 insn->operands, insn->operands == 1 ? "" : "s");
        return 0;
      }
    }
    at += 2;
  }
  return 1;
}

static int read_row(ASTNode *row, MachineInsn *insn, char *error,
                    size_t error_size) {
  AggregateLiteral *literal =
      row && row->type == AST_AGGREGATE_LITERAL ? (AggregateLiteral *)row->data
                                                : NULL;
  const ASTNode *encoding = NULL;
  memset(insn, 0, sizeof(*insn));
  insn->operands = -1;
  insn->location = row ? row->location : (SourceLocation){0};
  if (!literal || !literal->is_struct || !literal->field_names) {
    snprintf(error, error_size,
             "an instruction is written as `{ name: ..., encoding: ..., "
             "operands: ..., reads: ..., writes: ..., semantics: ... }`");
    return 0;
  }
  for (size_t i = 0; i < literal->element_count; i++) {
    const char *field = literal->field_names[i];
    ASTNode *value = literal->elements[i];
    int ok = 1;
    if (!field) {
      continue;
    }
    if (strcmp(field, "name") == 0) {
      ok = copy_field(insn->name, sizeof(insn->name), value);
    } else if (strcmp(field, "semantics") == 0) {
      ok = copy_field(insn->semantics, sizeof(insn->semantics), value);
    } else if (strcmp(field, "reads") == 0) {
      ok = copy_field(insn->reads, sizeof(insn->reads), value);
    } else if (strcmp(field, "writes") == 0) {
      ok = copy_field(insn->writes, sizeof(insn->writes), value);
    } else if (strcmp(field, "operands") == 0) {
      ok = int_of(value, &insn->operands);
    } else if (strcmp(field, "encoding") == 0) {
      encoding = value;
      ok = string_of(value) != NULL;
    }
    if (!ok) {
      snprintf(error, error_size,
               "the '%s' of an instruction is not a literal the compiler can "
               "read while compiling",
               field);
      return 0;
    }
  }
  if (!insn->name[0] || !insn->semantics[0] || !encoding) {
    snprintf(error, error_size,
             "an instruction needs a 'name', an 'encoding' and a 'semantics'");
    return 0;
  }
  if (insn->operands < 0 || insn->operands > MACHINE_OPERANDS) {
    snprintf(error, error_size,
             "instruction '%s' takes %lld operands, and an instruction takes "
             "between zero and %d",
             insn->name, insn->operands, MACHINE_OPERANDS);
    return 0;
  }
  if (!parse_encoding(insn, string_of(encoding), error, error_size)) {
    return 0;
  }
  if (!check_slots(insn, error, error_size)) {
    return 0;
  }
  return check_uses(insn, insn->reads, "reads", error, error_size) &&
         check_uses(insn, insn->writes, "writes", error, error_size);
}

static int names_machine(const char *type_name) {
  const char *bracket = NULL;
  size_t length = 0;
  if (!type_name) {
    return 0;
  }
  bracket = strchr(type_name, '[');
  length = bracket ? (size_t)(bracket - type_name) : strlen(type_name);
  if (length == 11 && strncmp(type_name, "MachineInsn", 11) == 0) {
    return 1;
  }
  return length > 12 &&
         strncmp(type_name + length - 12, "_MachineInsn", 12) == 0;
}

static int module_declares_function(const Program *program, const char *name) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    FunctionDeclaration *function = NULL;
    if (!declaration || declaration->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    function = (FunctionDeclaration *)declaration->data;
    if (function && function->name && strcmp(function->name, name) == 0) {
      return (int)function->parameter_count == MACHINE_OPERANDS ? 1 : -1;
    }
  }
  return 0;
}

int machine_desc_read(ASTNode *program_node, MachineDesc *out, char *error,
                      size_t error_size, SourceLocation *error_at) {
  Program *program = NULL;
  ASTNode *found = NULL;
  VarDeclaration *variable = NULL;
  AggregateLiteral *rows = NULL;
  memset(out, 0, sizeof(*out));
  error[0] = '\0';
  if (error_at) {
    memset(error_at, 0, sizeof(*error_at));
  }
  if (!program_node || program_node->type != AST_PROGRAM) {
    snprintf(error, error_size, "no program to read a machine out of");
    return 0;
  }
  program = (Program *)program_node->data;
  for (size_t i = 0; program && i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    VarDeclaration *candidate = NULL;
    if (!declaration || declaration->type != AST_VAR_DECLARATION) {
      continue;
    }
    candidate = (VarDeclaration *)declaration->data;
    if (!candidate || !names_machine(candidate->type_name)) {
      continue;
    }
    if (found) {
      snprintf(error, error_size,
               "this file describes more than one machine; a machine is one "
               "`const` of MachineInsn rows");
      if (error_at) {
        *error_at = declaration->location;
      }
      return 0;
    }
    found = declaration;
    variable = candidate;
  }
  if (!found) {
    snprintf(error, error_size,
             "this file describes no machine; a machine is a `const` of "
             "std/machine's MachineInsn");
    return 0;
  }
  if (error_at) {
    *error_at = found->location;
  }
  out->location = found->location;
  snprintf(out->name, sizeof(out->name), "%s",
           variable->name ? variable->name : "?");
  if (!variable->is_const) {
    snprintf(error, error_size,
             "'%s' is read while compiling, so it has to be a `const`",
             out->name);
    return 0;
  }
  rows = variable->initializer &&
                 variable->initializer->type == AST_AGGREGATE_LITERAL
             ? (AggregateLiteral *)variable->initializer->data
             : NULL;
  if (!rows || rows->is_struct || rows->element_count == 0) {
    snprintf(error, error_size,
             "'%s' is written as an array literal of instructions", out->name);
    return 0;
  }
  if (rows->element_count > MACHINE_MAX_INSNS) {
    snprintf(error, error_size, "'%s' describes more than %d instructions",
             out->name, MACHINE_MAX_INSNS);
    return 0;
  }
  for (size_t i = 0; i < rows->element_count; i++) {
    MachineInsn *insn = &out->insns[out->count];
    int declared = 0;
    if (!read_row(rows->elements[i], insn, error, error_size)) {
      if (error_at) {
        *error_at = rows->elements[i] ? rows->elements[i]->location
                                      : found->location;
      }
      return 0;
    }
    declared = module_declares_function(program, insn->semantics);
    if (declared == 0) {
      snprintf(error, error_size,
               "instruction '%s' says '%s' is what it does, and this module "
               "declares no such function",
               insn->name, insn->semantics);
      if (error_at) {
        *error_at = insn->location;
      }
      return 0;
    }
    if (declared < 0) {
      snprintf(error, error_size,
               "instruction '%s' names '%s', and a semantics function takes "
               "the %d operand slots and returns the next instruction index "
               "or -1",
               insn->name, insn->semantics, MACHINE_OPERANDS);
      if (error_at) {
        *error_at = insn->location;
      }
      return 0;
    }
    for (size_t k = 0; k < out->count; k++) {
      const MachineInsn *other = &out->insns[k];
      size_t shared = other->prefix < insn->prefix ? other->prefix
                                                   : insn->prefix;
      if (strcmp(other->name, insn->name) == 0) {
        snprintf(error, error_size, "'%s' is described twice", insn->name);
        if (error_at) {
          *error_at = insn->location;
        }
        return 0;
      }
      if (memcmp(other->bytes, insn->bytes, shared) == 0) {
        snprintf(error, error_size,
                 "'%s' and '%s' start with the same fixed bytes, so a decoder "
                 "reading them back cannot tell which one it has",
                 other->name, insn->name);
        if (error_at) {
          *error_at = insn->location;
        }
        return 0;
      }
    }
    out->count++;
  }
  return 1;
}

const MachineInsn *machine_desc_find(const MachineDesc *desc,
                                     const char *name) {
  for (size_t i = 0; i < desc->count; i++) {
    if (strcmp(desc->insns[i].name, name) == 0) {
      return &desc->insns[i];
    }
  }
  return NULL;
}

int machine_assemble(const MachineDesc *desc, const char *line,
                     unsigned char *out, size_t capacity, size_t *written,
                     char *error, size_t error_size) {
  char mnemonic[64];
  size_t at = 0;
  size_t length = 0;
  long long operands[MACHINE_OPERANDS];
  long long given = 0;
  const MachineInsn *insn = NULL;
  *written = 0;
  while (line[at] == ' ') {
    at++;
  }
  while (line[at] && line[at] != ' ' && length + 1 < sizeof(mnemonic)) {
    mnemonic[length++] = line[at++];
  }
  mnemonic[length] = '\0';
  if (length == 0) {
    return 1;
  }
  insn = machine_desc_find(desc, mnemonic);
  if (!insn) {
    snprintf(error, error_size, "'%s' is not an instruction of '%s'", mnemonic,
             desc->name);
    return 0;
  }
  for (int i = 0; i < MACHINE_OPERANDS; i++) {
    operands[i] = 0;
  }
  while (line[at]) {
    char *stop = NULL;
    long long value = 0;
    while (line[at] == ' ' || line[at] == ',') {
      at++;
    }
    if (!line[at]) {
      break;
    }
    value = strtoll(line + at, &stop, 0);
    if (stop == line + at) {
      snprintf(error, error_size,
               "'%s' takes numbers as operands, and this one is not a number",
               mnemonic);
      return 0;
    }
    if (given >= MACHINE_OPERANDS) {
      snprintf(error, error_size, "'%s' was given more than %d operands",
               mnemonic, MACHINE_OPERANDS);
      return 0;
    }
    operands[given++] = value;
    at = (size_t)(stop - line);
  }
  if (given != insn->operands) {
    snprintf(error, error_size, "'%s' takes %lld operand%s and was given %lld",
             mnemonic, insn->operands, insn->operands == 1 ? "" : "s", given);
    return 0;
  }
  if (insn->length > capacity) {
    snprintf(error, error_size, "no room left for '%s'", mnemonic);
    return 0;
  }
  for (size_t i = 0; i < insn->length; i++) {
    if (insn->slot[i] < 0) {
      out[i] = insn->bytes[i];
      continue;
    }
    {
      long long value = operands[insn->slot[i]];
      if (value < 0 || value > 255) {
        snprintf(error, error_size,
                 "'%s' encodes operand %d in one byte, and %lld does not fit",
                 mnemonic, insn->slot[i], value);
        return 0;
      }
      out[i] = (unsigned char)value;
    }
  }
  *written = insn->length;
  return 1;
}

int machine_decode(const MachineDesc *desc, const unsigned char *bytes,
                   size_t length, size_t at, const MachineInsn **out,
                   long long *operands, size_t *consumed) {
  for (size_t i = 0; i < desc->count; i++) {
    const MachineInsn *insn = &desc->insns[i];
    if (at + insn->length > length ||
        memcmp(bytes + at, insn->bytes, insn->prefix) != 0) {
      continue;
    }
    for (int k = 0; k < MACHINE_OPERANDS; k++) {
      operands[k] = 0;
    }
    for (size_t k = 0; k < insn->length; k++) {
      if (insn->slot[k] >= 0) {
        operands[insn->slot[k]] = bytes[at + k];
      }
    }
    *out = insn;
    *consumed = insn->length;
    return 1;
  }
  return 0;
}

void machine_desc_print(FILE *out, const MachineDesc *desc) {
  fprintf(out, "machine %s: %zu instruction%s\n", desc->name, desc->count,
          desc->count == 1 ? "" : "s");
  for (size_t i = 0; i < desc->count; i++) {
    const MachineInsn *insn = &desc->insns[i];
    fprintf(out, "  %-8s ", insn->name);
    for (size_t k = 0; k < insn->length; k++) {
      if (insn->slot[k] >= 0) {
        fprintf(out, "%%%d ", insn->slot[k]);
      } else {
        fprintf(out, "%02x ", insn->bytes[k]);
      }
    }
    fprintf(out, " reads %s, writes %s, does %s\n",
            insn->reads[0] ? insn->reads : "nothing",
            insn->writes[0] ? insn->writes : "nothing", insn->semantics);
  }
}
