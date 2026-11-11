#include "xz_services.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *xz_arg_text(const cJSON *args, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}

bool xz_url_encode(char *out, size_t capacity, const char *text) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const unsigned char *s = (const unsigned char *)text; *s; ++s) {
    bool plain = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
                 strchr("-._~", *s);
    if (n + (plain ? 1 : 3) >= capacity)
      return false;
    if (plain)
      out[n++] = *s;
    else {
      out[n++] = '%';
      out[n++] = hex[*s >> 4];
      out[n++] = hex[*s & 15];
    }
  }
  if (n >= capacity)
    return false;
  out[n] = 0;
  return true;
}

typedef struct {
  char *data;
  size_t used, capacity;
  bool overflow;
} body_t;
static bool collect(const char *data, size_t length, void *arg) {
  body_t *body = arg;
  if (length >= body->capacity - body->used) {
    body->overflow = true;
    return false;
  }
  memcpy(body->data + body->used, data, length);
  body->used += length;
  body->data[body->used] = 0;
  return true;
}
static cJSON *read_json(const char *url, size_t capacity) {
  body_t body = {.data = calloc(1, capacity), .capacity = capacity};
  if (!body.data)
    return NULL;
  bool ok = xz_http_visit(url, collect, &body);
  cJSON *json = ok && !body.overflow ? cJSON_ParseWithOpts(body.data, NULL, true) : NULL;
  free(body.data);
  return json;
}
static double numeric(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}
typedef enum { CITY_UNAVAILABLE, CITY_FOUND, CITY_NEEDS_NAME } city_result_t;
static city_result_t lookup(const char *name, const char *region, xz_city_t *out, char *reply,
                            size_t cap) {
  char term[64], encoded[192], url[320];
  size_t n = strlen(name);
  if (n >= sizeof term || !n)
    return CITY_UNAVAILABLE;
  memcpy(term, name, n + 1);
  if (n > 3 && !strcmp(term + n - 3, "市"))
    term[n - 3] = 0;
  if (!xz_url_encode(encoded, sizeof encoded, term))
    return CITY_UNAVAILABLE;
  snprintf(url, sizeof url,
           "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=5&language=zh", encoded);
  cJSON *json = read_json(url, 6144);
  if (!json) {
    snprintf(reply, cap, "城市查询暂不可用，请稍后再试。");
    return CITY_UNAVAILABLE;
  }
  const cJSON *hits = cJSON_GetObjectItemCaseSensitive(json, "results"), *item;
  const cJSON *first = NULL, *second = NULL;
  double top = -1, runner = -1;
  cJSON_ArrayForEach(item, hits) {
    const char *city = xz_arg_text(item, "name"), *admin = xz_arg_text(item, "admin1");
    const char *country = xz_arg_text(item, "country"), *code = xz_arg_text(item, "country_code");
    double lat = numeric(item, "latitude"), lon = numeric(item, "longitude");
    if (!*city || strlen(city) >= sizeof out->name || strlen(admin) >= sizeof out->region ||
        !isfinite(lat) || fabs(lat) > 90 || !isfinite(lon) || fabs(lon) > 180)
      continue;
    if (*region && !((*admin && (strstr(admin, region) || strstr(region, admin))) ||
                     (*country && (strstr(country, region) || strstr(region, country))) ||
                     !strcmp(code, region)))
      continue;
    double population = numeric(item, "population");
    double score = isfinite(population) && population >= 0 ? population : 0;
    if (score > top) {
      runner = top;
      second = first;
      top = score;
      first = item;
    } else if (score > runner) {
      runner = score;
      second = item;
    }
  }
  bool ok = first != NULL;
  if (!first)
    snprintf(reply, cap, "没找到这个城市，请提供城市和省份或国家。");
  else if (second && top < 5 * (runner + 1) &&
           (strcmp(xz_arg_text(first, "admin1"), xz_arg_text(second, "admin1")) ||
            strcmp(xz_arg_text(first, "country_code"), xz_arg_text(second, "country_code")))) {
    snprintf(reply, cap, "请确认城市所在的省份或国家，例如%s（%s）或%s（%s）。",
             xz_arg_text(first, "admin1"), xz_arg_text(first, "country"),
             xz_arg_text(second, "admin1"), xz_arg_text(second, "country"));
    ok = false;
  } else {
    snprintf(out->name, sizeof out->name, "%s", xz_arg_text(first, "name"));
    snprintf(out->region, sizeof out->region, "%s", xz_arg_text(first, "admin1"));
    out->lat_e5 = (int32_t)lround(numeric(first, "latitude") * 100000);
    out->lon_e5 = (int32_t)lround(numeric(first, "longitude") * 100000);
  }
  cJSON_Delete(json);
  return ok ? CITY_FOUND : CITY_NEEDS_NAME;
}
static bool at_day(const cJSON *daily, const char *key, int day, double *out) {
  const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, key), day);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble))
    return false;
  *out = v->valuedouble;
  return true;
}
static const char *sky(int code) {
  if (code == 0)
    return "晴";
  if (code == 1)
    return "大致晴朗";
  if (code == 2)
    return "多云";
  if (code == 3)
    return "阴";
  if (code == 45 || code == 48)
    return "雾";
  if (code >= 51 && code <= 57)
    return "毛毛雨";
  if (code >= 61 && code <= 67)
    return "有雨";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    return "有雪";
  if (code >= 80 && code <= 82)
    return "阵雨";
  if (code == 95 || code == 96 || code == 99)
    return "雷暴";
  return "天气状况未知";
}
static struct {
  xz_city_t city;
  uint64_t time;
  char text[320];
} cache;

bool xz_weather_query(const cJSON *args, char *reply, size_t cap) {
  const char *name = xz_arg_text(args, "city"), *region = xz_arg_text(args, "region");
  xz_city_t home = {0}, city = {0};
  bool saved = xz_home_load(&home);
  bool remember = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "remember"));
  if (!*name && !saved) {
    snprintf(reply, cap, "请先问用户所在的城市，得到城市名后再查天气。");
    return true;
  }
  if (strlen(name) >= sizeof city.name || strlen(region) >= sizeof city.region) {
    snprintf(reply, cap, "请提供较短的城市名和省份。");
    return false;
  }
  if (!*name || (saved && !strcmp(home.name, name) && (!*region || !strcmp(home.region, region))))
    city = home;
  else {
    city_result_t result = lookup(name, region, &city, reply, cap);
    if (result != CITY_FOUND)
      return result == CITY_NEEDS_NAME;
  }
  bool same = saved && city.lat_e5 == home.lat_e5 && city.lon_e5 == home.lon_e5;
  bool store_failed = (!saved || remember) && !same && !xz_home_save(&city);
  uint64_t now = xz_online_seconds();
  bool cached = cache.time && now >= cache.time && cache.city.lat_e5 == city.lat_e5 &&
                cache.city.lon_e5 == city.lon_e5;
  const char *note = store_failed ? " 常用城市未能保存。" : "";
  if (cached && now - cache.time < 600) {
    snprintf(reply, cap, "%s%s", cache.text, note);
    return true;
  }
  char url[352];
  snprintf(
      url, sizeof url,
      "https://api.open-meteo.com/v1/"
      "forecast?latitude=%.5f&longitude=%.5f&current=temperature_2m,weather_code&daily=temperature_"
      "2m_max,temperature_2m_min,precipitation_probability_max&timezone=auto&forecast_days=2",
      city.lat_e5 / 100000., city.lon_e5 / 100000.);
  cJSON *json = read_json(url, 3072);
  const cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current"),
              *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
  double temp = numeric(current, "temperature_2m"), code = numeric(current, "weather_code"), low[2],
         high[2], rain[2];
  bool ok =
      isfinite(temp) && isfinite(code) && code >= 0 && code <= 99 && *xz_arg_text(current, "time");
  for (int d = 0; d < 2 && ok; d++)
    ok = at_day(daily, "temperature_2m_min", d, &low[d]) &&
         at_day(daily, "temperature_2m_max", d, &high[d]) &&
         at_day(daily, "precipitation_probability_max", d, &rain[d]) && rain[d] >= 0 &&
         rain[d] <= 100 && low[d] <= high[d];
  if (ok) {
    cache.city = city;
    cache.time = now;
    snprintf(cache.text, sizeof cache.text,
             "%s（%s）当地时间%.16s，%s，%.0f℃。今天 %.0f～%.0f℃，降水概率 %.0f%%；明天 "
             "%.0f～%.0f℃，降水概率 %.0f%%。来源 Open-Meteo。",
             city.name, city.region, xz_arg_text(current, "time"), sky((int)code), temp, low[0],
             high[0], rain[0], low[1], high[1], rain[1]);
    snprintf(reply, cap, "%s%s", cache.text, note);
  } else if (cached && now - cache.time <= 3600)
    snprintf(reply, cap, "更新失败，上次查到的是：%s%s", cache.text, note);
  else
    snprintf(reply, cap, "%s的天气暂时查不到，请稍后再试。%s", city.name, note);
  cJSON_Delete(json);
  return ok || (cached && now - cache.time <= 3600);
}
