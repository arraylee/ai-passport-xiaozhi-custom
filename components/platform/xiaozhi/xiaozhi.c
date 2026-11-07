#include "xiaozhi.h"
#include "xz_codec_memory.h"
#include "xz_capture.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "xz_network.h"
#include "xz_theme.h"
#include "xz_mcp.h"
#include "esp_system.h"
#include "xz_cloud_cache.h"
#include "xz_quiet.h"
#include "xz_prompt.h"
#include "xz_protocol.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static xz_config_t config;
static xz_view_t view, working;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool running, cancel, requested_talk;
static atomic_bool requested_interrupt, natural_interrupt;
static atomic_bool requested_end, requested_resume;
static atomic_bool welcome_visible, setup_confirmed;
typedef enum { SESSION_CLOSED, SESSION_PARK, SESSION_RENEW, SESSION_QUIET_BYE, SESSION_INTENT_BYE, SESSION_AUDIO_LOST, SESSION_THEME_FAILED } session_end_t;
static atomic_bool worker_finished;
// Owned by the existing plays worker, which also creates/deletes voice_task.
static TaskHandle_t voice_task;
static bool radio_owned;
static atomic_uint avatar = 0;
static bool setup_requested;
static const char *TAG = "xiaozhi";
static atomic_uint allocation_fail_count, allocation_fail_caps;
static atomic_size_t allocation_fail_size;
static unsigned allocation_fail_reported;
static bool allocation_hook_registered;
// On single-core C3, IDF implements these numeric atomics with a brief
// interrupt mask; they do not allocate or wait on a task/heap mutex.
static void allocation_failed(size_t size, uint32_t caps, const char *function_name) {
  (void)function_name;
  if (!atomic_load_explicit(&running, memory_order_relaxed)) return;
  // Heap callbacks may run under pressure on any task: no logging, allocation,
  // function names, or payload access. Last size/caps are numeric samples.
  atomic_store_explicit(&allocation_fail_size, size, memory_order_relaxed);
  atomic_store_explicit(&allocation_fail_caps, caps, memory_order_relaxed);
  atomic_fetch_add_explicit(&allocation_fail_count, 1, memory_order_release);
}
static void allocation_failure_metric(void) {
  unsigned count = atomic_load_explicit(&allocation_fail_count, memory_order_acquire);
  if (count == allocation_fail_reported) return;
  allocation_fail_reported = count;
  ESP_LOGI(TAG, "alloc_failed count=%u size=%u caps=%u", count,
           (unsigned)atomic_load_explicit(&allocation_fail_size, memory_order_relaxed),
           atomic_load_explicit(&allocation_fail_caps, memory_order_relaxed));
}
static void allocation_failure_init(void) {
  if (allocation_hook_registered) return;
  int result = heap_caps_register_failed_alloc_callback(allocation_failed);
  allocation_hook_registered = result == ESP_OK;
  if (!allocation_hook_registered)
    ESP_LOGW(TAG, "allocation diagnostic registration failed rc=%d", result);
}
static void codec_arena_metric(unsigned step);
static void reconnect_status(xz_phase_t phase, const char *detail);
static uint64_t ms(void) { return esp_timer_get_time() / 1000; }
static void status(xz_phase_t phase, const char *detail) {
  allocation_failure_metric();
  // Only phase and allocation metrics: never log speech, credentials or codes.
  ESP_LOGI(TAG, "state=%u heap=%u largest=%u min=%u", (unsigned)phase,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)esp_get_minimum_free_heap_size());
  working.phase = phase;
  if (phase == XZ_WIFI || phase == XZ_SETUP) working.stage = XZ_STAGE_WIFI;
  else if (phase == XZ_BIND) working.stage = XZ_STAGE_BIND;
  else if (phase == XZ_READY || phase == XZ_LISTEN || phase == XZ_THINK || phase == XZ_SPEAK)
    working.stage = XZ_STAGE_VOICE;
  if (detail != working.detail) xz_text_copy(working.detail, sizeof working.detail, detail);
  working.avatar = atomic_load(&avatar);
  working.revision++;
  taskENTER_CRITICAL(&guard);
  view = working;
  taskEXIT_CRITICAL(&guard);
}
// Report only the source line of a failed transition. Exact-build symbols map
// it to a fixed error branch without retaining cloud text or speech.
static void status_origin(xz_phase_t phase, const char *detail, unsigned line) {
  if (phase == XZ_ERROR) ESP_LOGW(TAG, "error_origin=%u", line);
  status(phase, detail);
}
#define status(phase, detail) status_origin((phase), (detail), __LINE__)

void xiaozhi_snapshot(xz_view_t *out) {
  taskENTER_CRITICAL(&guard);
  *out = view;
  taskEXIT_CRITICAL(&guard);
  out->avatar = atomic_load(&avatar);
  out->natural_interrupt = atomic_load(&natural_interrupt);
  out->volume = config.get_volume ? config.get_volume(config.user) : 95;
  out->output_active = xiaozhi_output_active();
}
bool xiaozhi_output_active(void) { return hal_audio_output_active(config.audio); }
void xiaozhi_welcome_shown(void) { atomic_store(&welcome_visible, true); }
void xiaozhi_begin_setup(void) { atomic_store(&setup_confirmed, true); }
bool xz_wait_for_setup(atomic_bool *stop, xz_status_fn publish) {
  extern const uint8_t start[] asm("_binary_welcome_zh_rice_start");
  extern const uint8_t end[] asm("_binary_welcome_zh_rice_end");
  publish(XZ_WELCOME, "");
  while (!atomic_load(&welcome_visible) && !atomic_load(&setup_confirmed) && !atomic_load(stop))
    vTaskDelay(pdMS_TO_TICKS(20));
  if (!atomic_load(stop) && !atomic_load(&setup_confirmed))
    (void)xz_play_pcm_prompt(&config, stop, &setup_confirmed, start, (size_t)(end - start));
  while (!atomic_load(&setup_confirmed) && !atomic_load(stop)) vTaskDelay(pdMS_TO_TICKS(30));
  return !atomic_load(stop);
}
void xiaozhi_init(const xz_config_t *c) {
  allocation_failure_init();
  config = *c;
  nvs_handle_t h;
  uint8_t value = 0, natural = 0;
  if (nvs_open("xiaozhi", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "avatar", &value);
    nvs_get_u8(h, "natural", &natural);
    nvs_close(h);
  }
  atomic_store(&avatar, value < XZ_AVATARS ? value : 0);
  atomic_store(&natural_interrupt, natural == 1);
}
bool xiaozhi_set_natural_interrupt(bool enabled) {
  if (enabled == atomic_load(&natural_interrupt)) return true;
  nvs_handle_t h;
  if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) return false;
  bool saved = nvs_set_u8(h, "natural", enabled ? 1 : 0) == ESP_OK &&
               nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (saved) atomic_store(&natural_interrupt, enabled);
  return saved;
}
bool xiaozhi_set_volume(unsigned volume) {
  return volume <= 100 && config.set_volume && config.set_volume(config.user, volume);
}
void xiaozhi_avatar(unsigned index) {
  if (index < XZ_AVATARS)
    atomic_store(&avatar, index);
}
void xiaozhi_talk(void) {
  if (atomic_load(&running)) atomic_store(&requested_end, true);
}
void xiaozhi_resume(void) {
  if (atomic_load(&running)) atomic_store(&requested_resume, true);
}
void xiaozhi_interrupt(void) {
  if (atomic_load(&running)) atomic_store(&requested_interrupt, true);
}
void xiaozhi_stop(void) { atomic_store(&cancel, true); }
bool xiaozhi_stopped(void) { return !atomic_load(&running); }
bool xz_portal_accept_input(void) {
  return !atomic_load(&cancel) && (!config.portal_activity || config.portal_activity());
}
// MQTT transfers ownership of each complete frame to the voice worker. Storage
// follows actual frame length, not three permanently reserved 4 KiB copies.
typedef struct channel_message {
  struct channel_message *next;
  size_t used, total;
  char data[];
} channel_message_t;
static portMUX_TYPE message_guard = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
  esp_mqtt_client_handle_t mqtt;
  channel_message_t *partial, *first, *last;
  size_t queued_bytes;
  atomic_bool connected, failed, control_ready, closing;
  atomic_int error, suback_id;
  char topic[256], session[128];
  int udp, rate;
  uint8_t nonce[16];
  uint32_t tx_sequence, rx_sequence;
  uint32_t tx_retries, tx_dropped;
  mbedtls_aes_context aes;
} channel_t;
enum { CHANNEL_OK, CHANNEL_CONFIG, CHANNEL_MEMORY, CHANNEL_TLS,
       CHANNEL_AUTH, CHANNEL_CONNECTION, CHANNEL_CONNECT_TIMEOUT,
       CHANNEL_SUBSCRIBE, CHANNEL_HELLO_SEND, CHANNEL_HELLO_TIMEOUT,
       CHANNEL_HELLO_INVALID, CHANNEL_SUBSCRIBE_TIMEOUT, CHANNEL_CONTROL_OVERFLOW, CHANNEL_CONTROL_PARSE };
static channel_message_t *channel_take(channel_t *c, unsigned wait_ms) {
  for (unsigned attempt = 0; ; ++attempt) {
    taskENTER_CRITICAL(&message_guard);
    channel_message_t *message = c->first;
    if (message) {
      c->first = message->next;
      if (!c->first) c->last = NULL;
      c->queued_bytes -= message->total + sizeof(uint32_t);
      message->next = NULL;
    }
    taskEXIT_CRITICAL(&message_guard);
    if (message || attempt || !wait_ms || atomic_load(&cancel) || atomic_load(&c->failed))
      return message;
    // Same bounded 100 ms hello wait as before; the live audio loop never waits.
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
  }
}
static void channel_discard_partial(channel_t *c) {
  free(c->partial);
  c->partial = NULL;
}
static void channel_push(channel_t *c, const char *data, int length, int offset, int total) {
  if (offset == 0) channel_discard_partial(c);
  if (!data || length <= 0 || offset < 0 || total <= 0 || total > XZ_JSON_MAX ||
      offset > total || length > total - offset ||
      (offset && (!c->partial || (size_t)offset != c->partial->used ||
                  (size_t)total != c->partial->total))) {
    channel_discard_partial(c);
    return;
  }
  if (!offset) {
    c->partial = malloc(sizeof *c->partial + (size_t)total + 1);
    if (!c->partial) {
      atomic_store(&c->error, CHANNEL_MEMORY);
      atomic_store(&c->failed, true);
      return;
    }
    // Cancellation/error may arrive while the allocator was running. Never
    // publish a late frame to a channel that is already shutting down.
    if (atomic_load(&cancel) || atomic_load(&c->failed) || !atomic_load(&c->control_ready)) {
      channel_discard_partial(c);
      return;
    }
    c->partial->next = NULL;
    c->partial->used = 0;
    c->partial->total = (size_t)total;
  }
  memcpy(c->partial->data + offset, data, (size_t)length);
  c->partial->used += (size_t)length;
  c->partial->data[c->partial->used] = 0;
  if (c->partial->used != c->partial->total) return;
  channel_message_t *message = c->partial;
  c->partial = NULL;
  const size_t charged = message->total + sizeof(uint32_t);
  taskENTER_CRITICAL(&message_guard);
  // Preserve the previous message-buffer byte quota, including each length
  // prefix. The producer never blocks while holding the MQTT client mutex.
  bool full = charged > XZ_JSON_MAX + 16u - c->queued_bytes;
  if (!full) {
    if (c->last) c->last->next = message;
    else c->first = message;
    c->last = message;
    c->queued_bytes += charged;
  }
  taskEXIT_CRITICAL(&message_guard);
  if (full) {
    free(message);
    atomic_store(&c->error, CHANNEL_CONTROL_OVERFLOW);
    atomic_store(&c->failed, true);
  }
}
static void channel_clear(channel_t *c) {
  // Caller has stopped/destroyed MQTT first; no callback can still own partial.
  channel_discard_partial(c);
  channel_message_t *message;
  while ((message = channel_take(c, 0)) != NULL) free(message);
}
static int mqtt_error_kind(const esp_mqtt_error_codes_t *e) {
  if (!e) return CHANNEL_CONNECTION;
  int64_t code=e->esp_tls_stack_err;
  if(code<0)code=-code;
  if(e->esp_tls_last_esp_err==ESP_ERR_NO_MEM || code==0x7f00 ||
     code==0x2880 || code==0x3f80 || (code && (code&0x7f)==0x10))
    return CHANNEL_MEMORY;
  if(e->error_type==MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
     (e->connect_return_code==MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
      e->connect_return_code==MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED))return CHANNEL_AUTH;
  return code || e->esp_tls_cert_verify_flags ? CHANNEL_TLS : CHANNEL_CONNECTION;
}
static const char *channel_error_text(const channel_t *c) {
  int error = atomic_load(&c->error);
  ESP_LOGW(TAG, "channel_error=%d", error);
  switch(error) {
  case CHANNEL_MEMORY:return "语音连接内存不足\n请返回玩法后重试";
  case CHANNEL_CONFIG:return "设备已绑定\n语音配置暂不可用，请重试";
  case CHANNEL_TLS:return "语音安全连接未完成\n请重试";
  case CHANNEL_AUTH:return "语音服务验证未通过\n请在控制台检查设备";
  case CHANNEL_CONNECT_TIMEOUT:return "设备已绑定\n语音连接超时，请重试";
  case CHANNEL_HELLO_TIMEOUT:return "设备已绑定\n语音服务回应超时，请重试";
  case CHANNEL_HELLO_INVALID:return "语音服务参数未就绪\n请重试";
  case CHANNEL_SUBSCRIBE:return "语音消息订阅失败\n请重试";
  case CHANNEL_SUBSCRIBE_TIMEOUT:return "语音消息订阅超时\n请重试";
  case CHANNEL_HELLO_SEND:return "语音握手发送失败\n请重试";
  case CHANNEL_CONTROL_OVERFLOW:return "语音控制消息过多\n请返回玩法后重试";
  case CHANNEL_CONTROL_PARSE:return "语音控制消息解析失败\n请重试";
  default:return "设备已绑定\n语音连接中断，请重试";
  }
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t event,
                       void *data) {
  (void)base;
  channel_t *c = arg;
  esp_mqtt_event_handle_t e = data;
  if (event == MQTT_EVENT_CONNECTED) {
    // A terminal disconnect/error wins over a late CONNECTED event; this client
    // has auto-reconnect disabled and will be stopped/destroyed by the caller.
    if (!atomic_load(&cancel) && !atomic_load(&c->failed) && !atomic_load(&c->closing)) {
      // The broker can deliver MCP initialize immediately after CONNACK, before
      // the voice worker wakes up or sends hello. Queue it on this callback.
      atomic_store(&c->control_ready, true);
      atomic_store(&c->connected, true);
    }
  } else if (event == MQTT_EVENT_SUBSCRIBED && e) {
    if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
      atomic_store(&c->error, CHANNEL_SUBSCRIBE);
      atomic_store(&c->failed, true);
    } else {
      // Save the ACK even if it arrives before subscribe() returns its msg_id.
      atomic_store(&c->suback_id, e->msg_id);
    }
  } else if (event == MQTT_EVENT_DISCONNECTED || event == MQTT_EVENT_ERROR) {
    if(event==MQTT_EVENT_ERROR) {
      atomic_store(&c->error,mqtt_error_kind(e->error_handle));
      const esp_mqtt_error_codes_t *error=e->error_handle;
      ESP_LOGW(TAG,"mqtt failed type=%d esp=%d tls=%d flags=%d auth=%d heap=%u largest=%u",
          error?error->error_type:0,error?error->esp_tls_last_esp_err:0,
          error?error->esp_tls_stack_err:0,error?error->esp_tls_cert_verify_flags:0,
          error?error->connect_return_code:0,(unsigned)esp_get_free_heap_size(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    } else if(atomic_load(&c->error)==CHANNEL_OK)
      atomic_store(&c->error,CHANNEL_CONNECTION);
    atomic_store(&c->connected, false);
    atomic_store(&c->failed, true);
  } else if (event == MQTT_EVENT_DATA && !atomic_load(&cancel) &&
             !atomic_load(&c->failed) && !atomic_load(&c->closing) && atomic_load(&c->control_ready)) {
    channel_push(c, e->data, e->data_len, e->current_data_offset, e->total_data_len);
  }
}
static bool string_field(const cJSON *obj, const char *name, char *out,
                         size_t cap) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
  if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) >= cap)
    return false;
  strcpy(out, v->valuestring);
  return true;
}
static bool send_text(channel_t *c, const char *text) {
  return c->mqtt && !atomic_load(&c->failed) && atomic_load(&c->connected) &&
         esp_mqtt_client_publish(c->mqtt, c->topic, text, 0, 0, 0) >= 0;
}
static void handle_mcp(channel_t *c, const cJSON *j) {
  const cJSON *sid = cJSON_GetObjectItemCaseSensitive(j, "session_id");
  if (sid && !cJSON_IsString(sid)) return;
  const char *session=cJSON_IsString(sid)?sid->valuestring:c->session;
  if (strlen(session)>=sizeof c->session || (c->session[0] && session[0] && strcmp(c->session,session))) return;
  xz_mcp_device_t device={xz_theme_queue_url,xz_theme_queue_reboot,xz_theme_set_dark,
      XZ_CONSOLE_VERSION,xz_theme_address(),XZ_THEME_LIMIT};
  char *reply=xz_mcp_reply(cJSON_GetObjectItemCaseSensitive(j,"payload"),session,&config,&device);
  if (reply) { (void)send_text(c,reply); cJSON_free(reply); }
}
static bool command(channel_t *c, const char *type, const char *state) {
  cJSON *j = cJSON_CreateObject();
  if (!j)
    return false;
  if (!cJSON_AddStringToObject(j, "session_id", c->session) ||
      !cJSON_AddStringToObject(j, "type", type) ||
      (state && !cJSON_AddStringToObject(j, "state", state)) ||
      (state && !strcmp(state, "start") &&
       !cJSON_AddStringToObject(j, "mode", "auto"))) {
    cJSON_Delete(j);
    return false;
  }
  char *s = cJSON_PrintUnformatted(j);
  bool ok = s && send_text(c, s);
  free(s);
  cJSON_Delete(j);
  return ok;
}
static bool hello(channel_t *c, const cJSON *j) {
  char host[128], key[33], nonce[33], transport[16];
  cJSON *u = cJSON_GetObjectItem(j, "udp"),
        *a = cJSON_GetObjectItem(j, "audio_params"),
        *port = cJSON_GetObjectItem(u, "port");
  cJSON *rate = cJSON_GetObjectItem(a, "sample_rate"),
        *duration = cJSON_GetObjectItem(a, "frame_duration");
  if (!string_field(j, "transport", transport, sizeof transport) ||
      strcmp(transport, "udp") ||
      !string_field(j, "session_id", c->session, sizeof c->session) ||
      !string_field(u, "server", host, sizeof host) ||
      !string_field(u, "key", key, sizeof key) ||
      !string_field(u, "nonce", nonce, sizeof nonce) || !cJSON_IsNumber(port) ||
      port->valueint < 1 || port->valueint > 65535 || !cJSON_IsNumber(rate) ||
      (rate->valueint != 16000 && rate->valueint != 24000) ||
      !cJSON_IsNumber(duration) || duration->valueint != 60)
    return false;
  uint8_t rawkey[16];
  if (!xz_hex16(key, rawkey) || !xz_hex16(nonce, c->nonce))
    return false;
  int error = mbedtls_aes_setkey_enc(&c->aes, rawkey, 128);
  memset(rawkey, 0, sizeof rawkey);
  memset(key, 0, sizeof key);
  if (error)
    return false;
  char service[8];
  snprintf(service, sizeof service, "%d", port->valueint);
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM},
                  *result = NULL;
  if (getaddrinfo(host, service, &hints, &result) != 0)
    return false;
  c->udp = socket(result->ai_family, result->ai_socktype, 0);
  bool ok =
      c->udp >= 0 && connect(c->udp, result->ai_addr, result->ai_addrlen) == 0;
  freeaddrinfo(result);
  if (!ok)
    return false;
  struct timeval timeout = {.tv_usec = 20000};
  // Bound queued voice by bytes as well as mailbox entries. With copied L3
  // packets this retains compact Opus payloads instead of whole Wi-Fi buffers,
  // leaving room for TLS/control traffic while the decoder consumes a frame.
  // Two maximum legal packets fit; typical voice packets allow a longer burst.
  const int receive_bytes = 2 * (XZ_PACKET_MAX + 16);
  if (setsockopt(c->udp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
      setsockopt(c->udp, SOL_SOCKET, SO_RCVBUF, &receive_bytes, sizeof receive_bytes) != 0)
    return false; // The channel owner closes this socket on every failure.
  c->rate = rate->valueint;
  return true;
}
static bool open_channel(channel_t *c, cJSON **cloud) {
  const cJSON *m = cJSON_GetObjectItem(*cloud, "mqtt");
  char endpoint[192], client[256], username[512], password[768], uri[208], subscribe[256]={0};
  atomic_store(&c->error,CHANNEL_CONFIG);
  if (!string_field(m, "endpoint", endpoint, sizeof endpoint) ||
      !string_field(m, "client_id", client, sizeof client) ||
      !string_field(m, "username", username, sizeof username) ||
      !string_field(m, "password", password, sizeof password) ||
      !string_field(m, "publish_topic", c->topic, sizeof c->topic))
    return false;
  // Endpoint is a host[:port], never an unverified downgrade to plain TCP.
  if (strchr(endpoint, '/') || strchr(endpoint, '\r') || strchr(endpoint, '\n'))
    return false;
  const cJSON *sub=cJSON_GetObjectItem(m,"subscribe_topic");
  if(cJSON_IsString(sub) && sub->valuestring[0] &&
     !string_field(m,"subscribe_topic",subscribe,sizeof subscribe))return false;
  snprintf(uri, sizeof uri, "mqtts://%s", endpoint);
  esp_mqtt_client_config_t mc = {
      .broker.address.uri = uri,
      .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
      .credentials.client_id = client,
      .credentials.username = username,
      .credentials.authentication.password = password,
      .session.keepalive = 60,
      .network.timeout_ms = 6000,
      .network.reconnect_timeout_ms = 500,
      .network.disable_auto_reconnect = true,
      .task.stack_size = 5120,
      .buffer.size = 1024,
      .buffer.out_size = 1024};
  // All MQTT fields now live in bounded local copies. Free the parsed OTA
  // graph before allocating any long-lived MQTT state, so small client objects
  // cannot pin a former TLS/JSON hole in the middle of our largest heap block.
  cJSON_Delete(*cloud); *cloud = NULL;
  c->mqtt = esp_mqtt_client_init(&mc);
  memset(password, 0, sizeof password);
  if (!c->mqtt) {
    atomic_store(&c->error,CHANNEL_MEMORY);
    return false;
  }
  // ESP-MQTT copies its configuration. Keep control storage deferred until
  // its TLS handshake is complete.
  atomic_store(&c->error,CHANNEL_OK);
  ESP_LOGI(TAG,"mqtt pre-TLS heap=%u largest=%u",(unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  esp_err_t error=esp_mqtt_client_register_event(c->mqtt, ESP_EVENT_ANY_ID, mqtt_event, c);
  if(error==ESP_OK)error=esp_mqtt_client_start(c->mqtt);
  if (error != ESP_OK) {
    atomic_store(&c->error,error==ESP_ERR_NO_MEM?CHANNEL_MEMORY:CHANNEL_CONNECTION);
    return false;
  }
  for (int i = 0; i < 150 && !atomic_load(&cancel) &&
                  !atomic_load(&c->failed) && !atomic_load(&c->connected);
       i++)
    vTaskDelay(pdMS_TO_TICKS(100));
  if (atomic_load(&cancel) || atomic_load(&c->failed) || !atomic_load(&c->connected)) {
    if(!atomic_load(&cancel) && !atomic_load(&c->failed))
      atomic_store(&c->error,CHANNEL_CONNECT_TIMEOUT);
    return false;
  }
  ESP_LOGI(TAG,"mqtt connected heap=%u largest=%u",(unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  codec_arena_metric(4);
  if (atomic_load(&cancel) || atomic_load(&c->failed)) return false;
  if (subscribe[0]) {
    atomic_store(&c->suback_id, -1);
    int subscription = esp_mqtt_client_subscribe(c->mqtt, subscribe, 0);
    if (subscription < 0) {
      atomic_store(&c->error, CHANNEL_SUBSCRIBE); return false;
    }
    // The API only queues SUBSCRIBE. Do not send hello until the broker has
    // acknowledged this subscription, including a synchronous early callback.
    for (unsigned i = 0; i < 50 && !atomic_load(&cancel) && !atomic_load(&c->failed) &&
                              atomic_load(&c->suback_id) != subscription; ++i)
      vTaskDelay(pdMS_TO_TICKS(100));
    if (atomic_load(&cancel) || atomic_load(&c->failed)) return false;
    if (atomic_load(&c->suback_id) != subscription) {
      atomic_store(&c->error, CHANNEL_SUBSCRIBE_TIMEOUT); return false;
    }
  }
  if (atomic_load(&cancel) || atomic_load(&c->failed)) return false;
  if (!send_text(
          c, "{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\","
             "\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"opus\",\"sample_"
             "rate\":16000,\"channels\":1,\"frame_duration\":60}}")) {
    atomic_store(&c->error,CHANNEL_HELLO_SEND);
    return false;
  }
  bool ok = false;
  for (int i = 0; i < 100 && !atomic_load(&cancel) && !atomic_load(&c->failed);
       i++) {
    channel_message_t *message = channel_take(c, 100);
    if (!message) continue;
    cJSON *j = cJSON_Parse(message->data);
    free(message);
    if (!j) {
      atomic_store(&c->error, CHANNEL_CONTROL_PARSE);
      atomic_store(&c->failed, true);
      break;
    }
    cJSON *t = cJSON_GetObjectItem(j, "type");
    if (cJSON_IsString(t) && !strcmp(t->valuestring,"mcp")) handle_mcp(c,j);
    if (cJSON_IsString(t) && !strcmp(t->valuestring, "hello")) {
      ok = hello(c, j);
      if(!ok)atomic_store(&c->error,CHANNEL_HELLO_INVALID);
    }
    cJSON_Delete(j);
    if (ok)
      break;
  }
  if (atomic_load(&cancel) || atomic_load(&c->failed)) return false;
  if(!ok && atomic_load(&c->error)==CHANNEL_OK)
    atomic_store(&c->error,CHANNEL_HELLO_TIMEOUT);
  return ok;
}
static void channel_release(channel_t *c, bool notify_peer) {
  if (!c) return;
  atomic_store(&c->closing,true);
  atomic_store(&c->control_ready,false);
  if (c->mqtt) {
    if (notify_peer && atomic_load(&c->connected)) command(c,"goodbye",NULL);
    esp_mqtt_client_stop(c->mqtt);
    esp_mqtt_client_destroy(c->mqtt);
  }
  if (c->udp>=0) close(c->udp);
  channel_clear(c);
  mbedtls_aes_free(&c->aes);
  memset(c,0,sizeof *c);
  free(c);
}
static channel_t *open_session_channel(cJSON **cloud, bool cached, bool warm) {
  // At most one refresh. Release the old TLS client, socket and messages before
  // HTTPS allocates anything; only the voice worker owns this transition.
  for (unsigned attempt=0; attempt<2 && !atomic_load(&cancel); ++attempt) {
    working.stage=XZ_STAGE_VOICE;
    if (!cached) {
      bool saved=xz_cloud_cache_save(*cloud);
      ESP_LOGI(TAG,"cloud_cache_saved=%d",saved);
    }
    channel_t *c=calloc(1,sizeof *c);
    if (!c) { status(XZ_ERROR,"语音连接内存不足\n请返回玩法后重试"); return NULL; }
    c->udp=-1; mbedtls_aes_init(&c->aes);
    status(warm || cached ? XZ_WAKING : XZ_CLOUD,"正在连接小智");
    bool connected=open_channel(c,cloud);
    cJSON_Delete(*cloud); *cloud=NULL;
    if (connected) return c;
    int error=atomic_load(&c->error);
    bool refresh=cached && attempt==0 && error!=CHANNEL_MEMORY && !atomic_load(&cancel);
    if (!refresh && !atomic_load(&cancel)) status(XZ_ERROR,channel_error_text(c));
    channel_release(c,false);
    if (!refresh || atomic_load(&cancel)) return NULL;
    if (error==CHANNEL_AUTH || error==CHANNEL_CONFIG) (void)xz_cloud_cache_clear();
    ESP_LOGI(TAG,"cloud_cache_refresh=%d",error);
    cached=false;
    working.stage=XZ_STAGE_CLOUD;
    *cloud=xz_cloud_config(&cancel,warm ? reconnect_status : status,working.code);
    codec_arena_metric(3);
    if (!*cloud || atomic_load(&cancel)) return NULL;
  }
  return NULL;
}
static bool audio_crypt(channel_t *c, const uint8_t *in, size_t n,
                        const uint8_t nonce[16], uint8_t *out) {
  uint8_t counter[16], stream[16] = {0};
  memcpy(counter, nonce, 16);
  size_t offset = 0;
  return mbedtls_aes_crypt_ctr(&c->aes, n, &offset, counter, stream, in, out) ==
         0;
}
static bool send_audio(channel_t *c, const uint8_t *opus, size_t n,
                       uint8_t *packet) {
  if (!n || n > XZ_PACKET_MAX) {
    ESP_LOGW(TAG, "audio_send_invalid_size=%u", (unsigned)n);
    return false;
  }
  memcpy(packet, c->nonce, 16);
  packet[2] = n >> 8;
  packet[3] = n;
  uint32_t sequence = htonl(++c->tx_sequence), stamp = htonl((uint32_t)ms());
  memcpy(packet + 8, &stamp, 4);
  memcpy(packet + 12, &sequence, 4);
  if (!audio_crypt(c, opus, n, packet, packet + 16)) {
    ESP_LOGW(TAG, "audio_send_crypto_failed=1"); return false;
  }
  // ESP-IDF reports temporary Wi-Fi TX exhaustion as ENOMEM. Keep the
  // encrypted datagram intact across retries and bound delays below one
  // 60 ms audio frame. A congested frame can be dropped without ending audio.
  int last_errno = 0;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    if (atomic_load(&cancel) || atomic_load(&requested_end)) return true;
    int sent = send(c->udp, packet, n + 16, 0);
    if (sent >= 0 && (size_t)sent == n + 16) {
      if (attempt) {
        c->tx_retries += attempt;
        if (c->tx_retries <= 2 || (c->tx_retries & 63u) == 0)
          ESP_LOGI(TAG, "audio_send_recovered=%u attempts=%u", c->tx_retries, attempt + 1);
      }
      return true;
    }
    int cause = sent < 0 ? errno : 0;
    last_errno = cause;
    bool transient = sent < 0 && (cause == ENOMEM || cause == ENOBUFS ||
        cause == EAGAIN || cause == EWOULDBLOCK || cause == EINTR);
    if (!transient) {
      ESP_LOGW(TAG, "audio_send_errno=%d sent=%d size=%u", cause, sent, (unsigned)(n + 16));
      return false;
    }
    if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(10));
  }
  ++c->tx_dropped;
  if ((c->tx_dropped & 63u) == 1)
    ESP_LOGW(TAG, "audio_send_dropped=%u errno=%d bytes=%u", c->tx_dropped, last_errno, (unsigned)(n+16));
  return true;
}
static void codec_arena_metric(unsigned step) {
  allocation_failure_metric();
  xz_codec_memory_stats_t memory;
  xz_codec_memory_snapshot(&memory);
  ESP_LOGI(TAG, "arena=%u cap=%u used=%u peak=%u failures=%u last=%u small_used=%u small_live=%u small_peak=%u heap=%u largest=%u min=%u",
           step, (unsigned)memory.capacity, (unsigned)memory.used,
           (unsigned)memory.peak, (unsigned)memory.failures, (unsigned)memory.last_failure,
           (unsigned)memory.small_used, (unsigned)memory.small_live, (unsigned)memory.small_peak,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)esp_get_minimum_free_heap_size());
}
// These exported Opus size queries come from the pinned esp_audio_codec 2.5
// library itself; they allocate no memory and reveal no speech or credentials.
extern int opus_encoder_get_size(int channels);
extern int opus_decoder_get_size(int channels);
static void codec_metric(unsigned codec, unsigned step, int result) {
  ESP_LOGI(TAG, "codec=%u step=%u rc=%d stage=%u need=%d heap=%u largest=%u stack_free=%u",
           codec, step, result, (unsigned)working.stage,
           codec == 1 ? opus_encoder_get_size(1) : opus_decoder_get_size(1),
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
static bool encoder_open(void **enc) {
  esp_opus_enc_config_t ec = ESP_OPUS_ENC_CONFIG_DEFAULT();
  ec.sample_rate = 16000;
  ec.channel = 1;
  ec.bits_per_sample = 16;
  ec.bitrate = 16000;
  ec.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
  ec.complexity = 0;
  ec.enable_dtx = true;
  codec_metric(1, 0, 0);
  int result = esp_opus_enc_open(&ec, sizeof ec, enc);
  codec_metric(1, 1, result);
  return result == ESP_AUDIO_ERR_OK;
}
static bool decoder_open(int rate, void **decoder) {
  esp_opus_dec_cfg_t dc = {.sample_rate=rate, .channel=1,
      .frame_duration=ESP_OPUS_DEC_FRAME_DURATION_60_MS, .self_delimited=false};
  codec_metric(2, 0, 0);
  int result = esp_opus_dec_open(&dc, sizeof dc, decoder);
  codec_metric(2, 1, result);
  return result == ESP_AUDIO_ERR_OK;
}
#define XZ_CODEC_STACK_RESERVE 2048u
static bool codec_probe(void) {
  // Measure real processing before Wi-Fi/TLS consumes the remaining heap. This
  // synthetic frame never acquires hardware, leaves the device, or plays audio.
  void *encoder = NULL, *decoder = NULL;
  int16_t *pcm = calloc(1440, sizeof *pcm);
  uint8_t *packet = malloc(XZ_PACKET_MAX + 16);
  bool ok = false;
  if (!pcm || !packet) {
    status(XZ_ERROR, "语音内存不足，请返回后重试"); goto done;
  }
  uint32_t noise = 0x51ab19u;
  for (unsigned i = 0; i < 960; ++i) {
    noise = noise * 1664525u + 1013904223u;
    pcm[i] = (int16_t)(noise >> 16);
  }
  if (!encoder_open(&encoder)) {
    status(XZ_ERROR, "录音内存不足\n请返回玩法后重试"); goto done;
  }
  esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = 1920};
  esp_audio_enc_out_frame_t out = {.buffer = packet + 16, .len = XZ_PACKET_MAX};
  int result = esp_opus_enc_process(encoder, &in, &out);
  codec_metric(1, 2, result);
  if (result != ESP_AUDIO_ERR_OK || !out.encoded_bytes || out.encoded_bytes > XZ_PACKET_MAX ||
      uxTaskGetStackHighWaterMark(NULL) < XZ_CODEC_STACK_RESERVE) {
    status(XZ_ERROR, "录音准备失败，请重试"); goto done;
  }
  esp_opus_enc_close(encoder); encoder = NULL;
  const unsigned rates[] = {16000, 24000};
  for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
    if (!decoder_open(rates[i], &decoder)) {
      status(XZ_ERROR, "播放内存不足\n请返回玩法后重试"); goto done;
    }
    esp_audio_dec_in_raw_t raw = {.buffer = packet + 16, .len = out.encoded_bytes};
    esp_audio_dec_out_frame_t frame = {.buffer = (uint8_t *)pcm, .len = 2880};
    esp_audio_dec_info_t info = {0};
    result = esp_opus_dec_decode(decoder, &raw, &frame, &info);
    codec_metric(2, 2, result);
    if (result != ESP_AUDIO_ERR_OK || frame.decoded_size != rates[i] * 60u / 1000u * 2u ||
        uxTaskGetStackHighWaterMark(NULL) < XZ_CODEC_STACK_RESERVE) {
      status(XZ_ERROR, "播放准备失败，请重试"); goto done;
    }
    esp_opus_dec_close(decoder); decoder = NULL;
  }
  ok = true;
done:
  if (encoder) esp_opus_enc_close(encoder);
  if (decoder) esp_opus_dec_close(decoder);
  if (pcm) { memset(pcm, 0, 2880); free(pcm); }
  if (packet) { memset(packet, 0, XZ_PACKET_MAX + 16); free(packet); }
  return ok;
}
static void persist_avatar(unsigned value) {
  nvs_handle_t h;
  if (nvs_open("xiaozhi", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "avatar", value);
    nvs_commit(h);
    nvs_close(h);
  }
}
static void finish_status(void) {
  if (working.restart_required) {
    working.stage = XZ_STAGE_DEVICE;
    status(XZ_ERROR, "蓝牙暂不可用，请重启设备");
  } else if (atomic_load(&cancel)) {
    status(XZ_OFF, "");
  } else if (working.phase != XZ_ERROR && working.phase != XZ_OFF) {
    // A boundary may fail before it can publish a detailed status. Retain the
    // stage reached, and never imply that persisted Wi-Fi needs to be retyped.
    const char *reason = working.stage == XZ_STAGE_WIFI ? "Wi-Fi 初始化失败，请重试" :
                         working.stage == XZ_STAGE_BIND ? "设备绑定未完成，请重新获取" :
                         working.stage == XZ_STAGE_CLOUD ? "小智服务连接失败，请重试" :
                         "语音连接中断，请重试";
    status(XZ_ERROR, reason);
  }
}
/* Consume clock-paired ADC/DAC samples while playback owns the codec. The
 * hardware reference remains aligned even when a network packet is delayed. */
static void audio_heap_metric(unsigned stage) {
  ESP_LOGI(TAG, "audio_heap stage=%u dma_free=%u dma_largest=%u rtc_free=%u rtc_largest=%u",
           stage, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM));
}
// Sole voice-worker-owned, fixed-size numeric diagnostics. Never print from
// the DMA service path: USB/log backpressure must not extend its deadline.
typedef struct {
  uint32_t read_peak_us, read_interval_peak_us, write_peak_us;
  uint32_t reads, writes;
  int64_t last_read_at;
  bool pending;
} audio_timing_t;
static audio_timing_t audio_timing;
static void timing_peak(uint32_t *peak, int64_t started) {
  int64_t elapsed = esp_timer_get_time() - started;
  if (elapsed <= 0) return;
  uint32_t value = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
  if (value > *peak) *peak = value;
}
static void audio_timing_report(xz_capture_t *capture, uint32_t echo_peak, uint32_t decode_peak) {
  if (!audio_timing.pending) return;
  audio_timing.pending = false;
  ESP_LOGI(TAG, "audio_timing reads=%u writes=%u read_peak_us=%u read_interval_peak_us=%u write_peak_us=%u decode_peak_us=%u echo_peak_us=%u gaps=%u",
           (unsigned)audio_timing.reads, (unsigned)audio_timing.writes,
           (unsigned)audio_timing.read_peak_us, (unsigned)audio_timing.read_interval_peak_us,
           (unsigned)audio_timing.write_peak_us, (unsigned)decode_peak,
           (unsigned)echo_peak, xz_capture_gap_count(capture));
  xz_bargein_metrics_t observed;
  if (xz_capture_observe(capture, &observed) && observed.frames) {
    ESP_LOGI(TAG, "aec frames=%u peak_us=%u mic_energy=%u ref_energy=%u residual_energy=%u clipped=%u aligned=%u gaps=%u stack_free=%u candidates=%u echo_veto=%u last_clip=%u trigger_far=%u",
             (unsigned)observed.frames, (unsigned)echo_peak,
             (unsigned)(observed.microphone_energy_sum / observed.frames),
             (unsigned)(observed.reference_energy_sum / observed.frames),
             (unsigned)(observed.residual_energy_sum / observed.frames),
             (unsigned)observed.clipped_frames, (unsigned)observed.aligned_frames,
             xz_capture_gap_count(capture), (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)observed.candidate_frames, (unsigned)observed.echo_veto_frames,
             (unsigned)observed.last_clipped_frame, (unsigned)observed.trigger_far_hangover);
  }
}
static bool capture_reply(xz_capture_t *capture, unsigned rate, size_t frames, uint32_t *peak_us) {
  if ((rate != 16000 && rate != 24000) || frames % HAL_AUDIO_DUPLEX_FRAMES) return false;
  int16_t stereo[HAL_AUDIO_DUPLEX_FRAMES * 2];
  while (frames && !atomic_load(&cancel) && !xz_capture_triggered(capture)) {
    size_t count = HAL_AUDIO_DUPLEX_FRAMES, got = 0;
    int64_t read_started = esp_timer_get_time();
    if (audio_timing.reads) timing_peak(&audio_timing.read_interval_peak_us, audio_timing.last_read_at);
    audio_timing.last_read_at = read_started;
    int read_result = hal_audio_read_echo(config.audio, stereo, count, &got);
    timing_peak(&audio_timing.read_peak_us, read_started);
    if (audio_timing.reads != UINT32_MAX) ++audio_timing.reads;
    audio_timing.pending = true;
    if (got != count || (read_result != 0 && read_result != HAL_AUDIO_ECHO_GAP)) return false;
    if (read_result == HAL_AUDIO_ECHO_GAP) {
      if (!xz_capture_discontinuity(capture)) return false;
    }
    int64_t start = esp_timer_get_time();
    bool ok = xz_capture_feed(capture, stereo, count);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    if (elapsed > *peak_us) *peak_us = elapsed;
    memset(stereo, 0, sizeof stereo);
    if (!ok) return false;
    frames -= count;
  }
  return true;
}
static void discard_reply_audio(channel_t *c, uint8_t *packet) {
  for (unsigned i = 0; i < 64; ++i) {
    int got = recv(c->udp, packet, XZ_PACKET_MAX + 16, MSG_DONTWAIT);
    if (got <= 0) break;
    uint32_t sequence;
    if (xz_udp_valid(packet, got, c->rx_sequence, &sequence)) c->rx_sequence = sequence;
  }
}
static bool send_preroll(channel_t *c, void *encoder, xz_capture_t *capture,
                         int16_t *pcm, uint8_t *packet) {
  for (size_t offset = 0; offset < 2400; offset += 480) {
    size_t count = xz_capture_preroll(capture, offset, pcm, 480);
    if (!count) break;
    /* Reuse the existing 60ms Opus input buffer; no second recording buffer.
     * Backwards expansion preserves the first captured syllable at 16kHz. */
    for (size_t i = count; i > 0; --i) pcm[2*i-2] = pcm[2*i-1] = pcm[i-1];
    memset(pcm + count * 2, 0, (960 - count * 2) * sizeof *pcm);
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = 1920};
    esp_audio_enc_out_frame_t out = {.buffer = packet + 16, .len = XZ_PACKET_MAX};
    if (atomic_load(&cancel) || esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK ||
        !send_audio(c, packet + 16, out.encoded_bytes, packet)) return false;
  }
  memset(pcm, 0, 1920);
  return true;
}
static void reconnect_status(xz_phase_t phase, const char *detail) {
  status(phase == XZ_CLOUD ? XZ_WAKING : phase, detail);
}
// A cloud conversation owns the codec arena, socket, TLS client and audio.
// The outer worker owns Wi-Fi, which outlives a normal conversation end.
static bool reply_audio_missing(bool text, uint32_t written, bool stopped, uint64_t idle_ms) {
  // An explicitly empty completed reply is legal. A spoken sentence with no
  // audio is not: give delayed UDP a grace period, then renew its transport.
  return !written && (stopped ? text && idle_ms >= 1500 : idle_ms >= 10000);
}
static session_end_t run_session(bool *wifi_up, xz_quiet_t *quiet, unsigned *missing_replies) {
  session_end_t outcome = SESSION_CLOSED;
  bool warm = *wifi_up && xz_wifi_online();
  if (*wifi_up && !warm) { xz_wifi_close(); *wifi_up = false; }
  memset(&audio_timing, 0, sizeof audio_timing);
  bool audio_owned = false, codec_memory_owned = false, echo_owned = false;
  bool ignore_aborted_tts = false, pending_preroll = false;
  xz_capture_t *capture = NULL;
  uint32_t capture_peak_us = 0;
  uint32_t decode_peak_us = 0;
  channel_t *c = NULL;
  void *encoder = NULL, *decoder = NULL;
  xz_voice_pcm_t *audio_buffer = NULL;
  int16_t *pcm = NULL;
  uint8_t *packet = NULL, *opus = NULL;
  cJSON *cloud = NULL;
  unsigned saved_avatar = atomic_load(&avatar);
  uint64_t last_action = ms(), last_audio = 0;
  bool tts_stop = false, tts_text = false, encoder_measured = false;
  uint32_t tts_received = 0, tts_valid = 0, tts_written = 0;
  working.stage = XZ_STAGE_VOICE;
  if (xz_theme_pending_boot()) {
    if (!*wifi_up && !xz_wifi_open(setup_requested,&cancel,status,working.hotspot,working.password)) goto finish;
    *wifi_up=true; warm=true; setup_requested=false; working.wifi_saved=true;
    status(XZ_CLOUD,"正在下载官方主题");
    if (xz_cloud_time_ready(&cancel,status) && xz_theme_download(&cancel)) {
      persist_avatar(XZ_OFFICIAL_AVATAR);
      esp_restart();
    }
    if (atomic_load(&cancel)) goto finish;
    if (xz_theme_pending_boot()) xz_theme_cancel_download();
    status(XZ_STANDBY,"主题下载失败，按对话继续");
    outcome=SESSION_THEME_FAILED; goto finish;
  }
  if (!xz_codec_memory_begin()) {
    codec_arena_metric(0);
    status(XZ_ERROR, "语音内存不足，请返回后重试"); goto finish;
  }
  codec_memory_owned = true;
  codec_arena_metric(0);
  if (!codec_probe() || atomic_load(&cancel)) goto finish;
  codec_arena_metric(1);
  working.stage = XZ_STAGE_WIFI;
  if (!warm) {
    if (!xz_wifi_open(setup_requested, &cancel, status, working.hotspot, working.password))
      goto finish;
    *wifi_up = true;
    setup_requested = false;
  } else {
    xz_wifi_idle_power(false);
  }
  ESP_LOGI(TAG, "wifi ready heap=%u largest=%u stack_free=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  codec_arena_metric(2);
  ESP_LOGI(TAG,"wifi_signal_rssi=%d",xz_wifi_rssi());
  if (atomic_load(&cancel))
    goto finish;
  working.wifi_saved = true; // xz_wifi_open succeeds only after durable profile save.
  working.stage = XZ_STAGE_CLOUD;
  cloud = xz_cloud_cache_load();
  bool cloud_cached = cloud != NULL;
  ESP_LOGI(TAG,"cloud_cache_hit=%d",cloud_cached);
  if (cloud_cached) {
    working.code[0]=0;
    if (!xz_cloud_time_ready(&cancel,warm ? reconnect_status : status)) goto finish;
    cloud = xz_cloud_refresh_version(cloud, &cancel);
    if (!cloud && !atomic_load(&cancel)) {
      cloud_cached = false;
      cloud = xz_cloud_config(&cancel, warm ? reconnect_status : status, working.code);
    }
  } else cloud = xz_cloud_config(&cancel, warm ? reconnect_status : status, working.code);
  codec_arena_metric(3);
  if (!cloud || atomic_load(&cancel))
    goto finish;
  // MQTT storage is unused during provisioning and the HTTPS handshake.
  c = open_session_channel(&cloud,cloud_cached,warm);
  if (!c) goto finish;
  cJSON_Delete(cloud);
  cloud = NULL;
  codec_arena_metric(5);
  // One codec direction at a time. Buffers are bounded and allocated only in
  // this play.
  if (atomic_load(&natural_interrupt)) {
    capture = xz_capture_create();
    if (!capture) { status(XZ_ERROR, "语音内存不足，请返回后重试"); goto finish; }
  }
  audio_buffer = malloc(sizeof *audio_buffer);
  if (audio_buffer) pcm = audio_buffer->playback;
  packet = malloc(XZ_PACKET_MAX + 16);
  // CTR supports exact in-place input/output. The packet payload is also the
  // codec's Opus scratch; header bytes remain a separate 16-byte prefix.
  opus = packet ? packet + 16 : NULL;
  if (!pcm || !packet) {
    status(XZ_ERROR, "语音内存不足，请返回后重试");
    goto finish;
  }
  audio_heap_metric(0);
  // Verify both mutually exclusive codec allocations before advertising READY.
  // No microphone is acquired and no audio is captured or sent here.
  if(!encoder_open(&encoder)) {
    status(XZ_ERROR,"录音内存不足\n请返回玩法后重试");goto finish;
  }
  ESP_LOGI(TAG,"encoder ready heap=%u largest=%u stack_free=%u",
      (unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      (unsigned)uxTaskGetStackHighWaterMark(NULL));
  esp_opus_enc_close(encoder);encoder=NULL;
  if(!decoder_open(c->rate,&decoder)) {
    status(XZ_ERROR,"播放内存不足\n请返回玩法后重试");goto finish;
  }
  ESP_LOGI(TAG,"decoder ready heap=%u largest=%u stack_free=%u",
      (unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      (unsigned)uxTaskGetStackHighWaterMark(NULL));
  esp_opus_dec_close(decoder);decoder=NULL;
  last_action = ms(); // Provisioning/binding time is not idle conversation time.
  status(XZ_READY, "正在准备收听");
  atomic_store(&requested_talk, true);
  ESP_LOGI(TAG, "voice ready heap=%u largest=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  while (!atomic_load(&cancel) && !atomic_load(&c->failed)) {
    if (saved_avatar != atomic_load(&avatar)) {
      saved_avatar = atomic_load(&avatar);
      persist_avatar(saved_avatar);
    }
    if (atomic_load(&requested_end)) { outcome = SESSION_PARK; break; }
    // Control can arrive in a burst (STT, TTS start, sentence). Consume the
    // queued control first, so the first audio frame is not mistaken for stale
    // audio. Bound the work to keep cancellation and capture responsive.
    for (unsigned control = 0; control < 32 && !atomic_load(&cancel); control++) {
      channel_message_t *message = channel_take(c, 0);
      if (!message) break;
      cJSON *j = cJSON_Parse(message->data);
      free(message);
      if (!j) {
        atomic_store(&c->error, CHANNEL_CONTROL_PARSE);
        atomic_store(&c->failed, true);
        status(XZ_ERROR, channel_error_text(c));
        break;
      }
      cJSON *type = cJSON_GetObjectItem(j, "type"),
            *state = cJSON_GetObjectItem(j, "state"),
            *line = cJSON_GetObjectItem(j, "text");
      if (cJSON_IsString(type)) {
        if (!strcmp(type->valuestring,"mcp")) {
          handle_mcp(c,j);
        } else if (!strcmp(type->valuestring,"llm")) {
          cJSON *e=cJSON_GetObjectItemCaseSensitive(j,"emotion");
          int emotion=cJSON_IsString(e)?xz_emotion_index(e->valuestring):-1;
          if (emotion>=0) {working.emotion=(unsigned)emotion; status(working.phase,working.detail);}
        } else if (!strcmp(type->valuestring, "tts") && cJSON_IsString(state)) {
          /* MQTT is ordered but the old reply's stop/start may already be in
           * flight at abort. Only a new recognized utterance opens its reply. */
          if (ignore_aborted_tts) { cJSON_Delete(j); continue; }
          if (!strcmp(state->valuestring, "start")) {
            quiet->quiet_samples = 0; // Playback/processing time is outside the listening window.
            // Auto endpointing is server-owned. Release capture/encoder before
            // any decoder allocation: both directions share one codec arena.
            if (encoder) {
              codec_metric(1, 4, 0);
              esp_opus_enc_close(encoder);
              encoder = NULL;
            }
            if (audio_owned && working.phase == XZ_LISTEN) {
              // A goodbye may follow in the same control burst, before any
              // playback opens. Close the ADC before relinquishing ownership.
              (void)hal_audio_set_echo_capture(config.audio, false);
              config.release_audio(config.user);
              audio_owned = false;
            }
            if (working.phase != XZ_SPEAK) {
              tts_received = tts_valid = tts_written = 0;
              tts_stop = tts_text = false;
              last_audio = ms();
            }
            status(XZ_SPEAK, "正在回答你");
          } else if (!strcmp(state->valuestring, "stop") && working.phase == XZ_SPEAK) {
            tts_stop = true;
            last_audio = ms();
          } else if (!strcmp(state->valuestring, "sentence_start") &&
                     cJSON_IsString(line) && working.phase == XZ_SPEAK) {
            if (line->valuestring[0]) tts_text = true;
            status(XZ_SPEAK, line->valuestring);
          }
        } else if (!strcmp(type->valuestring, "stt") && cJSON_IsString(line)) {
          // Some sessions deliver the user's transcript after TTS start.
          // Accept a goodbye in either order, before draining queued audio.
          if (xz_is_farewell(line->valuestring)) {
            // Close input and the cloud session before emitting one local goodbye.
            // A queued server reply cannot overlap or duplicate that announcement.
            cJSON_Delete(j);
            outcome = SESSION_INTENT_BYE;
            break;
          }
          if (working.phase == XZ_THINK || working.phase == XZ_LISTEN) {
            ignore_aborted_tts = false;
            if (line->valuestring[0]) quiet->quiet_samples = 0;
            status(working.phase, line->valuestring);
          }
        } else if (!strcmp(type->valuestring, "goodbye")) {
          cJSON *sid = cJSON_GetObjectItem(j, "session_id");
          if (!sid || (cJSON_IsString(sid) && !strcmp(c->session, sid->valuestring))) {
            cJSON_Delete(j);
            outcome = SESSION_RENEW;
            break;
          }
        }
      }
      cJSON_Delete(j);
    }
    if (xz_theme_reboot_requested()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
    }
    if (outcome != SESSION_CLOSED || working.phase == XZ_ERROR || atomic_load(&cancel)) break;
    if (atomic_load(&requested_end)) continue;
    // Ignore a late button event after a reply has already finished. Keeping
    // it queued would incorrectly abort the following answer.
    if (working.phase != XZ_SPEAK && working.phase != XZ_THINK)
      atomic_store(&requested_interrupt, false);
    if (working.phase == XZ_THINK && atomic_exchange(&requested_interrupt, false)) {
      if (!command(c, "abort", NULL)) {
        status(XZ_ERROR, "语音连接中断，请重试"); break;
      }
      ignore_aborted_tts = true;
      pending_preroll = false;
      discard_reply_audio(c, packet);
      status(XZ_READY, "正在继续收听");
      atomic_store(&requested_talk, true);
    }
    if (working.phase == XZ_READY && atomic_exchange(&requested_talk, false)) {
      // Auto mode streams until the server authorizes its response. The only
      // user action during a live session is cancellation, not utterance stop.
      last_action = ms();
      discard_reply_audio(c, packet);
      if (!config.audio || !config.acquire_audio ||
          !config.acquire_audio(config.user)) {
        status(XZ_ERROR, "麦克风暂不可用，请重试");
        break;
      }
      audio_owned = true;
      if (atomic_load(&cancel)) break;
      if (!encoder_open(&encoder) || !command(c, "listen", "start")) {
        status(XZ_ERROR, "录音准备失败，请重试");
        break;
      }
      encoder_measured = false;
      tts_stop = false;
      xz_quiet_begin_listening(audio_buffer);
      status(XZ_LISTEN, "聆听中。。。");
      if (pending_preroll) {
        if (!send_preroll(c, encoder, capture, audio_buffer->listen.samples, packet)) {
          status(XZ_ERROR, "语音发送失败，请重试"); break;
        }
        pending_preroll = false;
      }
    }
    if (ignore_aborted_tts) discard_reply_audio(c, packet);
    if (working.phase == XZ_LISTEN) {
      size_t got = 0;
      if (hal_audio_read(config.audio, audio_buffer->listen.samples, 1920, &got) != 0 || got != 1920) {
        status(XZ_ERROR, "麦克风读取失败，请重试");
        break;
      }
      if (atomic_load(&cancel))
        break;
      if (atomic_load(&requested_end)) continue;
      if (xz_quiet_feed(quiet, audio_buffer)) {
        outcome = SESSION_QUIET_BYE;
        break;
      }
      esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)audio_buffer->listen.samples, .len = 1920};
      esp_audio_enc_out_frame_t out = {.buffer = opus, .len = XZ_PACKET_MAX};
      int result = esp_opus_enc_process(encoder, &in, &out);
      if (!encoder_measured || result != ESP_AUDIO_ERR_OK) {
        codec_metric(1, 3, result); encoder_measured = true;
      }
      if (result != ESP_AUDIO_ERR_OK || uxTaskGetStackHighWaterMark(NULL) < XZ_CODEC_STACK_RESERVE ||
          !send_audio(c, opus, out.encoded_bytes, packet)) {
        status(XZ_ERROR, "语音发送失败，请重试");
        break;
      }
      memset(audio_buffer->listen.samples, 0, sizeof audio_buffer->listen.samples);
    } else if (working.phase == XZ_THINK) {
      // UDP may arrive before TTS-start on MQTT. Keep it in the bounded socket
      // mailbox until the control channel authorizes playback.
      if (ms() - last_action > 45000) {
        status(XZ_ERROR, "回答等待超时，请重试");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      int got = recv(c->udp, packet, XZ_PACKET_MAX + 16, decoder ? MSG_DONTWAIT : 0);
      if (got<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) {
        ESP_LOGW(TAG,"audio_receive_errno=%d",errno);
        outcome=SESSION_AUDIO_LOST; break;
      }
      if (got > 0 && working.phase == XZ_SPEAK && tts_received != UINT32_MAX)
        ++tts_received;
      uint32_t sequence;
      if (got > 0 && xz_udp_valid(packet, got, c->rx_sequence, &sequence) &&
          working.phase == XZ_SPEAK) {
        if (!audio_crypt(c, packet + 16, got - 16, packet, opus)) {
          status(XZ_ERROR, "语音数据异常，请重试");
          break;
        }
        c->rx_sequence = sequence;
        if (tts_valid != UINT32_MAX) ++tts_valid;
        if (!decoder) {
          if (!audio_owned) {
            if (!config.acquire_audio(config.user)) {
              status(XZ_ERROR, "扬声器暂不可用");
              break;
            }
            audio_owned = true;
          }
          if (hal_audio_set_sample_rate(config.audio, c->rate, 16, 1) != 0 ||
              !decoder_open(c->rate,&decoder)) {
            status(XZ_ERROR, "播放准备失败，请重试");
            break;
          }
          audio_heap_metric(1);
          // Flush the previous DSP turn before enabling the duplex stream:
          // its synthetic warm-up must not fill the live RX DMA queue.
          if (capture && !xz_capture_begin(capture, c->rate)) {
            status(XZ_ERROR, "麦克风暂不可用，请重试"); break;
          }
          if (capture && hal_audio_set_echo_capture(config.audio, true) != 0) {
            status(XZ_ERROR, "麦克风暂不可用，请重试"); break;
          }
          echo_owned = capture != NULL;
          memset(&audio_timing, 0, sizeof audio_timing);
          capture_peak_us = decode_peak_us = 0;
          hal_audio_set_out_mute(config.audio, false);
        }
        esp_audio_dec_in_raw_t raw = {.buffer = opus, .len = got - 16};
        esp_audio_dec_out_frame_t frame = {.buffer = (uint8_t *)pcm,
                                           .len = 2880};
        esp_audio_dec_info_t info = {0};
        int64_t decode_started = esp_timer_get_time();
        int result = esp_opus_dec_decode(decoder, &raw, &frame, &info);
        uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_started);
        if (decode_us > decode_peak_us) decode_peak_us = decode_us;
        if (result != ESP_AUDIO_ERR_OK || frame.decoded_size != c->rate * 60u / 1000u * sizeof(int16_t) ||
            uxTaskGetStackHighWaterMark(NULL) < XZ_CODEC_STACK_RESERVE) {
          status(XZ_ERROR, "播放数据异常，请重试");
          break;
        }
        size_t written = 0;
        // Pair TX/RX on complete DMA blocks. IDF's near-full-queue guard may
        // skip an unfinished block without an overflow callback; splitting
        //128-frame DMA blocks into192-frame slices caused silent sample gaps.
        while (written < frame.decoded_size && !atomic_load(&cancel) &&
               !atomic_load(&requested_end) && !atomic_load(&requested_interrupt) &&
               !xz_capture_triggered(capture)) {
          size_t bytes = HAL_AUDIO_DUPLEX_FRAMES * sizeof(int16_t), done = 0;
          int64_t write_started = esp_timer_get_time();
          int write_result = hal_audio_write(config.audio, (uint8_t *)pcm + written, bytes, &done);
          timing_peak(&audio_timing.write_peak_us, write_started);
          if (audio_timing.writes != UINT32_MAX) ++audio_timing.writes;
          audio_timing.pending = true;
          if (write_result != 0 || done != bytes) {
            status(XZ_ERROR, "扬声器播放失败，请重试"); break;
          }
          written += done;
          if (echo_owned && !capture_reply(capture, c->rate, done / sizeof(int16_t), &capture_peak_us)) {
            status(XZ_ERROR, "麦克风读取失败，请重试"); break;
          }
        }
        if (working.phase == XZ_ERROR) break;
        if (written) {
          *missing_replies=0;
          if (tts_written != UINT32_MAX) ++tts_written;
        }
        last_action = last_audio = ms();
      } else if (decoder && echo_owned && working.phase == XZ_SPEAK) {
        /* No UDP does not mean no sound: consume DMA tail and live microphone. */
        if (!capture_reply(capture, c->rate, HAL_AUDIO_DUPLEX_FRAMES, &capture_peak_us)) {
          status(XZ_ERROR, "麦克风读取失败，请重试"); break;
        }
      } else if (decoder && got <= 0) {
        // Button mode has no microphone read to pace an empty UDP mailbox.
        // Yield to UI/MQTT while waiting for the next audio packet.
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      if (atomic_load(&cancel)) break;
      bool button_interrupt = atomic_exchange(&requested_interrupt, false);
      if (working.phase == XZ_SPEAK &&
          (button_interrupt || (echo_owned && xz_capture_triggered(capture)))) {
        if (audio_owned) hal_audio_set_out_mute(config.audio, true);
        // This HAL transition also flushes queued playback in button mode.
        int stopped = audio_owned ? hal_audio_set_echo_capture(config.audio, false) : 0;
        echo_owned = false;
        audio_timing_report(capture, capture_peak_us, decode_peak_us);
        if (decoder) { codec_metric(2, 4, 0); esp_opus_dec_close(decoder); decoder = NULL; }
        if (audio_owned) config.release_audio(config.user);
        audio_owned = false;
        if (stopped || !command(c, "abort", NULL)) {
          status(XZ_ERROR, "语音连接中断，请重试"); break;
        }
        ignore_aborted_tts = true;
        pending_preroll = !button_interrupt;
        discard_reply_audio(c, packet);
        if (button_interrupt) ESP_LOGI(TAG, "button-interrupt");
        else ESP_LOGI(TAG, "barge-in aec_peak_us=%u heap=%u stack_free=%u",
                      (unsigned)capture_peak_us, (unsigned)esp_get_free_heap_size(),
                      (unsigned)uxTaskGetStackHighWaterMark(NULL));
        status(XZ_READY, "正在继续收听");
        atomic_store(&requested_talk, true);
        continue;
      }
      if (working.phase == XZ_SPEAK && reply_audio_missing(tts_text,tts_written,tts_stop,ms()-last_audio)) {
        ESP_LOGW(TAG,"tts_audio_missing=1 text=%d received=%u valid=%u written=%u",
                 tts_text,(unsigned)tts_received,(unsigned)tts_valid,(unsigned)tts_written);
        outcome=SESSION_AUDIO_LOST; break;
      }
      if (working.phase == XZ_SPEAK && tts_stop &&
          ms() - last_audio > (tts_text && !tts_received ? 1500u : 600u)) {
        ESP_LOGI(TAG, "tts received=%u valid=%u written=%u",
                 (unsigned)tts_received, (unsigned)tts_valid, (unsigned)tts_written);
        if (!tts_written && tts_received) {
          status(XZ_ERROR, "未收到语音回复，请重试");
          break;
        }
        // The service can finish a cancelled or empty reply without any UDP
        // audio. Resume the existing listening session after its stop event.
        if (!tts_written) ESP_LOGI(TAG, "tts_empty_reply=1");
        if (decoder) {
          codec_metric(2, 4, 0);
          esp_opus_dec_close(decoder);
          decoder = NULL;
        }
        if (audio_owned) {
          bool had_echo = echo_owned;
          // Button mode must also discard RX samples collected during our
          // own reply before the next listening turn starts.
          int stopped = hal_audio_set_echo_capture(config.audio, false);
          echo_owned = false;
          audio_timing_report(capture, capture_peak_us, decode_peak_us);
          if (stopped) { status(XZ_ERROR, "麦克风暂不可用，请重试"); break; }
          if (had_echo) ESP_LOGI(TAG, "aec peak_us=%u heap=%u stack_free=%u",
                   (unsigned)capture_peak_us, (unsigned)esp_get_free_heap_size(),
                   (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (audio_owned) {
          config.release_audio(config.user);
          audio_owned = false;
        }
        // The tail grace exceeds all seven120-frame I2S DMA buffers (52.5ms at16k).
        // Do not reopen capture while reply audio is pending.
        status(XZ_READY, "正在继续收听");
        atomic_store(&requested_talk, true);
      }
    }
  }
  if(outcome == SESSION_CLOSED && c && atomic_load(&c->failed) && working.phase!=XZ_ERROR && !atomic_load(&cancel))
    status(XZ_ERROR,channel_error_text(c));
finish:
  if (audio_owned) hal_audio_set_echo_capture(config.audio, false);
  audio_timing_report(capture, capture_peak_us, decode_peak_us);
  xz_capture_destroy(capture);
  if (encoder) {
    codec_metric(1, 4, 0); esp_opus_enc_close(encoder);
  }
  if (decoder) {
    codec_metric(2, 4, 0); esp_opus_dec_close(decoder);
  }
  if (audio_owned) {
    hal_audio_set_out_mute(config.audio, true);
    config.release_audio(config.user);
  }
  if (c) {
    channel_release(c,outcome != SESSION_RENEW);
  }
  cJSON_Delete(cloud);
  if (audio_buffer) {
    memset(audio_buffer, 0, sizeof *audio_buffer);
    free(audio_buffer);
  }
  free(packet);
  if (codec_memory_owned) {
    codec_arena_metric(9);
    bool clean = xz_codec_memory_end();
    if (!clean) ESP_LOGW(TAG, "codec arena reclaimed live allocations");
  }
  persist_avatar(atomic_load(&avatar));
  return outcome;
}

unsigned xiaozhi_play_startup_sound(void) {
  if (!xiaozhi_stopped()) return 0;
  // Unique C identifiers are required even for block-scope extern asm labels:
  // GCC otherwise retains an earlier declaration's assembler name.
  extern const uint8_t startup_start[] asm("_binary_startup_magicbox_adpcm_start");
  extern const uint8_t startup_end[] asm("_binary_startup_magicbox_adpcm_end");
  size_t bytes = (size_t)(startup_end - startup_start);
  if (bytes != 25251) return 0; // 50,502 samples; never block boot on a wrong asset.
  atomic_bool idle = false;
  return (unsigned)xz_play_prompt(&config, &idle, &idle, startup_start, bytes);
}

static size_t play_farewell_prompt(void) {
  extern const uint8_t bye_start[] asm("_binary_farewell_zh_adpcm_start");
  extern const uint8_t bye_end[] asm("_binary_farewell_zh_adpcm_end");
  return xz_play_prompt(&config, &cancel, &requested_end, bye_start, (size_t)(bye_end - bye_start));
}

static void run(void *arg) {
  (void)arg;
  xz_theme_boot();
  bool wifi_up = false;
  unsigned rapid_closes = 0, missing_replies = 0;
  xz_quiet_t quiet = {0};
  while (!atomic_load(&cancel)) {
    uint64_t session_at = ms();
    session_end_t reason = run_session(&wifi_up, &quiet, &missing_replies);
    if (atomic_load(&cancel)) break;
    if (!wifi_up || working.phase == XZ_ERROR) break;
    if (atomic_exchange(&requested_end, false)) reason = SESSION_PARK;
    if (reason == SESSION_CLOSED) break;
    const char *idle_text = reason==SESSION_THEME_FAILED?"主题未安装，按对话继续":"按对话开始聊天";
    // Closing a cloud stream does not end our listening window. Only a peer
    // that repeatedly closes immediately enters the recoverable network state.
    if (reason == SESSION_AUDIO_LOST) {
      if (++missing_replies <= 1) {
        ESP_LOGI(TAG,"audio_channel_recovery=1");
        status(XZ_WAKING,"语音连接恢复中");
        continue;
      }
      idle_text="语音暂未收到，按对话重试";
    } else if (reason == SESSION_RENEW) {
      rapid_closes = ms() - session_at < 3000 ? rapid_closes + 1 : 0;
      if (rapid_closes < 3) {
        status(XZ_WAKING, "正在继续聆听");
        continue;
      }
      idle_text = "语音连接未完成，按对话再试";
    } else if (reason == SESSION_QUIET_BYE || reason == SESSION_INTENT_BYE) {
      status(XZ_FAREWELL, "拜拜");
      size_t played = play_farewell_prompt();
      ESP_LOGI(TAG, "farewell cause=%s quiet_ms=%u pcm_samples=%u",
               reason == SESSION_QUIET_BYE ? "quiet" : "intent",
               quiet.quiet_samples / (XZ_QUIET_RATE / 1000), (unsigned)played);
      if (atomic_load(&cancel)) break;
      if (!played && !atomic_load(&requested_end)) idle_text = "声音暂不可用，按对话开始聊天";
    }
    rapid_closes = 0;
    atomic_store(&requested_resume, false);
    atomic_store(&requested_interrupt, false);
    xz_wifi_idle_power(true);
    status(XZ_STANDBY, idle_text);
    ESP_LOGI(TAG, "standby online=%d mic=0 heap=%u", xz_wifi_online(), (unsigned)esp_get_free_heap_size());
    unsigned saved_avatar = atomic_load(&avatar);
    while (!atomic_load(&cancel) && !atomic_exchange(&requested_resume, false)) {
      unsigned selected = atomic_load(&avatar);
      if (selected != saved_avatar) { persist_avatar(selected); saved_avatar = selected; }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!atomic_load(&cancel)) {
      atomic_store(&requested_end, false);
      quiet.quiet_samples = 0;
      missing_replies = 0;
      status(XZ_WAKING, "正在开始对话");
    }
  }
  xz_wifi_close();
  finish_status();
  ESP_LOGI(TAG, "voice cleanup heap=%u stack_free=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  // The caller deletes this task before restoring BLE, so its stack is
  // reclaimed immediately rather than waiting for idle-task self-delete cleanup.
  atomic_store(&worker_finished, true);
  for (;;) vTaskSuspend(NULL);
}
// The pinned DMA AES backend allocates its persistent interrupt in init(),
// even before any encryption. Keep BLE's interrupt occupied while that happens,
// so AES cannot take its slot and force a new descriptor into the freed arena
// when BLE resumes. The SDK's void init aborts on IRQ allocation failure.
static void prepare_aes_interrupt(void) {
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  mbedtls_aes_free(&context); // Clears the local context; the SDK retains its IRQ.
}
static void restore_radio(void) {
  if (radio_owned && config.resume_radio && !config.resume_radio()) {
    working.restart_required = true;
    working.stage = XZ_STAGE_DEVICE;
    status(XZ_ERROR, "蓝牙暂不可用，请重启设备");
    ESP_LOGE(TAG, "BLE restore failed; restart device");
  }
  radio_owned = false;
}
void xiaozhi_service(void) {
  if (!voice_task || !atomic_load(&worker_finished)) return;
  vTaskDelete(voice_task);
  voice_task = NULL;
  atomic_store(&worker_finished, false);
  restore_radio();
  if (atomic_load(&cancel) && !working.restart_required) status(XZ_OFF, "");
  ESP_LOGI(TAG, "voice stopped heap=%u largest=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  atomic_store(&running, false);
}
bool xiaozhi_start(bool setup) {
  bool expected = false;
  if (!atomic_compare_exchange_strong(&running, &expected, true))
    return false;
  setup_requested = setup;
  atomic_store(&welcome_visible, false);
  atomic_store(&setup_confirmed, false);
  atomic_store(&requested_end, false);
  atomic_store(&requested_resume, false);
  memset(&working, 0, sizeof working);
  atomic_store(&cancel, false);
  atomic_store(&requested_talk, false);
  atomic_store(&requested_interrupt, false);
  atomic_store(&worker_finished, false);
  status(XZ_BOOT, "");
  ESP_LOGI(TAG, "voice launch heap=%u largest=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  prepare_aes_interrupt();
  // Called by the existing plays worker without an LVGL lock. Free the radio
  // pools before requesting a large contiguous task stack.
  if (config.pause_radio && !config.pause_radio()) {
    working.restart_required = true;
    working.stage = XZ_STAGE_DEVICE;
    status(XZ_ERROR, "蓝牙释放失败，请重启设备");
    atomic_store(&running, false);
    return false;
  }
  radio_owned = true;
  ESP_LOGI(TAG, "BLE released heap=%u largest=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  // Duplex audio must refill the 32ms TX ring ahead of LVGL rendering (4)
  // and MQTT dispatch (5). I2S reads/writes block regularly, leaving both
  // those tasks CPU time; Wi-Fi and the TCP/IP task retain higher priority.
  if (xTaskCreate(run, "xiaozhi", 28672, NULL, 6, &voice_task) != pdPASS) {
    working.stage = XZ_STAGE_DEVICE;
    status(XZ_ERROR, "内存不足，请返回后重试");
    restore_radio();
    atomic_store(&running, false);
    return false;
  }
  return true;
}
