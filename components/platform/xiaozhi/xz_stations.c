#include "xz_services.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
  const char *city, *name, *aliases, *id;
} preset_t;
static const preset_t presets[] = {
    {"北京", "北京新闻广播", "新闻|北京新闻|北京新闻广播|FM100.6", "339"},
    {"北京", "北京音乐广播", "音乐|北京音乐|北京音乐台|北京音乐广播|FM97.4", "332"},
    {"北京", "北京交通广播", "交通|北京交通|北京交通广播|FM103.9", "4873"},
    {"深圳", "深圳先锋898", "新闻|先锋|深圳新闻广播|先锋898|FM89.8", "1270"},
    {"深圳", "深圳飞扬971", "音乐|深圳音乐|深圳音乐广播|音乐广播|飞扬971|FM97.1|97.1", "1271"},
    {"深圳", "深圳交通广播", "交通|深圳交通|深圳交通频率|FM106.2", "1272"},
    {"全国", "中国之声", "央广中国之声|中央人民广播电台中国之声", "15318317"},
    {"全国", "怀旧音乐广播", "怀旧音乐|怀旧音乐广播", "4804"},
    {"上海", "第一财经广播", "第一财经|上海第一财经", "276"},
};
enum { PRESETS = sizeof presets / sizeof presets[0], MAX_EXTRA = 9, PAGE = 3 };
static uint8_t matches[PRESETS + MAX_EXTRA];
static unsigned count, page_at, active;
static bool queued;
static xz_station_t *extra;
static unsigned extra_count;
static char filter[64];
static xz_station_t selected;
static const char *label(unsigned i) {
  return i < PRESETS ? presets[i].name : extra[i - PRESETS].name;
}
static bool alias(const char *list, const char *s) {
  size_t size = strlen(s);
  for (const char *p = list; *p;) {
    const char *end = strchr(p, '|');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n == size && !strncasecmp(p, s, n))
      return true;
    if (!end)
      break;
    p = end + 1;
  }
  return false;
}
static bool city_only(const char *s, const char *city) {
  size_t n = strlen(city);
  return n && !strncmp(s, city, n) && alias("|市|电台|市电台|的电台|广播电台|人民广播电台", s + n);
}
static bool is_city(const char *s, const char *city) {
  return !strcmp(s, city) || city_only(s, city);
}

/* Accept only public HTTP(S) DNS names and conventional ports. The transport
 * checks all resolved addresses and every redirect again before opening them. */
bool xz_url_public(const char *url) {
  if (!url || strlen(url) >= sizeof selected.url)
    return false;
  const char *host = !strncmp(url, "https://", 8)  ? url + 8
                     : !strncmp(url, "http://", 7) ? url + 7
                                                   : NULL;
  if (!host)
    return false;
  for (const unsigned char *p = (const unsigned char *)host; *p; ++p)
    if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '#')
      return false;
  size_t n = strcspn(host, "/:?");
  if (!n || n > 200 || !isalpha((unsigned char)host[n - 1]) || host[0] == '-' || host[0] == '.')
    return false;
  const char *dot = memchr(host, '.', n);
  if (!dot || n - (size_t)(dot - host) < 3)
    return false;
  for (size_t i = 0; i < n; i++)
    if (!(isalnum((unsigned char)host[i]) || host[i] == '-' || host[i] == '.') ||
        (host[i] == '.' && (!i || host[i - 1] == '.' || host[i - 1] == '-')))
      return false;
  if (n >= 6 && !strncasecmp(host + n - 6, ".local", 6))
    return false;
  if (n >= 10 && !strncasecmp(host + n - 10, ".localhost", 10))
    return false;
  if (host[n] == ':') {
    const char *port = host + n + 1;
    size_t len = strcspn(port, "/?");
    if (!((len == 2 && !strncmp(port, "80", 2)) || (len == 3 && !strncmp(port, "443", 3)) ||
          (len == 4 && (!strncmp(port, "8000", 4) || !strncmp(port, "8080", 4)))))
      return false;
  }
  return true;
}
static bool direct(const char *url) {
  size_t n = strcspn(url, "?");
  for (size_t i = 0; i + 4 <= n; i++)
    if (!strncasecmp(url + i, ".m3u", 4) || !strncasecmp(url + i, ".pls", 4) ||
        !strncasecmp(url + i, ".aac", 4))
      return false;
  return xz_url_public(url);
}
static bool gather(const char *object) {
  cJSON *row = cJSON_Parse(object);
  const char *name = xz_arg_text(row, "name"), *url = xz_arg_text(row, "url_resolved");
  const cJSON *health = cJSON_GetObjectItemCaseSensitive(row, "lastcheckok"),
              *hls = cJSON_GetObjectItemCaseSensitive(row, "hls");
  bool valid = *name && strlen(name) < sizeof selected.name &&
               !strcasecmp(xz_arg_text(row, "codec"), "MP3") && cJSON_IsNumber(health) &&
               health->valueint == 1 && cJSON_IsNumber(hls) && !hls->valueint && direct(url);
  for (size_t p = 0; valid && name[p]; p++)
    if ((unsigned char)name[p] < 32)
      valid = false;
  for (unsigned i = 0; valid && i < extra_count; i++)
    if (!strcmp(extra[i].name, name) || !strcmp(extra[i].url, url))
      valid = false;
  if (valid && extra_count < MAX_EXTRA) {
    xz_station_t *s = &extra[extra_count++];
    strcpy(s->name, name);
    strcpy(s->url, url);
  }
  cJSON_Delete(row);
  return extra_count < MAX_EXTRA;
}
/* A directory row includes verbose metadata. Parse one bounded JSON object
 * at a time; braces inside escaped strings never affect framing. */
typedef struct {
  char row[2304];
  unsigned depth, used;
  bool string, slash, skipped, array, done, bad;
} rows_t;
static bool rows(const char *data, size_t n, void *ctx) {
  rows_t *s = ctx;
  for (size_t i = 0; i < n; i++) {
    char c = data[i];
    if (!s->depth) {
      if (isspace((unsigned char)c))
        continue;
      if (!s->array && c == '[') {
        s->array = true;
        continue;
      }
      if (s->array && c == ',')
        continue;
      if (s->array && c == ']') {
        s->done = true;
        return false;
      }
      if (!s->array || c != '{') {
        s->bad = true;
        return false;
      }
      s->used = 0;
      s->skipped = false;
    }
    if (s->used < sizeof s->row - 1)
      s->row[s->used++] = c;
    else
      s->skipped = true;
    if (s->string) {
      if (s->slash)
        s->slash = false;
      else if (c == '\\')
        s->slash = true;
      else if (c == '"')
        s->string = false;
    } else if (c == '"')
      s->string = true;
    else if (c == '{')
      s->depth++;
    else if (c == '}' && !--s->depth && !s->skipped) {
      s->row[s->used] = 0;
      if (!gather(s->row)) {
        s->done = true;
        return false;
      }
    }
  }
  return true;
}
static bool directory(const char *query) {
  char encoded[192], url[384];
  if (!*query || !xz_url_encode(encoded, sizeof encoded, query))
    return false;
  if (!extra)
    extra = calloc(MAX_EXTRA, sizeof *extra);
  if (!extra)
    return false;
  const char *hosts[] = {"de1", "fi1"};
  for (unsigned i = 0; i < 2; i++) {
    snprintf(url, sizeof url,
             "https://%s.api.radio-browser.info/json/stations/"
             "search?countrycode=CN&name=%s&codec=MP3&hidebroken=true&order=clickcount&reverse="
             "true&limit=15",
             hosts[i], encoded);
    rows_t parse = {0};
    extra_count = 0;
    if (xz_http_visit(url, rows, &parse) && parse.done && !parse.bad)
      return true;
  }
  extra_count = 0;
  return false;
}
static void reset_list(void) {
  count = page_at = active = 0;
  extra_count = 0;
  queued = false;
}
static bool make_list(const char *city) {
  reset_list();
  snprintf(filter, sizeof filter, "%s", city);
  for (unsigned i = 0; i < PRESETS; i++)
    if (!*city || !strcmp(city, presets[i].city))
      matches[count++] = i;
  if (!count && *city) {
    if (!directory(city))
      return false;
    for (unsigned i = 0; i < extra_count; i++)
      matches[count++] = PRESETS + i;
  }
  return true;
}
static bool describe(char *reply, size_t cap) {
  if (!count) {
    snprintf(reply, cap, "当前目录里没有找到可播的电台，请换个城市或台名。");
    return true;
  }
  size_t used = 0;
  for (unsigned i = page_at; i < count && i < page_at + PAGE; i++) {
    int n = snprintf(reply + used, cap - used, "%s%u. %s", used ? "，" : "", i - page_at + 1,
                     label(matches[i]));
    if (n < 0 || (size_t)n >= cap - used)
      break;
    used += (size_t)n;
  }
  snprintf(reply + used, cap - used, "。%s请用户选台，可说台名或本页序号。",
           page_at + PAGE < count ? "还有下一页。" : "");
  return true;
}
bool xz_radio_queued(void) { return queued; }
void xz_radio_unqueue(void) { queued = false; }
void xz_radio_forget(void) {
  reset_list();
  filter[0] = 0;
  free(extra);
  extra = NULL;
}
const xz_station_t *xz_radio_select(int step) {
  if (!count)
    return NULL;
  active = (unsigned)(((int)active + (step < 0 ? -1 : step > 0 ? 1 : 0) + (int)count) % (int)count);
  unsigned i = matches[active];
  if (i >= PRESETS)
    selected = extra[i - PRESETS];
  else {
    snprintf(selected.name, sizeof selected.name, "%s", presets[i].name);
    // Provider's public HTTP endpoint, also used by Passport OS. No user
    // credentials are sent. Directory-supplied HTTPS URLs remain unchanged.
    snprintf(selected.url, sizeof selected.url, "http://lhttp.qingting.fm/live/%s/64k.mp3",
             presets[i].id);
  }
  return &selected;
}
static bool select_at(unsigned i, char *reply, size_t cap) {
  active = i;
  queued = true;
  snprintf(reply, cap, "即将播放%s。", label(matches[i]));
  return true;
}
static bool named(const char *name, unsigned index) {
  return !strcasecmp(name, label(index)) ||
         (index < PRESETS && alias(presets[index].aliases, name));
}
/* Some agents pass the whole utterance as the city, e.g. 深圳市的电台. */
static size_t city_prefix(const char *name) {
  static const char *suffixes[] = {"市的电台", "的电台", "市电台", "人民广播电台",
                                   "广播电台", "电台",   "市"};
  size_t n = strlen(name);
  for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
    size_t length = strlen(suffixes[i]);
    if (n > length && !strcmp(name + n - length, suffixes[i]))
      return n - length;
  }
  return 0;
}
bool xz_radio_query(const cJSON *args, bool play, char *reply, size_t cap) {
  const char *city = xz_arg_text(args, "city"), *name = xz_arg_text(args, "station");
  if (strlen(city) >= sizeof filter || strlen(name) >= sizeof selected.name) {
    snprintf(reply, cap, "城市或台名过长。");
    return false;
  }
  char area[sizeof filter] = {0};
  size_t prefix = city_prefix(city);
  if (*city) {
    size_t length = prefix ? prefix : strlen(city);
    memcpy(area, city, length);
  } else if (*name) {
    prefix = city_prefix(name);
    bool known_station = false;
    for (unsigned i = 0; i < PRESETS; i++) {
      if (named(name, i)) {
        known_station = true;
        break;
      }
      if (!prefix && is_city(name, presets[i].city))
        strcpy(area, presets[i].city);
    }
    if (!known_station && prefix && prefix < sizeof area) {
      memcpy(area, name, prefix);
      name = "";
    }
  }
  const cJSON *ord = cJSON_GetObjectItemCaseSensitive(args, "number");
  bool next = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "next_page"));
  if (play && cJSON_IsNumber(ord)) {
    int choice = ord->valueint;
    if ((!*area || !strcmp(area, filter)) && choice == ord->valuedouble && choice >= 1 &&
        choice <= PAGE && page_at + (unsigned)choice <= count)
      return select_at(page_at + (unsigned)choice - 1, reply, cap);
    snprintf(reply, cap, "请先列出这个城市的电台，再选本页的序号。");
    return true;
  }
  if (!play || !*name || (*area && is_city(name, area))) {
    if (next && count && (!*area || !strcmp(area, filter))) {
      if (page_at + PAGE >= count) {
        snprintf(reply, cap, "已经是最后一页。");
        return true;
      }
      page_at += PAGE;
    } else if (!make_list(area))
      goto unavailable;
    return describe(reply, cap);
  }
  if (*area && (strcmp(area, filter) || !count)) {
    if (!make_list(area))
      goto unavailable;
  } else if (!count && !make_list(""))
    goto unavailable;
  unsigned found = 0, position = 0;
  for (unsigned i = 0; i < count; i++)
    if (named(name, matches[i])) {
      found++;
      position = i;
    }
  if (found == 1)
    return select_at(position, reply, cap);
  if (!found && !*area) {
    /* Fully qualified station names work even after browsing another city. */
    for (unsigned i = 0; i < PRESETS; i++)
      if (named(name, i)) {
        found++;
        position = i;
      }
    if (found == 1) {
      unsigned preset = position;
      make_list(presets[preset].city);
      for (unsigned i = 0; i < count; i++)
        if (matches[i] == preset)
          return select_at(i, reply, cap);
    }
  }
  if (found > 1) {
    snprintf(reply, cap, "有多个匹配的电台，请问想听哪个城市的%s？", name);
    return true;
  }
  if (!*area) {
    if (!make_list(name))
      goto unavailable;
    for (unsigned i = 0; i < count; i++)
      if (named(name, matches[i]))
        return select_at(i, reply, cap);
  }
  /* A fuzzy match is a suggestion, never permission to play a different station. */
  if (count) {
    unsigned kept = 0;
    for (unsigned i = 0; i < count; i++)
      if (strstr(label(matches[i]), name))
        matches[kept++] = matches[i];
    if (kept) {
      count = kept;
      page_at = 0;
      return describe(reply, cap);
    }
  }
  snprintf(reply, cap, "当前目录没有匹配“%s”的台，可询问某个城市有哪些电台。", name);
  return true;
unavailable:
  snprintf(reply, cap, "在线电台目录暂时无法访问，可点播北京或深圳的内置电台。");
  return false;
}
