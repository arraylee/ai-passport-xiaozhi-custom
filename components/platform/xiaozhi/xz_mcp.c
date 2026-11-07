#include "xz_mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMPTY "{\"type\":\"object\",\"properties\":{}}"
#define INT_ARG(key, low)                                                                          \
  "{\"type\":\"object\",\"properties\":{\"" key "\":{\"type\":\"integer\",\"minimum\":" low        \
  ",\"maximum\":100}},\"required\":[\"" key "\"]}"
#define TEXT_ARG(key)                                                                              \
  "{\"type\":\"object\",\"properties\":{\"" key "\":{\"type\":\"string\"}},\"required\":[\"" key   \
  "\"]}"
enum { STATUS, VOLUME, BRIGHTNESS, PALETTE, INFO, SCREEN, ASSETS, REBOOT };
static const struct {
  const char *name, *description, *schema;
  bool user;
} tools[] = {
    {"self.get_device_status",
     "Read current speaker volume and screen brightness before relative adjustments.", EMPTY,
     false},
    {"self.audio_speaker.set_volume",
     "Set speaker volume. For louder or quieter, read current volume first.",
     INT_ARG("volume", "0"), false},
    {"self.screen.set_brightness",
     "Set screen brightness. For brighter or dimmer, read current brightness first.",
     INT_ARG("brightness", "10"), false},
    {"self.screen.set_theme", "Choose the official theme's light or dark palette.",
     TEXT_ARG("theme"), false},
    {"self.get_system_info", "Get chip, firmware and assets partition information.", EMPTY, true},
    {"self.screen.get_info", "Read the LCD size and color mode.", EMPTY, true},
    {"self.assets.set_download_url",
     "Set the URL of an official assets.bin to install on next boot.", TEXT_ARG("url"), true},
    {"self.reboot", "Restart the device to apply pending assets.", EMPTY, true}};
#define TOOL_COUNT (sizeof tools / sizeof tools[0])
static const char *str(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool number(const cJSON *o, const char *key, int minimum, unsigned *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsNumber(v) || v->valuedouble != v->valueint || v->valueint < minimum ||
      v->valueint > 100)
    return false;
  *out = (unsigned)v->valueint;
  return true;
}
static cJSON *list(const cJSON *params) {
  const char *cursor = str(params, "cursor");
  bool user = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "withUserTools"));
  cJSON *root = cJSON_CreateObject(), *items = cJSON_AddArrayToObject(root, "tools");
  if (!items)
    goto fail;
  for (unsigned i = 0; i < TOOL_COUNT; i++) {
    if (tools[i].user && !user)
      continue;
    if (cursor && *cursor) {
      if (strcmp(cursor, tools[i].name))
        continue;
      cursor = NULL;
    }
    cJSON *tool = cJSON_CreateObject();
    if (!tool || !cJSON_AddItemToArray(items, tool)) {
      cJSON_Delete(tool);
      goto fail;
    }
    if (!cJSON_AddStringToObject(tool, "name", tools[i].name) ||
        !cJSON_AddStringToObject(tool, "description", tools[i].description) ||
        !cJSON_AddRawToObject(tool, "inputSchema", tools[i].schema) ||
        (tools[i].user && !cJSON_AddRawToObject(tool, "annotations", "{\"audience\":[\"user\"]}")))
      goto fail;
  }
  return root;
fail:
  cJSON_Delete(root);
  return NULL;
}
static cJSON *call_result(const char *text, bool error) {
  cJSON *r = cJSON_CreateObject(), *content = cJSON_AddArrayToObject(r, "content"),
        *item = cJSON_CreateObject();
  if (!content || !item || !cJSON_AddItemToArray(content, item)) {
    cJSON_Delete(item);
    goto fail;
  }
  if (!cJSON_AddStringToObject(item, "type", "text") ||
      !cJSON_AddStringToObject(item, "text", text) || !cJSON_AddBoolToObject(r, "isError", error))
    goto fail;
  return r;
fail:
  cJSON_Delete(r);
  return NULL;
}
static cJSON *invoke(unsigned i, const cJSON *arg, const xz_config_t *s, const xz_mcp_device_t *d) {
  unsigned value = 0;
  bool ok = false;
  char info[384];
  switch (i) {
  case STATUS:
    if (!s->get_volume || !s->get_brightness)
      break;
    snprintf(info, sizeof info,
             "{\"audio_speaker\":{\"volume\":%u},\"screen\":{\"brightness\":%u},\"network\":{"
             "\"type\":\"wifi\"}}",
             s->get_volume(s->user), s->get_brightness(s->user));
    return call_result(info, false);
  case VOLUME:
    ok = number(arg, "volume", 0, &value) && s->set_volume && s->set_volume(s->user, value);
    break;
  case BRIGHTNESS:
    ok = number(arg, "brightness", 10, &value) && s->set_brightness &&
         s->set_brightness(s->user, value);
    break;
  case PALETTE: {
    const char *theme = str(arg, "theme");
    ok = theme && (!strcmp(theme, "light") || !strcmp(theme, "dark")) && d->set_dark &&
         d->set_dark(!strcmp(theme, "dark"));
    break;
  }
  case INFO:
    snprintf(info, sizeof info,
             "{\"chip_model_name\":\"esp32c3\",\"flash_size\":8388608,\"board\":{\"name\":\"ai-"
             "passport\"},\"application\":{\"version\":\"%s\"},\"partition_table\":[{\"label\":"
             "\"assets\",\"type\":1,\"subtype\":130,\"address\":%u,\"size\":%u}]}",
             d->version, d->assets_address, d->assets_size);
    return call_result(info, false);
  case SCREEN:
    return call_result("{\"width\":240,\"height\":320,\"monochrome\":false}", false);
  case ASSETS: {
    const char *url = str(arg, "url");
    ok = url && d->queue_url && d->queue_url(url);
    break;
  }
  case REBOOT:
    ok = d->queue_reboot && d->queue_reboot();
    break;
  }
  return call_result(ok ? "true" : "Invalid value or device could not apply the change", !ok);
}
char *xz_mcp_reply(const cJSON *req, const char *session, const xz_config_t *s,
                   const xz_mcp_device_t *d) {
  const char *version = str(req, "jsonrpc"), *method = str(req, "method");
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id"),
              *params = cJSON_GetObjectItemCaseSensitive(req, "params");
  if (!version || strcmp(version, "2.0") || !method || (!cJSON_IsNumber(id) && !cJSON_IsString(id)))
    return NULL;
  cJSON *msg = cJSON_CreateObject(), *rpc = cJSON_AddObjectToObject(msg, "payload"), *r = NULL;
  if (!rpc || !cJSON_AddStringToObject(msg, "session_id", session) ||
      !cJSON_AddStringToObject(msg, "type", "mcp") ||
      !cJSON_AddStringToObject(rpc, "jsonrpc", "2.0"))
    goto fail;
  cJSON *copy = cJSON_Duplicate(id, false);
  if (!copy || !cJSON_AddItemToObject(rpc, "id", copy)) {
    cJSON_Delete(copy);
    goto fail;
  }
  int error = 0;
  if (!strcmp(method, "initialize")) {
    r = cJSON_CreateObject();
    cJSON *server = cJSON_AddObjectToObject(r, "serverInfo");
    if (!server || !cJSON_AddStringToObject(server, "name", "ai-passport") ||
        !cJSON_AddStringToObject(server, "version", d->version) ||
        !cJSON_AddStringToObject(r, "protocolVersion", "2024-11-05") ||
        !cJSON_AddRawToObject(r, "capabilities", "{\"tools\":{}}"))
      goto fail;
  } else if (!strcmp(method, "tools/list"))
    r = list(params);
  else if (!strcmp(method, "ping"))
    r = cJSON_CreateObject();
  else if (!strcmp(method, "tools/call")) {
    const char *name = str(params, "name");
    const cJSON *arg = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    unsigned i = 0;
    for (; i < TOOL_COUNT; i++)
      if (name && !strcmp(name, tools[i].name))
        break;
    if (i == TOOL_COUNT || (arg && !cJSON_IsObject(arg)))
      error = -32602;
    else
      r = invoke(i, arg, s, d);
  } else
    error = -32601;
  if (error) {
    cJSON *e = cJSON_AddObjectToObject(rpc, "error");
    if (!e || !cJSON_AddNumberToObject(e, "code", error) ||
        !cJSON_AddStringToObject(e, "message",
                                 error == -32601 ? "Unknown method" : "Invalid tool or arguments"))
      goto fail;
  } else if (!r || !cJSON_AddItemToObject(rpc, "result", r))
    goto fail;
  char *wire = cJSON_PrintUnformatted(msg);
  cJSON_Delete(msg);
  return wire;
fail:
  cJSON_Delete(r);
  cJSON_Delete(msg);
  return NULL;
}
