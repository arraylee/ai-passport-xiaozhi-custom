#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define XZ_WIFI_OPTION_MAX 12
typedef struct { uint8_t ssid[32], length; int8_t rssi; bool secure; } xz_wifi_option_t;
size_t xz_wifi_option_add(xz_wifi_option_t *list, size_t count, const uint8_t *ssid,
                         size_t length, int rssi, bool secure, unsigned channel);
void xz_ssid_encode(const uint8_t *ssid, size_t length, char out[65]);
bool xz_ssid_decode(const char *hex, uint8_t out[32], size_t *length);
// Build an in-place, bounded DNS response. Zero means malformed/unsupported.
size_t xz_portal_dns_reply(uint8_t *packet, size_t size, size_t capacity,
                          const uint8_t address[4]);
