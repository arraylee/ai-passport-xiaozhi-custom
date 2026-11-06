#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  size_t capacity, used, peak, failures, last_failure;
  // Requested system bytes for bounded small wrappers; excludes heap metadata.
  size_t small_used, small_live, small_peak;
} xz_codec_memory_stats_t;

// Reserve24KiB for codec state before network/TLS fragments heap. Small (<=64B)
// owner codec allocations use at most8 tracked system allocations, reclaimed at end.
// Internal16-byte alignment leaves24560..24576 usable bytes; end frees the
// original complete allocation, never the aligned interior pointer.
bool xz_codec_memory_begin(void);
// Call after codec handles and network work are closed, before restoring BLE.
// A false result reports leaked allocations; the owning session is reclaimed.
bool xz_codec_memory_end(void);
void xz_codec_memory_snapshot(xz_codec_memory_stats_t *out);

// Strong definitions of the pinned esp_audio_codec 2.5 weak allocation ABI.
void *media_lib_module_malloc(const char *module, size_t size);
void *media_lib_module_calloc(const char *module, size_t count, size_t size);
void *media_lib_module_realloc(const char *module, void *pointer, size_t size);
void media_lib_free(void *pointer);
