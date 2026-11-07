#pragma once
#include "xz_theme.h"
typedef struct {
  int (*read)(void *, uint8_t *, size_t);
  bool (*erase)(void *, size_t, size_t);
  bool (*write)(void *, size_t, const void *, size_t);
  bool (*complete)(void *);
} xz_transfer_t;
bool xz_theme_receive(const xz_transfer_t *io, void *ctx, int64_t length, size_t capacity);
