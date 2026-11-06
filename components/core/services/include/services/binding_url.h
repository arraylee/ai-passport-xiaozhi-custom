#pragma once
#include <stddef.h>

// Full 64-byte DeviceSecret, percent-encoded byte-for-byte; never truncate.
#define BINDING_URL_CAP 272
// Returns URL length, or -1 for a missing/oversized identity or output buffer.
int binding_url_build(char *out, size_t cap, const char *sn, const char *key);
