#include "font_heap_cap.h"
#include <assert.h>
#include <stdlib.h>
size_t font_heap_limit = 512, font_heap_live, font_heap_peak, font_heap_largest_request;
static void *active;
void *passport_font_test_malloc(size_t size) {
    if (size > font_heap_largest_request) font_heap_largest_request = size;
    if (size > font_heap_limit || font_heap_live > font_heap_limit - size) return NULL;
    assert(!active);
    void *result = malloc(size);
    if (result) {
        active = result; font_heap_live += size;
        if (font_heap_live > font_heap_peak) font_heap_peak = font_heap_live;
    }
    return result;
}
void passport_font_test_free(void *pointer) {
    assert(pointer && pointer == active);
    free(pointer); active = NULL; font_heap_live = 0;
}
