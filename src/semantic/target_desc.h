#ifndef TARGET_DESC_H
#define TARGET_DESC_H

#include "../parser/ast.h"
#include "../codegen/target.h"

int target_desc_read(ASTNode *program, MtlcTargetDescription *out,
                     char *error, size_t error_size);

#endif
