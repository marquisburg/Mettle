#include "linker/unresolved_hint.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap the diagnostic so it stays a diagnostic, not a page. The generic helps
 * are ~900 bytes; a referrer line and two conditional hints still fit. */
#define UNRESOLVED_BUFFER_SIZE 4096u

static const char *unresolved_basename(const char *path) {
  const char *slash = NULL;
  const char *back = NULL;
  const char *base = NULL;
  if (!path || !path[0]) {
    return "<unknown>";
  }
  slash = strrchr(path, '/');
  back = strrchr(path, '\\');
  base = slash && back ? (slash > back ? slash + 1 : back + 1)
                       : slash          ? slash + 1
                       : back           ? back + 1
                                        : path;
  return base && base[0] ? base : path;
}

static int unresolved_is_float64_math(const char *name) {
  static const char *const math[] = {
      "sqrt", "sin",  "cos",  "tan",  "pow",  "exp",  "log",   "log2",
      "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",  NULL,
  };
  size_t i = 0;
  if (!name) {
    return 0;
  }
  for (i = 0; math[i]; i++) {
    if (strcmp(name, math[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Levenshtein distance with an early exit once the running minimum exceeds
 * max_dist. Candidates are short symbol names and the table is two rows, so
 * this is cheap even across a few hundred defined symbols on a failure path. */
static size_t unresolved_edit_distance(const char *left, const char *right,
                                       size_t max_dist) {
  size_t left_len = strlen(left);
  size_t right_len = strlen(right);
  size_t *prev = NULL;
  size_t *curr = NULL;
  size_t *swap = NULL;
  size_t i = 0;
  size_t j = 0;
  size_t result = 0;
  size_t diff = left_len > right_len ? left_len - right_len : right_len - left_len;
  if (diff > max_dist) {
    return max_dist + 1;
  }
  prev = malloc((right_len + 1) * sizeof(size_t));
  curr = malloc((right_len + 1) * sizeof(size_t));
  if (!prev || !curr) {
    free(prev);
    free(curr);
    return max_dist + 1;
  }
  for (j = 0; j <= right_len; j++) {
    prev[j] = j;
  }
  for (i = 1; i <= left_len; i++) {
    size_t row_min = curr[0] = i;
    for (j = 1; j <= right_len; j++) {
      size_t cost = left[i - 1] == right[j - 1] ? 0 : 1;
      size_t deletion = prev[j] + 1;
      size_t insertion = curr[j - 1] + 1;
      size_t substitution = prev[j - 1] + cost;
      size_t best = deletion < insertion ? deletion : insertion;
      best = best < substitution ? best : substitution;
      curr[j] = best;
      if (best < row_min) {
        row_min = best;
      }
    }
    if (row_min > max_dist) {
      result = max_dist + 1;
      goto done;
    }
    swap = prev;
    prev = curr;
    curr = swap;
  }
  result = prev[right_len];
done:
  free(prev);
  free(curr);
  return result;
}

static const char *
unresolved_suggest(const LinkResolution *resolution, const char *name) {
  const char *best = NULL;
  size_t best_dist = 0;
  size_t max_dist = 3;
  size_t name_len = 0;
  size_t i = 0;
  if (!resolution || !name || !name[0]) {
    return NULL;
  }
  name_len = strlen(name);
  /* Short names have little room for error: only near-exact matches help. */
  max_dist = name_len <= 4 ? 1 : name_len <= 8 ? 2 : 3;
  best_dist = max_dist + 1;
  for (i = 0; i < resolution->symbol_count; i++) {
    const LinkedSymbol *symbol = &resolution->symbols[i];
    size_t dist = 0;
    if (!symbol->is_defined || !symbol->name || !symbol->name[0]) {
      continue;
    }
    if (strcmp(symbol->name, name) == 0) {
      continue;
    }
    /* Import thunks and section symbols never help a source-level typo. */
    if (strncmp(symbol->name, "__imp_", 6) == 0 || symbol->name[0] == '.') {
      continue;
    }
    dist = unresolved_edit_distance(name, symbol->name, max_dist);
    if (dist < best_dist) {
      best_dist = dist;
      best = symbol->name;
      if (dist == 1 && name_len > 8) {
        break;
      }
    }
  }
  return best_dist <= max_dist ? best : NULL;
}

static void unresolved_append(char *buffer, size_t size, size_t *used,
                              const char *text) {
  size_t take = 0;
  if (*used + 1 >= size) {
    return;
  }
  take = strlen(text);
  if (*used + take >= size) {
    take = size - *used - 1;
  }
  memcpy(buffer + *used, text, take);
  *used += take;
  buffer[*used] = '\0';
}

void link_unresolved_format(const LinkResolution *resolution,
                            const char *symbol_name, char **error_message_out) {
  char buffer[UNRESOLVED_BUFFER_SIZE];
  size_t used = 0;
  const char *name = symbol_name && symbol_name[0] ? symbol_name : "<unnamed>";
  const char *referrers[2] = {NULL, NULL};
  size_t referrer_count = 0;
  const char *suggestion = NULL;
  char head[320];
  char line[512];
  size_t i = 0;

  buffer[0] = '\0';
  if (error_message_out) {
    free(*error_message_out);
    *error_message_out = NULL;
  }

  /* Prefer program objects as the referrer: runtime defaults referencing a
   * missing symbol is a compiler bug, not a user typo, and naming a temp
   * runtime path would only confuse. */
  if (resolution) {
    for (int pass = 0; pass < 2 && referrer_count < 2; pass++) {
      for (i = 0; i < resolution->object_count && referrer_count < 2; i++) {
        const LinkedInputObject *input = &resolution->objects[i];
        size_t k = 0;
        if (pass == 0 && input->is_runtime_default) {
          continue;
        }
        if (pass == 1 && !input->is_runtime_default) {
          continue;
        }
        for (k = 0; k < input->symbol_count; k++) {
          const LinkedObjectSymbol *symbol = &input->symbols[k];
          if (symbol->is_defined || !symbol->name ||
              strcmp(symbol->name, name) != 0) {
            continue;
          }
          referrers[referrer_count++] = unresolved_basename(input->path);
          break;
        }
      }
      if (referrer_count > 0) {
        break;
      }
    }
    suggestion = unresolved_suggest(resolution, name);
  }

  if (referrer_count == 1) {
    snprintf(head, sizeof(head), "Unresolved external symbol '%s' (referenced by '%s')",
             name, referrers[0]);
  } else if (referrer_count == 2) {
    snprintf(head, sizeof(head),
             "Unresolved external symbol '%s' (referenced by '%s', '%s')", name,
             referrers[0], referrers[1]);
  } else {
    snprintf(head, sizeof(head), "Unresolved external symbol '%s'", name);
  }
  unresolved_append(buffer, sizeof(buffer), &used, head);

  unresolved_append(buffer, sizeof(buffer), &used,
                    "\nhelp: `extern` declares a name; it does not provide one. "
                    "Nothing on the link provided '");
  unresolved_append(buffer, sizeof(buffer), &used, name);
  unresolved_append(buffer, sizeof(buffer), &used, "'.");

  unresolved_append(buffer, sizeof(buffer), &used,
                    "\nhelp: provide it with `--link-arg your.obj`, `--link-arg "
                    "-lname` or an import library path; check the `= \"symbol\"` "
                    "link name and spelling.");

  unresolved_append(buffer, sizeof(buffer), &used,
                    "\nhelp: on Windows the internal linker searches the owned "
                    "runtime plus kernel32, user32, gdi32, advapi32, ws2_32, "
                    "winmm (UCRT/MSVCRT are excluded); on Linux add `-l` for the "
                    "library that defines it. See docs/c-interop.md.");

  if (suggestion) {
    snprintf(line, sizeof(line), "\nhelp: did you mean '%s'?", suggestion);
    unresolved_append(buffer, sizeof(buffer), &used, line);
  }
  if (unresolved_is_float64_math(name)) {
    snprintf(line, sizeof(line),
             "\nhelp: '%s' is double-precision math; prefer import \"std/math\" "
             "(on Windows the owned runtime carries only the float32 '%sf', and "
             "UCRT/MSVCRT are excluded).",
             name, name);
    unresolved_append(buffer, sizeof(buffer), &used, line);
  } else if (strcmp(name, "time") == 0) {
    unresolved_append(buffer, sizeof(buffer), &used,
                      "\nhelp: 'time' is a C-library name, not a Win32 one, so the "
                      "DLL probe never sees it; use bench_time_us() from "
                      "std/bench, or clock/gettimeofday raw.");
  }

  if (error_message_out) {
    *error_message_out = mettle_strdup(buffer);
  }
}
