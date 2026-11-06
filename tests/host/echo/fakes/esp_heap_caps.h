#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_RTCRAM (1u << 15)
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps);
