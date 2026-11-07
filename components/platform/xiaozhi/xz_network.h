#pragma once
#include "cJSON.h"
#include "xiaozhi.h"
#include <stdatomic.h>
typedef void (*xz_status_fn)(xz_phase_t phase, const char *text);
bool xz_wait_for_setup(atomic_bool *cancel, xz_status_fn publish);
bool xz_portal_accept_input(void);
bool xz_wifi_open(bool setup, atomic_bool *cancel, xz_status_fn status,
                  char hotspot[32], char password[12]);
bool xz_wifi_online(void);
int xz_wifi_rssi(void);
void xz_wifi_idle_power(bool idle);
void xz_wifi_close(void);
cJSON *xz_cloud_config(atomic_bool *cancel, xz_status_fn status, char code[24]);
// Takes ownership of an already validated cached profile after time sync.
cJSON *xz_cloud_refresh_version(cJSON *cached, const atomic_bool *cancel);
bool xz_cloud_time_ready(atomic_bool *cancel, xz_status_fn status);
void xz_identity(char mac[18], char uuid[37]);
