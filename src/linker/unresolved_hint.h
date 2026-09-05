#ifndef LINK_UNRESOLVED_HINT_H
#define LINK_UNRESOLVED_HINT_H

#include "linker/symbol_resolve.h"

/* Format a helpful unresolved-external diagnostic into *error_message_out.
 * The first line stays `Unresolved external symbol '<name>'` (existing
 * consumers match on that substring); `help:` lines follow with what provides
 * a name, platform specifics, a did-you-mean suggestion drawn from the defined
 * symbols in `resolution` (which may be NULL), and pointers for known traps
 * such as double-precision math and `time`. */
void link_unresolved_format(const LinkResolution *resolution,
                            const char *symbol_name,
                            char **error_message_out);

#endif
