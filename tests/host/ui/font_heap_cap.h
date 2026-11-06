#pragma once
#include <stddef.h>
extern size_t font_heap_limit, font_heap_live, font_heap_peak, font_heap_largest_request;
void *passport_font_test_malloc(size_t size);
void passport_font_test_free(void *pointer);
