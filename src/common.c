#include "common.h"
#include "string_intern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const g_trust_modes[] = {
    "METTLE_TRUST_REFINEMENTS",
    "METTLE_TRUST_EFFECTS",
    "METTLE_TRUST_DEADLINES",
};

static int mettle_trust_mode_set(const char *name) {
  const char *value = getenv(name);
  return value && value[0] && !(value[0] == '0' && value[1] == 0);
}

int mettle_trust_mode_active(const char **name_out) {
  size_t i;
  for (i = 0; i < sizeof(g_trust_modes) / sizeof(g_trust_modes[0]); i++) {
    if (mettle_trust_mode_set(g_trust_modes[i])) {
      if (name_out) {
        *name_out = g_trust_modes[i];
      }
      return 1;
    }
  }
  return 0;
}

void mettle_trust_mode_announce(void) {
  size_t i;
  for (i = 0; i < sizeof(g_trust_modes) / sizeof(g_trust_modes[0]); i++) {
    if (!mettle_trust_mode_set(g_trust_modes[i])) {
      continue;
    }
    fprintf(stderr,
            "warning[V0001]: %s is set, so this build records as established "
            "what it did not check\n",
            g_trust_modes[i]);
  }
}

#if defined(_WIN32)
typedef long long MettleClockTicks;
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(MettleClockTicks *frequency);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(MettleClockTicks *counter);
#else
#include <time.h>
#endif

double mettle_now_ms(void) {
#if defined(_WIN32)
  static MettleClockTicks frequency = 0;
  MettleClockTicks counter = 0;

  if (frequency == 0 && !QueryPerformanceFrequency(&frequency)) {
    return 0.0;
  }
  if (frequency == 0) {
    return 0.0;
  }
  QueryPerformanceCounter(&counter);
  return (double)counter * 1000.0 / (double)frequency;
#else
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

char *mettle_strdup(const char *text) {
  if (!text) {
    return NULL;
  }
  size_t length = strlen(text) + 1;
  char *copy = malloc(length);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, length);
  return copy;
}

size_t mettle_fnv1a_hash(const char *str) {
  size_t hash = METTLE_FNV1A_OFFSET_BASIS;
  for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
    hash ^= (size_t)*p;
    hash *= METTLE_FNV1A_PRIME;
  }
  return hash;
}

void mettle_set_error(char **dest, const char *fmt, ...) {
  char buffer[512];
  va_list args;

  if (!dest) {
    return;
  }

  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  char *copy = mettle_strdup(buffer);
  if (!copy) {
    return;
  }

  free(*dest);
  *dest = copy;
}

/* Free a string unless it is interned (shared and managed by the interner). */
void mettle_free_string(char *str) {
  if (!str) {
    return;
  }
  if (!string_is_interned(str)) {
    free(str);
  }
}

void mettle_free_string_array(char **values, size_t count) {
  if (!values) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(values[i]);
  }
  free(values);
}

uint32_t mettle_f16bits_to_f32bits(uint16_t h) {
  uint32_t sign = ((uint32_t)h & (uint32_t)0x8000u) << (uint32_t)16;
  uint32_t exp = ((uint32_t)h >> (uint32_t)10) & (uint32_t)0x1Fu;
  uint32_t mant = (uint32_t)h & (uint32_t)0x3FFu;
  uint32_t out;
  if (exp == (uint32_t)0) {
    if (mant == (uint32_t)0) {
      return sign;
    }
    {
      uint32_t m = mant;
      int32_t e = (int32_t)-14;
      while ((m & (uint32_t)0x400u) == (uint32_t)0) {
        m <<= (uint32_t)1;
        e -= (int32_t)1;
      }
      m &= (uint32_t)0x3FFu;
      out = sign | ((uint32_t)(e + (int32_t)127) << (uint32_t)23) | (m << (uint32_t)13);
      return out;
    }
  }
  if (exp == (uint32_t)31) {
    if (mant == (uint32_t)0) {
      return sign | (uint32_t)0x7F800000u;
    }
    return sign | (uint32_t)0x7FC00000u | (mant << (uint32_t)13);
  }
  out = sign | ((uint32_t)(exp + (uint32_t)112) << (uint32_t)23) | (mant << (uint32_t)13);
  return out;
}

uint16_t mettle_f32bits_to_f16bits(uint32_t u) {
  uint32_t sign = (u >> (uint32_t)16) & (uint32_t)0x8000u;
  uint32_t exp = (u >> (uint32_t)23) & (uint32_t)0xFFu;
  uint32_t mant = u & (uint32_t)0x7FFFFFu;
  if (exp == (uint32_t)255) {
    if (mant == (uint32_t)0) {
      return (uint16_t)(sign | (uint32_t)0x7C00u);
    }
    return (uint16_t)(sign | (uint32_t)0x7E00u | ((mant >> (uint32_t)13) & (uint32_t)0x01FFu));
  }
  if (exp == (uint32_t)0) {
    return (uint16_t)sign;
  }
  {
    int32_t half_exp = (int32_t)exp - (int32_t)112;
    if (half_exp >= (int32_t)31) {
      return (uint16_t)(sign | (uint32_t)0x7C00u);
    }
    if (half_exp <= (int32_t)0) {
      if (half_exp < (int32_t)-10) {
        return (uint16_t)sign;
      }
      {
        uint32_t full = mant | (uint32_t)0x800000u;
        int32_t shift = (int32_t)14 - half_exp;
        uint32_t keep = full >> (uint32_t)shift;
        uint32_t mask = (shift >= (int32_t)32) ? (uint32_t)0xFFFFFFFFu : (((uint32_t)1 << (uint32_t)shift) - (uint32_t)1);
        uint32_t dropped = full & mask;
        uint32_t half = (uint32_t)1 << (uint32_t)(shift - (int32_t)1);
        if (dropped > half || (dropped == half && (keep & (uint32_t)1u) != (uint32_t)0)) {
          keep += (uint32_t)1;
        }
        if (keep >= (uint32_t)1024) {
          return (uint16_t)(sign | (uint32_t)0x0400u);
        }
        return (uint16_t)(sign | keep);
      }
    }
    {
      uint32_t dropped = mant & (uint32_t)0x1FFFu;
      uint32_t keep = mant >> (uint32_t)13;
      uint32_t half = (uint32_t)0x1000u;
      if (dropped > half || (dropped == half && (keep & (uint32_t)1u) != (uint32_t)0)) {
        keep += (uint32_t)1;
        if (keep >= (uint32_t)1024) {
          keep = (uint32_t)0;
          half_exp += (int32_t)1;
          if (half_exp >= (int32_t)31) {
            return (uint16_t)(sign | (uint32_t)0x7C00u);
          }
        }
      }
      return (uint16_t)(sign | ((uint32_t)half_exp << (uint32_t)10) | keep);
    }
  }
}

uint32_t mettle_bf16bits_to_f32bits(uint16_t h) {
  return ((uint32_t)h) << (uint32_t)16;
}

uint16_t mettle_f32bits_to_bf16bits(uint32_t u) {
  uint32_t exp = (u >> (uint32_t)23) & (uint32_t)0xFFu;
  uint32_t mant = u & (uint32_t)0x7FFFFFu;
  if (exp == (uint32_t)255) {
    if (mant == (uint32_t)0) {
      return (uint16_t)(u >> (uint32_t)16);
    }
    return (uint16_t)(((u >> (uint32_t)16) & (uint32_t)0xFF80u) | (uint32_t)0x0040u | ((u >> (uint32_t)16) & (uint32_t)0x8000u));
  }
  {
    uint32_t bias = (uint32_t)0x7FFFu + ((u >> (uint32_t)16) & (uint32_t)1u);
    return (uint16_t)((u + bias) >> (uint32_t)16);
  }
}

float mettle_f16bits_to_f32(uint16_t h) {
  uint32_t u = mettle_f16bits_to_f32bits(h);
  float f;
  memcpy(&f, &u, (size_t)4);
  return f;
}

uint16_t mettle_f32_to_f16bits(float f) {
  uint32_t u;
  memcpy(&u, &f, (size_t)4);
  return mettle_f32bits_to_f16bits(u);
}

float mettle_bf16bits_to_f32(uint16_t h) {
  uint32_t u = mettle_bf16bits_to_f32bits(h);
  float f;
  memcpy(&f, &u, (size_t)4);
  return f;
}

uint16_t mettle_f32_to_bf16bits(float f) {
  uint32_t u;
  memcpy(&u, &f, (size_t)4);
  return mettle_f32bits_to_bf16bits(u);
}

uint16_t mettle_f64bits_to_f16bits(uint64_t u) {
  double d;
  float f;
  uint32_t b;
  memcpy(&d, &u, (size_t)8);
  f = (float)d;
  memcpy(&b, &f, (size_t)4);
  return mettle_f32bits_to_f16bits(b);
}

uint16_t mettle_f64bits_to_bf16bits(uint64_t u) {
  double d;
  float f;
  uint32_t b;
  memcpy(&d, &u, (size_t)8);
  f = (float)d;
  memcpy(&b, &f, (size_t)4);
  return mettle_f32bits_to_bf16bits(b);
}

int mettle_f64_is_exact_f16(double d) {
  uint64_t u;
  uint16_t h;
  uint32_t back32;
  uint64_t backu;
  double back;
  memcpy(&u, &d, (size_t)8);
  if ((u & (uint64_t)0x7FF0000000000000ULL) == (uint64_t)0x7FF0000000000000ULL) {
    return 0;
  }
  h = mettle_f64bits_to_f16bits(u);
  back32 = mettle_f16bits_to_f32bits(h);
  {
    float bf;
    memcpy(&bf, &back32, (size_t)4);
    back = (double)bf;
  }
  memcpy(&backu, &back, (size_t)8);
  if ((u & (uint64_t)0x8000000000000000ULL) != (backu & (uint64_t)0x8000000000000000ULL)) {
    return 0;
  }
  return back == d;
}

int mettle_f64_is_exact_bf16(double d) {
  uint64_t u;
  uint16_t h;
  uint32_t back32;
  double back;
  memcpy(&u, &d, (size_t)8);
  if ((u & (uint64_t)0x7FF0000000000000ULL) == (uint64_t)0x7FF0000000000000ULL) {
    return 0;
  }
  h = mettle_f64bits_to_bf16bits(u);
  back32 = mettle_bf16bits_to_f32bits(h);
  {
    float bf;
    memcpy(&bf, &back32, (size_t)4);
    back = (double)bf;
  }
  return back == d;
}

int mettle_f32_is_exact_f16(float f) {
  uint32_t u;
  uint16_t h;
  uint32_t back;
  float bf;
  memcpy(&u, &f, (size_t)4);
  if ((u & (uint32_t)0x7F800000u) == (uint32_t)0x7F800000u) {
    return 0;
  }
  h = mettle_f32bits_to_f16bits(u);
  back = mettle_f16bits_to_f32bits(h);
  memcpy(&bf, &back, (size_t)4);
  return bf == f;
}

int mettle_f32_is_exact_bf16(float f) {
  uint32_t u;
  uint16_t h;
  uint32_t back;
  float bf;
  memcpy(&u, &f, (size_t)4);
  if ((u & (uint32_t)0x7F800000u) == (uint32_t)0x7F800000u) {
    return 0;
  }
  h = mettle_f32bits_to_bf16bits(u);
  back = mettle_bf16bits_to_f32bits(h);
  memcpy(&bf, &back, (size_t)4);
  return bf == f;
}
