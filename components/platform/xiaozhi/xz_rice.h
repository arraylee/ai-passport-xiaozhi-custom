#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* 120 samples per independent bitstream; predictors continue between blocks. */
typedef struct {
  const uint8_t *p, *end;
  uint32_t remaining;
  int previous, older;
} xz_rice_t;
static inline uint32_t rice_u32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline int rice_bit(const uint8_t *p, size_t limit, size_t *bit) {
  if (*bit / 8 >= limit)
    return -1;
  size_t pos = (*bit)++;
  return (p[pos / 8] >> (7 - pos % 8)) & 1;
}
static inline bool xz_rice_block(xz_rice_t *s, int16_t *dst, unsigned n) {
  if (s->p >= s->end || !n || n > 120 || n > s->remaining)
    return false;
  unsigned tag = *s->p++, order = tag >> 4, k = tag & 15;
  size_t available = s->end - s->p, bit = 0;
  if (tag == 255 && available < 2 * n)
    return false;
  if (tag != 255 && order > 2)
    return false;
  for (unsigned i = 0; i < n; i++) {
    int sample;
    if (tag == 255)
      sample = (int16_t)(s->p[2 * i] | (unsigned)s->p[2 * i + 1] << 8);
    else {
      unsigned q = 0;
      int digit;
      do {
        digit = rice_bit(s->p, available, &bit);
        if (digit < 0 || q > 262144u >> k)
          return false;
        if (!digit)
          q++;
      } while (!digit);
      unsigned value = q;
      for (unsigned j = 0; j < k; j++) {
        digit = rice_bit(s->p, available, &bit);
        if (digit < 0)
          return false;
        value = value * 2 + digit;
      }
      int delta = (value & 1) ? -1 - (int)(value / 2) : (int)(value / 2);
      sample = delta + (order == 2 ? 2 * s->previous - s->older : order == 1 ? s->previous : 0);
      if (sample < -32768 || sample > 32767)
        return false;
    }
    dst[i] = sample;
    s->older = s->previous;
    s->previous = sample;
  }
  s->remaining -= n;
  s->p += tag == 255 ? 2 * n : (bit + 7) / 8;
  return true;
}
