/* Exercise the shipped voice tools with deterministic network and NVS boundaries. */
#include "xz_mcp.h"
#include "xz_services.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *geo =
    "{\"results\":[{\"name\":\"深圳\",\"admin1\":\"广东\",\"country\":\"中国\",\"population\":"
    "1000000,\"latitude\":22.5,\"longitude\":114},{\"name\":\"深圳\",\"admin1\":\"福建\","
    "\"population\":100,\"latitude\":25,\"longitude\":118}]}";
static const char *forecast =
    "{\"current\":{\"temperature_2m\":22.1,\"weather_code\":2,\"time\":\"2026-11-10T12:00\"},"
    "\"daily\":{\"temperature_2m_min\":[16,17],\"temperature_2m_max\":[25,24],\"precipitation_"
    "probability_max\":[18,51]}}";
static const char *radio =
    "[{\"name\":\"杭州西湖之声\",\"url_resolved\":\"https://radio.example.org/"
    "x.mp3\",\"codec\":\"MP3\",\"hls\":0,\"lastcheckok\":1},"
    "{\"name\":\"重复台\",\"url_resolved\":\"https://radio.example.org/"
    "x.mp3\",\"codec\":\"MP3\",\"hls\":0,\"lastcheckok\":1},"
    "{\"name\":\"杭州交通广播\",\"url_resolved\":\"https://radio.example.org/"
    "jt.mp3\",\"codec\":\"MP3\",\"hls\":0,\"lastcheckok\":1},"
    "{\"name\":\"other\",\"url_resolved\":\"http://127.0.0.1/"
    "x\",\"codec\":\"MP3\",\"hls\":0,\"lastcheckok\":1},"
    "{\"name\":\"unsupported\",\"url_resolved\":\"https://radio.example.org/"
    "a.m3u8\",\"codec\":\"MP3\",\"hls\":1,\"lastcheckok\":1}]";
static unsigned geos, forecasts, dirs, saves;
static uint64_t now = 100;
static bool offline, broken, save_ok = true;
static xz_city_t home;
bool xz_home_load(xz_city_t *out) {
  *out = home;
  return home.name[0] != 0;
}
bool xz_home_save(const xz_city_t *v) {
  if (!save_ok)
    return false;
  home = *v;
  saves++;
  return true;
}
uint64_t xz_online_seconds(void) { return now; }
bool xz_http_visit(const char *url, xz_body_fn receive, void *ctx) {
  const char *body;
  if (strstr(url, "/v1/search?")) {
    geos++;
    body = geo;
  } else if (strstr(url, "/v1/forecast?")) {
    forecasts++;
    body = forecast;
  } else {
    assert(strstr(url, ".api.radio-browser.info"));
    dirs++;
    body = radio;
  }
  if (offline || !body)
    return false;
  if (broken)
    body = "{broken[";
  for (size_t i = 0, n = strlen(body); i < n; i += 11)
    if (!receive(body + i, n - i < 11 ? n - i : 11, ctx))
      break;
  return true;
}
static bool adapter(const char *tool, const cJSON *args, char *out, size_t size) {
  return !strcmp(tool, "self.weather.get_forecast")
             ? xz_weather_query(args, out, size)
             : xz_radio_query(args, !strcmp(tool, "self.radio.play_station"), out, size);
}
static char answer[750];
static const char *ask(const char *tool, const char *args, bool error) {
  char json[512];
  snprintf(json, sizeof json,
           "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/"
           "call\",\"params\":{\"name\":\"self.%s\",\"arguments\":%s}}",
           tool, args);
  cJSON *request = cJSON_Parse(json);
  xz_mcp_device_t dev = {.version = "2.1.0.20", .online = adapter};
  char *wire = xz_mcp_reply(request, "local-test", &(xz_config_t){0}, &dev);
  assert(wire);
  cJSON *obj = cJSON_Parse(wire);
  const cJSON *rpc = cJSON_GetObjectItem(obj, "payload"), *r = cJSON_GetObjectItem(rpc, "result");
  assert(cJSON_GetObjectItem(rpc, "id")->valueint == 21 && r &&
         cJSON_IsTrue(cJSON_GetObjectItem(r, "isError")) == error);
  snprintf(answer, sizeof answer, "%s",
           xz_arg_text(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "content"), 0), "text"));
  cJSON_Delete(request);
  cJSON_Delete(obj);
  cJSON_free(wire);
  return answer;
}
#define W(args, err) ask("weather.get_forecast", args, err)
#define L(args, err) ask("radio.list_stations", args, err)
#define P(args, err) ask("radio.play_station", args, err)
static char *fixture(const char *folder, const char *name) {
  char file[1024];
  snprintf(file, sizeof file, "%s/%s.json", folder, name);
  FILE *f = fopen(file, "rb");
  assert(f);
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  char *out = calloc((size_t)n + 1, 1);
  assert(out && fread(out, 1, n, f) == (size_t)n);
  fclose(f);
  return out;
}
int main(int argc, char **argv) {
  assert(strstr(W("{}", false), "所在的城市") && !geos && !forecasts);
  assert(strstr(W("{\"city\":\"深圳市\"}", false), "多云") && strstr(answer, "51%") && saves == 1 &&
         geos == 1);
  assert(strstr(W("{}", false), "深圳") && forecasts == 1);
  now += 601;
  offline = true;
  assert(strstr(W("{}", false), "上次") && strstr(answer, "22℃"));
  now += 3601;
  assert(strstr(W("{}", true), "暂时") && !strstr(answer, "22℃"));
  offline = false;
  geo = "{\"results\":[{\"name\":\"青山\",\"admin1\":\"甲省\",\"country\":\"中国\",\"population\":"
        "300,\"latitude\":30,\"longitude\":110},{\"name\":\"青山\",\"admin1\":\"乙省\",\"country\":"
        "\"中国\",\"population\":280,\"latitude\":31,\"longitude\":111}]}";
  assert(strstr(W("{\"city\":\"青山\"}", false), "确认城市") && saves == 1);
  assert(strstr(W("{\"city\":\"青山\",\"region\":\"乙省\"}", false), "乙省") &&
         !strcmp(home.name, "深圳"));
  assert(strstr(W("{\"city\":\"青山\",\"region\":\"甲省\",\"remember\":true}", false), "甲省") &&
         !strcmp(home.name, "青山") && saves == 2);
  now += 3601;
  broken = true;
  assert(strstr(W("{}", true), "暂时"));
  broken = false;
  assert(strstr(L("{}", false), "北京新闻广播") && strstr(answer, "下一页") && !xz_radio_queued());
  assert(strstr(L("{\"next_page\":true}", false), "深圳飞扬971"));
  assert(strstr(P("{\"number\":2}", false), "飞扬") && xz_radio_queued());
  assert(strstr(xz_radio_select(0)->url, "/1271/"));
  xz_radio_unqueue();
  assert(strstr(P("{\"station\":\"北京电台\"}", false), "北京交通广播") && !xz_radio_queued());
  assert(strstr(L("{\"city\":\"深圳市的电台\"}", false), "深圳飞扬971") && !xz_radio_queued() &&
         !dirs);
  assert(strstr(P("{\"station\":\"深圳电台\"}", false), "深圳先锋") && !xz_radio_queued());
  assert(strstr(P("{\"station\":\"深圳音乐广播\"}", false), "飞扬") && xz_radio_queued());
  assert(strstr(xz_radio_select(1)->name, "交通"));
  assert(strstr(xz_radio_select(-1)->url, "/1271/"));
  xz_radio_unqueue();
  assert(strstr(P("{\"number\":4}", false), "本页"));
  assert(strstr(L("{\"city\":\"杭州市\"}", false), "西湖") && strstr(answer, "杭州交通") &&
         !strstr(answer, "重复") && dirs == 1);
  assert(strstr(P("{\"number\":2}", false), "杭州交通") &&
         strstr(xz_radio_select(0)->url, "/jt.mp3"));
  xz_radio_unqueue();
  assert(strstr(L("{\"next_page\":true}", false), "最后一页") && !xz_radio_queued());
  radio = "[]";
  assert(strstr(L("{\"city\":\"无此城市\"}", false), "没有找到") && !xz_radio_queued());
  offline = true;
  assert(strstr(L("{\"city\":\"杭州\"}", true), "暂时") && !xz_radio_queued());
  offline = false;
  assert(!xz_url_public("http://192.168.1.1/") &&
         !xz_url_public("https://cdn.org@127.0.0.1/file") && !xz_url_public("file:///etc/hosts"));
  assert(!xz_url_public("https://abc.local/file") &&
         xz_url_public("https://cdn.example.org/live/stream.mp3"));
  if (argc == 2) {
    char *g = fixture(argv[1], "geo"), *f = fixture(argv[1], "forecast"),
         *d = fixture(argv[1], "directory");
    geo = g;
    forecast = f;
    radio = d;
    now += 7200;
    assert(strstr(W("{\"city\":\"深圳\"}", false), "Open-Meteo"));
    printf("Live provider response: %s\n", answer);
    assert(strstr(L("{\"city\":\"杭州\"}", false), "杭州"));
    printf("Live directory: %s\n", answer);
    assert(strstr(P("{\"number\":1}", false), "即将播放") && xz_radio_queued());
    printf("Directory selection: %s %s\n", xz_radio_select(0)->name, xz_radio_select(0)->url);
    free(g);
    free(f);
    free(d);
  }
  xz_radio_forget();
  assert(!xz_radio_queued() && !xz_radio_select(0));
  puts("PASS: MCP, city selection/persistence, forecasts, cache/failures, "
       "city/station/ordinal/paging and directory filtering");
}
