#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { XZ_ECHO_FRAME = 64, XZ_ECHO_RATE = 8000, XZ_ECHO_POOL_BYTES = 12288 };
typedef struct xz_echo xz_echo_t;

/* Single-owner processing. Distinct instances may be used sequentially.
 * Allocation is bounded and occurs only in create; no audio is retained after
 * destroy. The caller owns playback-reference timing and sample conversion. */
xz_echo_t *xz_echo_create(void);
bool xz_echo_process(xz_echo_t *echo, const int16_t mic[XZ_ECHO_FRAME],
                     const int16_t reference[XZ_ECHO_FRAME],
                     int16_t clean[XZ_ECHO_FRAME]);
bool xz_echo_reset(xz_echo_t *echo);
void xz_echo_destroy(xz_echo_t *echo);
size_t xz_echo_workspace_used(const xz_echo_t *echo);

#ifdef XZ_ECHO_TEST
xz_echo_t *xz_echo_test_create(size_t capacity);
#endif
