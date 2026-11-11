#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All calls and the selected station belong to the voice worker. */
typedef struct {
  char name[64], region[64];
  int32_t lat_e5, lon_e5;
} xz_city_t;
typedef struct {
  char name[80], url[256];
} xz_station_t;
typedef bool (*xz_body_fn)(const char *data, size_t length, void *context);
/* A visitor returning false finishes an otherwise successful transfer early. */
bool xz_http_visit(const char *url, xz_body_fn receive, void *context);
bool xz_url_public(const char *url);
bool xz_home_load(xz_city_t *city);
bool xz_home_save(const xz_city_t *city);
uint64_t xz_online_seconds(void);
bool xz_online_cancelled(void);

bool xz_weather_query(const cJSON *args, char *reply, size_t capacity);
bool xz_radio_query(const cJSON *args, bool play, char *reply, size_t capacity);
bool xz_radio_queued(void);
const xz_station_t *xz_radio_select(int step);
void xz_radio_unqueue(void);
void xz_radio_forget(void);
const char *xz_arg_text(const cJSON *args, const char *key);
bool xz_url_encode(char *out, size_t capacity, const char *text);
