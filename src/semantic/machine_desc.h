#ifndef METTLE_MACHINE_DESC_H
#define METTLE_MACHINE_DESC_H

/* A machine described as data.
 *
 * A `const` of `std/machine`'s `MachineInsn` is an instruction set: one row
 * per instruction, carrying its mnemonic, its encoding, which registers it
 * reads and writes, and the name of the Mettle function that says what it
 * does. This reads that const straight out of the AST, the same way a target
 * description is read, and checks it before anything believes it.
 *
 * What the description buys is the whole of `mettle emulate`: a program is
 * assembled into the described encoding, decoded back out of the bytes, and
 * run by calling each instruction's own semantics function in the compile
 * time interpreter. The assemble and decode halves are separate code paths
 * over the same description, so a description that cannot round-trip is a
 * build that says so rather than a machine that quietly disagrees with
 * itself.
 */

#include "parser/ast.h"

#define MACHINE_MAX_INSNS 128
#define MACHINE_MAX_BYTES 32
#define MACHINE_OPERANDS 3

typedef struct {
  char name[64];
  char semantics[64];
  char reads[64];
  char writes[64];
  long long operands;
  unsigned char bytes[MACHINE_MAX_BYTES];
  int slot[MACHINE_MAX_BYTES];
  size_t length;
  size_t prefix;
  SourceLocation location;
} MachineInsn;

typedef struct {
  char name[64];
  MachineInsn insns[MACHINE_MAX_INSNS];
  size_t count;
  SourceLocation location;
} MachineDesc;

int machine_desc_read(ASTNode *program, MachineDesc *out, char *error,
                      size_t error_size, SourceLocation *error_at);

const MachineInsn *machine_desc_find(const MachineDesc *desc,
                                     const char *name);

int machine_assemble(const MachineDesc *desc, const char *line,
                     unsigned char *out, size_t capacity, size_t *written,
                     char *error, size_t error_size);

int machine_decode(const MachineDesc *desc, const unsigned char *bytes,
                   size_t length, size_t at, const MachineInsn **out,
                   long long *operands, size_t *consumed);

void machine_desc_print(FILE *out, const MachineDesc *desc);

#endif
