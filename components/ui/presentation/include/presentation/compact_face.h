#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { XZ_FACE_WIDTH = 144, XZ_FACE_HEIGHT = 144, XZ_FACE_ROWS = 4,
       XZ_FACE_STRIP_BYTES = XZ_FACE_WIDTH * XZ_FACE_ROWS * 3 };
bool compact_face_decode(const uint8_t *data, size_t bytes, unsigned strip,
                         uint8_t out[XZ_FACE_STRIP_BYTES]);
bool compact_face_register(void);
