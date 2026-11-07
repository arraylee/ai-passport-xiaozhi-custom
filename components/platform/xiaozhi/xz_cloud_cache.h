#pragma once
#include "cJSON.h"
#include <stdbool.h>

// Voice-worker owned, no retained RAM. Stores only bounded MQTT connection
// fields for this device identity, never activation or UDP session material.
cJSON *xz_cloud_cache_load(void);
bool xz_cloud_cache_version_current(const cJSON *cached);
bool xz_cloud_cache_save(const cJSON *cloud);
bool xz_cloud_cache_clear(void);
