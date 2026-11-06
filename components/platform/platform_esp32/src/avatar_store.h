#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "services/avatar_pack.h"

int avatar_store_init(void);
bool avatar_store_has(const char *name);
int avatar_store_first_name(char name[16]);
int avatar_store_name_at(uint8_t index, char name[16]);
int avatar_store_get(const char *name, const uint8_t **png, int *len);
// OK: immutable mmap frame; NOT_FOUND: legacy PNG-only entry; negative: invalid.
int avatar_store_get_native(const char *name, const uint8_t **pixels, size_t *size);
void avatar_store_deinit(void);
