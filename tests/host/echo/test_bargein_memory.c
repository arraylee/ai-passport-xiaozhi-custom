/* CPU-only history must leave scarce DMA-capable SRAM for the audio driver. */
#include "xz_bargein.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

typedef union { max_align_t align; struct {size_t size; bool rtc;} value; } header_t;
static size_t general_used, rtc_used;
static bool fail_rtc;
static void *allocate(size_t bytes, bool rtc) {
    size_t *used = rtc ? &rtc_used : &general_used;
    size_t capacity = rtc ? 8192 : 14336;
    if ((rtc && fail_rtc) || bytes > capacity - *used) return NULL;
    header_t *h = malloc(sizeof *h + bytes);
    assert(h); h->value.size = bytes; h->value.rtc = rtc; *used += bytes;
    memset(h + 1, 0, bytes); return h + 1;
}
void *qa_malloc(size_t bytes) { return allocate(bytes, false); }
void *qa_calloc(size_t n, size_t bytes) { return allocate(n * bytes, false); }
void qa_free(void *p) {
    if (!p) return;
    header_t *h = (header_t *)p - 1;
    size_t *used = h->value.rtc ? &rtc_used : &general_used;
    assert(*used >= h->value.size); *used -= h->value.size; free(h);
}
void *heap_caps_calloc(size_t n, size_t bytes, uint32_t caps) {
    assert(caps == (MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT));
    return allocate(n * bytes, true);
}
int main(void) {
    for (unsigned turn = 0; turn < 20; ++turn) {
        xz_bargein_t *b = xz_bargein_create(0);
        assert(b && general_used <= 14336 && rtc_used > 4800 && rtc_used < 8192);
        int16_t silence[64] = {0};
        assert(xz_bargein_resume(b));
        assert(xz_bargein_feed(b, silence, silence));
        xz_bargein_destroy(b); assert(!general_used && !rtc_used);
    }
    fail_rtc = true;
    assert(!xz_bargein_create(0));
    assert(!general_used && !rtc_used);
    return 0;
}
