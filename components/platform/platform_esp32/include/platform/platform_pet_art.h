#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Map the locked 0x500000/1MiB resource partition read-only. No heap payload
 * copy, erase, or write. The mapping is retained for the firmware lifetime.
 * Only the declared PTP1 byte extent is returned; the binder validates it.
 * Call once during boot before UI creation. No device access in host tests. */
bool platform_pet_art_map(const void **data, size_t *size);
