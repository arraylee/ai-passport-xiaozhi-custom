#include "xz_echo.h"
#include "speex/speex_echo.h"
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct xz_echo {
    unsigned char *pool;
    size_t capacity, used;
    SpeexEchoState *state;
    jmp_buf failure;
    bool failed;
};

/* Speex's allocator interface has no user argument. Guard its short calls so
 * a second caller cannot redirect allocations into another owner's workspace.
 * The voice worker remains the sole owner of each individual instance. */
static atomic_flag busy = ATOMIC_FLAG_INIT;
static xz_echo_t *owner;

static bool enter(xz_echo_t *echo) {
    if (atomic_flag_test_and_set_explicit(&busy, memory_order_acquire)) return false;
    owner = echo;
    return true;
}
static void leave(void) {
    owner = NULL;
    atomic_flag_clear_explicit(&busy, memory_order_release);
}
static _Noreturn void fail(void) {
    /* Only the vendored implementation calls this, inside a guarded operation
     * with an established failure frame; it is never an ISR callback. */
    owner->failed = true;
    longjmp(owner->failure, 1);
}

void *xz_speex_alloc(int bytes) {
    if (bytes <= 0 || (size_t)bytes > SIZE_MAX - 7u) fail();
    size_t size = ((size_t)bytes + 7u) & ~(size_t)7u;
    if (owner->used > owner->capacity || size > owner->capacity - owner->used) fail();
    void *result = owner->pool + owner->used;
    owner->used += size;
    memset(result, 0, size);
    return result;
}
void xz_speex_free(void *ptr) { (void)ptr; } /* Whole workspace is reclaimed. */
void *xz_speex_realloc(void *ptr, int bytes) {
    (void)ptr; (void)bytes;
    /* The pinned fixed-size MDF path does not realloc. Fail closed if a future
     * library change introduces an unsupported allocation pattern. */
    fail();
}
_Noreturn void xz_speex_fatal(const char *text, const char *file, int line) {
    (void)text; (void)file; (void)line;
    fail();
}

static void dispose(xz_echo_t *echo) {
    if (!echo) return;
    if (echo->pool) {
        /* Audio/filter histories are private, even on partial construction. */
        volatile unsigned char *p = echo->pool;
        for (size_t i = 0; i < echo->capacity; ++i) p[i] = 0;
        free(echo->pool);
    }
    memset(echo, 0, sizeof *echo);
    free(echo);
}
static void initialize(xz_echo_t *echo) {
    if (setjmp(echo->failure) == 0) {
        echo->state = speex_echo_state_init(XZ_ECHO_FRAME, 256);
        if (!echo->state) fail();
        int rate = XZ_ECHO_RATE;
        if (speex_echo_ctl(echo->state, SPEEX_ECHO_SET_SAMPLING_RATE, &rate)) fail();
    }
}
static xz_echo_t *create(size_t capacity) {
    if (!capacity || capacity > XZ_ECHO_POOL_BYTES) return NULL;
    xz_echo_t *echo = calloc(1, sizeof *echo);
    if (!echo) return NULL;
    echo->pool = malloc(capacity);
    echo->capacity = capacity;
    if (!echo->pool || !enter(echo)) { dispose(echo); return NULL; }
    initialize(echo);
    leave();
    if (echo->failed) { dispose(echo); return NULL; }
    return echo;
}
xz_echo_t *xz_echo_create(void) { return create(XZ_ECHO_POOL_BYTES); }

bool xz_echo_process(xz_echo_t *echo, const int16_t mic[XZ_ECHO_FRAME],
                     const int16_t reference[XZ_ECHO_FRAME], int16_t clean[XZ_ECHO_FRAME]) {
    if (!echo || !mic || !reference || !clean || echo->failed || !enter(echo)) return false;
    if (setjmp(echo->failure) == 0)
        speex_echo_cancellation(echo->state, mic, reference, clean);
    leave();
    if (echo->failed) memset(clean, 0, XZ_ECHO_FRAME * sizeof *clean);
    return !echo->failed;
}
bool xz_echo_reset(xz_echo_t *echo) {
    if (!echo || echo->failed || !enter(echo)) return false;
    if (setjmp(echo->failure) == 0) speex_echo_state_reset(echo->state);
    leave();
    return !echo->failed;
}
void xz_echo_destroy(xz_echo_t *echo) {
    /* All Speex allocations belong to the one pool: no partially initialized
     * upstream destroy traversal is needed after a trapped allocation failure. */
    dispose(echo);
}
size_t xz_echo_workspace_used(const xz_echo_t *echo) { return echo ? echo->used : 0; }
#ifdef XZ_ECHO_TEST
xz_echo_t *xz_echo_test_create(size_t capacity) { return create(capacity); }
#endif
