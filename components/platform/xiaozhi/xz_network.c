#include "xz_network.h"
#include "xz_cloud_cache.h"
#include "xz_portal.h"
#include "xz_wifi_profiles.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "xz_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static esp_netif_t *sta, *ap;
static httpd_handle_t http;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static bool wifi_inited, net_started, sntp_started;
// A channel hint survives ordinary session teardown, but never pins an AP.
// Only the voice worker updates it after a verified successful connection.
static wifi_ap_record_t recent_ap;
static atomic_bool got_ip, credentials_ready, wifi_connecting, connect_failed;
static atomic_bool disconnect_waiting, disconnect_received, wifi_attempt_issued;
static atomic_uint disconnect_events;
static atomic_int failure_reason;
typedef enum { PORTAL_IDLE, PORTAL_CONNECTING, PORTAL_CONNECTED, PORTAL_FAILED } portal_state_t;
typedef enum { PORTAL_NONE, PORTAL_AUTH, PORTAL_NOT_FOUND, PORTAL_TIMEOUT,
               PORTAL_SAVE_FAILED, PORTAL_UNKNOWN } portal_error_t;
/* Pack state and error into one atomic snapshot for the HTTP thread. */
static atomic_uint portal_outcome;
static void portal_outcome_set(portal_state_t state, portal_error_t error) {
  atomic_store(&portal_outcome, ((unsigned)state << 8) | (unsigned)error);
}
static wifi_config_t pending;
static SemaphoreHandle_t credential_guard;
static int portal_dns = -1;
static uint8_t portal_ip[4];
static void portal_dns_stop(void) {
  if (portal_dns >= 0) close(portal_dns);
  portal_dns = -1;
}
static bool portal_dns_start(void) {
  esp_netif_ip_info_t info;
  if (esp_netif_get_ip_info(ap, &info) != ESP_OK) return false;
  memcpy(portal_ip, &info.ip.addr, 4);
  esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4}};
  dns.ip.u_addr.ip4 = info.ip;
  uint8_t offer_dns = 2;
  esp_err_t e = esp_netif_dhcps_stop(ap);
  if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return false;
  if (esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                             &offer_dns, sizeof offer_dns) != ESP_OK ||
      esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
      esp_netif_dhcps_start(ap) != ESP_OK) return false;
  portal_dns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in address = {.sin_family=AF_INET, .sin_port=htons(53),
                                .sin_addr={.s_addr=info.ip.addr}};
  if (portal_dns < 0 || bind(portal_dns,(struct sockaddr *)&address,sizeof address)) {
    portal_dns_stop(); return false;
  }
  return true;
}
static void portal_dns_poll(void) {
  if (portal_dns < 0) return;
  // Reuse the provisioning worker: no extra task or persistent buffer.
  uint8_t packet[512];
  for (unsigned i=0;i<4;i++) {
    struct sockaddr_in peer;
    socklen_t len=sizeof peer;
    int n=recvfrom(portal_dns,packet,sizeof packet,MSG_DONTWAIT,
                   (struct sockaddr *)&peer,&len);
    if (n<=0) break;
    size_t reply=xz_portal_dns_reply(packet,(size_t)n,sizeof packet,portal_ip);
    if (reply) sendto(portal_dns,packet,reply,0,(struct sockaddr *)&peer,len);
  }
}
static bool wifi_current_link(void) {
  wifi_ap_record_t record;
  wifi_config_t configured = {0};
  if (esp_wifi_sta_get_ap_info(&record) != ESP_OK ||
      esp_wifi_get_config(WIFI_IF_STA, &configured) != ESP_OK) return false;
  size_t length = strnlen((char *)configured.sta.ssid, 32);
  bool matches = length && length == strnlen((char *)record.ssid, 32) &&
                 !memcmp(configured.sta.ssid, record.ssid, length);
  memset(&configured, 0, sizeof configured);
  return matches;
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    if (atomic_load(&wifi_connecting) && !atomic_load(&disconnect_waiting) &&
        wifi_current_link()) {
      atomic_store(&connect_failed, false);
      atomic_store(&failure_reason, 0);
      atomic_store(&got_ip, true);
    }
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (atomic_load(&disconnect_waiting)) {
      atomic_store(&got_ip, false);
      atomic_store(&disconnect_received, true);
      atomic_fetch_add(&disconnect_events, 1);
      return;
    }
    if (!atomic_load(&wifi_connecting)) return;
    const wifi_event_sta_disconnected_t *event = data;
    wifi_config_t configured = {0};
    if (event && event->ssid_len && event->ssid_len <= 32 &&
        esp_wifi_get_config(WIFI_IF_STA, &configured) == ESP_OK) {
      size_t length = strnlen((char *)configured.sta.ssid, 32);
      bool other = length != event->ssid_len || memcmp(configured.sta.ssid, event->ssid, length);
      memset(&configured, 0, sizeof configured);
      if (other) return;
    }
    /* Driver association is current; event delivery has no attempt generation.
     * A late event must not clear a new link's IP or abort its DHCP wait. */
    wifi_ap_record_t current;
    if (esp_wifi_sta_get_ap_info(&current) == ESP_OK) return;
    atomic_store(&got_ip, false);
    atomic_store(&failure_reason, event ? event->reason : 0);
    atomic_store(&connect_failed, true); // Diagnostic only until IP/deadline.
    if (event) ESP_LOGI("xiaozhi", "wifi disconnected reason=%u", event->reason);
  }
}
void xz_identity(char mac[18], char uuid[37]) {
  uint8_t id[6];
  esp_read_mac(id, ESP_MAC_WIFI_STA);
  snprintf(mac, 18, "%02x:%02x:%02x:%02x:%02x:%02x", id[0], id[1], id[2], id[3],
           id[4], id[5]);
  nvs_handle_t h;
  size_t n = 37;
  if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) {
    uuid[0] = 0;
    return;
  }
  if (nvs_get_str(h, "uuid", uuid, &n) != ESP_OK || strlen(uuid) != 36) {
    uint8_t u[16];
    esp_fill_random(u, sizeof u);
    u[6] = (u[6] & 15) | 0x40;
    u[8] = (u[8] & 63) | 0x80;
    snprintf(
        uuid, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
        u[11], u[12], u[13], u[14], u[15]);
    if (nvs_set_str(h, "uuid", uuid) != ESP_OK || nvs_commit(h) != ESP_OK)
      uuid[0] = 0;
  }
  nvs_close(h);
}
extern const char portal[] asm("_binary_portal_html_start");
static esp_err_t portal_get(httpd_req_t *r) {
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  return httpd_resp_send(r, portal, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t portal_redirect(httpd_req_t *r, httpd_err_code_t error) {
  (void)error;
  // Android/iOS probes use several paths; iOS also needs a nonempty body.
  httpd_resp_set_status(r, "303 See Other");
  httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  return httpd_resp_sendstr(r, "<a href='http://192.168.4.1/'>AI PASSPORT Wi-Fi</a>");
}
static esp_err_t portal_unavailable(httpd_req_t *r,const char *detail) {
  httpd_resp_set_status(r,"503 Service Unavailable");
  httpd_resp_set_hdr(r,"Cache-Control","no-store");
  return httpd_resp_sendstr(r,detail);
}
static esp_err_t portal_networks(httpd_req_t *r) {
  if (!xz_portal_accept_input()) return portal_unavailable(r,"sleeping");
  if (!ap || !credential_guard ||
      xSemaphoreTake(credential_guard, 0) != pdTRUE)
    return portal_unavailable(r,"connecting");
  if (atomic_load(&wifi_connecting) || atomic_load(&credentials_ready) ||
      atomic_load(&disconnect_waiting) ||
      ((atomic_load(&portal_outcome) >> 8) == PORTAL_CONNECTING ||
       (atomic_load(&portal_outcome) >> 8) == PORTAL_CONNECTED)) {
    xSemaphoreGive(credential_guard);
    return portal_unavailable(r,"connecting");
  }
  wifi_scan_config_t cfg={.show_hidden=false,.scan_type=WIFI_SCAN_TYPE_ACTIVE,
                          .scan_time.active={.min=50,.max=100}};
  if(esp_wifi_scan_start(&cfg,true)!=ESP_OK){
    esp_wifi_clear_ap_list();
    xSemaphoreGive(credential_guard);
    return portal_unavailable(r,"scan_failed");
  }
  xz_wifi_option_t options[XZ_WIFI_OPTION_MAX]={0};size_t count=0;
  // Drain one record at a time instead of allocating the driver's full list.
  for(unsigned i=0;i<128;i++){
    wifi_ap_record_t record;
    if(esp_wifi_scan_get_ap_record(&record)!=ESP_OK)break;
    count=xz_wifi_option_add(options,count,record.ssid,strnlen((char *)record.ssid,32),
       record.rssi,record.authmode!=WIFI_AUTH_OPEN&&record.authmode!=WIFI_AUTH_OWE,record.primary);
  }
  esp_wifi_clear_ap_list(); // Includes records beyond the bounded drain limit.
  xSemaphoreGive(credential_guard);
  ESP_LOGI("xiaozhi","wifi scan options=%u heap=%u stack_free=%u",(unsigned)count,
           (unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
  httpd_resp_set_type(r,"application/json; charset=utf-8");
  httpd_resp_set_hdr(r,"Cache-Control","no-store");
  esp_err_t e=httpd_resp_sendstr_chunk(r,"{\"networks\":[");
  for(size_t i=0;e==ESP_OK&&i<count;i++){
    char label[33]={0},hex[65];memcpy(label,options[i].ssid,options[i].length);
    xz_ssid_encode(options[i].ssid,options[i].length,hex);
    cJSON *j=cJSON_CreateObject();
    if(!j || !cJSON_AddStringToObject(j,"id",hex) ||
       !cJSON_AddStringToObject(j,"ssid",label) ||
       !cJSON_AddBoolToObject(j,"secure",options[i].secure)){
      cJSON_Delete(j);return ESP_ERR_NO_MEM;
    }
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!text)return ESP_ERR_NO_MEM;
    if(i)e=httpd_resp_sendstr_chunk(r,",");
    if(e==ESP_OK)e=httpd_resp_sendstr_chunk(r,text);
    free(text);
  }
  if(e==ESP_OK)e=httpd_resp_sendstr_chunk(r,"]}");
  return e==ESP_OK?httpd_resp_send_chunk(r,NULL,0):e;
}
static esp_err_t portal_post(httpd_req_t *r) {
  if (r->content_len <= 0 || r->content_len > 512)
    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "输入过长");
  if (!xz_portal_accept_input()) return portal_unavailable(r,"sleeping");
  char body[513];
  int n = 0;
  while (n < r->content_len) {
    int got = httpd_req_recv(r, body + n, r->content_len - n);
    if (got <= 0) {
      memset(body, 0, sizeof body);
      return ESP_FAIL;
    }
    n += got;
  }
  body[n] = 0;
  /* cJSON strings are NUL-terminated. Reject JSON NUL escapes rather than
   * silently connecting with a truncated password; escaped literal slashes stay valid. */
  bool clean = memchr(body, 0, (size_t)n) == NULL;
  for (int i=0; clean && i<n; i++) {
    if (body[i] == '\\') {
      if (i+5<n && !memcmp(body+i+1, "u0000", 5)) clean=false;
      i++;
    }
  }
  cJSON *root = clean ? cJSON_ParseWithOpts(body, NULL, true) : NULL,
        *s = cJSON_GetObjectItem(root, "ssid"),
        *p = cJSON_GetObjectItem(root, "password"),
        *hex = cJSON_GetObjectItem(root,"ssid_hex");
  uint8_t ssid[32]={0};size_t ssid_length=0;
  bool valid_ssid=cJSON_IsString(hex)?xz_ssid_decode(hex->valuestring,ssid,&ssid_length):
       cJSON_IsString(s)&&strlen(s->valuestring)>0&&strlen(s->valuestring)<=32;
  if(valid_ssid&&!cJSON_IsString(hex)){
    ssid_length=strlen(s->valuestring);memcpy(ssid,s->valuestring,ssid_length);
  }
  bool valid = cJSON_IsObject(root) && valid_ssid && cJSON_IsString(p) &&
               strlen(p->valuestring) <= 63 &&
               (!p->valuestring[0] || strlen(p->valuestring) >= 8);
  bool busy = false;
  if (valid) {
    if (!credential_guard || xSemaphoreTake(credential_guard, 0) != pdTRUE) busy = true;
    else {
      busy = atomic_load(&credentials_ready) || atomic_load(&wifi_connecting) ||
             ((atomic_load(&portal_outcome) >> 8) == PORTAL_CONNECTING ||
              (atomic_load(&portal_outcome) >> 8) == PORTAL_CONNECTED);
      if (!busy) {
        memset(&pending, 0, sizeof pending);
        memcpy(pending.sta.ssid, ssid, ssid_length);
        memcpy(pending.sta.password, p->valuestring, strlen(p->valuestring));
        portal_outcome_set(PORTAL_CONNECTING, PORTAL_NONE);
        atomic_store(&credentials_ready, true);
      }
      xSemaphoreGive(credential_guard);
    }
  }
  cJSON_Delete(root);
  memset(body, 0, sizeof body);
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_type(r, "application/json; charset=utf-8");
  if (!valid) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "invalid credentials");
  httpd_resp_set_status(r, busy ? "409 Conflict" : "202 Accepted");
  return httpd_resp_sendstr(r, busy ? "{\"accepted\":false}" : "{\"accepted\":true}");
}
static esp_err_t portal_status(httpd_req_t *r) {
  static const char *states[] = {"idle", "connecting", "connected", "failed"};
  static const char *errors[] = {"none", "auth", "not_found", "timeout", "save_failed", "unknown"};
  unsigned outcome = atomic_load(&portal_outcome), state = outcome >> 8, error = outcome & 255;
  char body[80];
  snprintf(body, sizeof body, "{\"state\":\"%s\",\"error\":\"%s\"}",
           states[state < 4 ? state : PORTAL_FAILED], errors[error < 6 ? error : PORTAL_UNKNOWN]);
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_type(r, "application/json; charset=utf-8");
  return httpd_resp_sendstr(r, body);
}
static void load_wifi_profiles(xz_wifi_profiles_t *profiles) {
  memset(profiles, 0, sizeof *profiles);
  nvs_handle_t h;
  if (nvs_open("xiaozhi", NVS_READONLY, &h) != ESP_OK)
    goto driver_profile;
  uint8_t blob[XZ_WIFI_PROFILES_BLOB_SIZE];
  size_t n = sizeof blob;
  bool ok = nvs_get_blob(h, "wifi_profiles", blob, &n) == ESP_OK &&
            xz_wifi_profiles_decode(profiles, blob, n);
  if (!ok) {
    // Import the existing single-network setting without clearing any NVS keys.
    wifi_config_t legacy = {0};
    n = sizeof legacy;
    if (nvs_get_blob(h, "wifi", &legacy, &n) == ESP_OK && n == sizeof legacy &&
        strnlen((char *)legacy.sta.password, 64) < 64)
      xz_wifi_profiles_remember(profiles, legacy.sta.ssid,
          strnlen((char *)legacy.sta.ssid, 32), (char *)legacy.sta.password);
    memset(&legacy, 0, sizeof legacy);
  }
  memset(blob, 0, sizeof blob);
  nvs_close(h);
driver_profile:
  if (!profiles->count) {
    /* Older firmware may have used the SDK's own persisted station config.
     * Read only: importing never clears SDK NVS or writes an unproven profile. */
    wifi_config_t saved = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK &&
        strnlen((char *)saved.sta.password, 64) < 64)
      xz_wifi_profiles_remember(profiles, saved.sta.ssid,
          strnlen((char *)saved.sta.ssid, 32), (char *)saved.sta.password);
    memset(&saved, 0, sizeof saved);
  }
}
static bool save_wifi_profile(xz_wifi_profiles_t *profiles,
                              const wifi_config_t *config) {
  xz_wifi_profiles_t next = *profiles;
  if (!xz_wifi_profiles_remember(&next, config->sta.ssid,
        strnlen((char *)config->sta.ssid, 32), (char *)config->sta.password)) {
    memset(&next, 0, sizeof next);
    return false;
  }
  uint8_t blob[XZ_WIFI_PROFILES_BLOB_SIZE], old[sizeof blob];
  if (!xz_wifi_profiles_encode(&next, blob, sizeof blob)) {
    memset(&next, 0, sizeof next);
    return false;
  }
  nvs_handle_t h;
  if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK) {
    memset(blob, 0, sizeof blob); memset(&next, 0, sizeof next); return false;
  }
  size_t n = sizeof old;
  bool same = nvs_get_blob(h, "wifi_profiles", old, &n) == ESP_OK &&
              n == sizeof old && !memcmp(old, blob, n);
  // Flash only changes when a successful network/password/recency changes.
  bool ok = same || (nvs_set_blob(h, "wifi_profiles", blob, sizeof blob) == ESP_OK &&
                     nvs_commit(h) == ESP_OK);
  memset(blob, 0, sizeof blob); memset(old, 0, sizeof old);
  nvs_close(h);
  if (ok) *profiles = next;
  memset(&next, 0, sizeof next);
  return ok;
}

// Association/authentication and DHCP get a realistic window, with a total cap.
#define XZ_WIFI_AUTO_BUDGET_MS 20000
#define XZ_WIFI_ATTEMPT_MS 10000
#define XZ_WIFI_DISCONNECT_MS 1000
static int64_t wifi_now_ms(void) { return esp_timer_get_time() / 1000; }
static bool wifi_quiesce(atomic_bool *cancel, int64_t deadline) {
  atomic_store(&wifi_connecting, false);
  atomic_store(&got_ip, false);
  if (!atomic_load(&disconnect_waiting)) {
    bool issued = atomic_exchange(&wifi_attempt_issued, false);
    wifi_ap_record_t current;
    /* On a newly started idle STA, disconnect can succeed without producing an
     * event. No previous connect was issued here, so there is nothing to drain. */
    if (!issued && esp_wifi_sta_get_ap_info(&current) != ESP_OK) {
      atomic_store(&disconnect_received, false);
      return !atomic_load(cancel) && wifi_now_ms() < deadline;
    }
    atomic_store(&disconnect_received, false);
    atomic_store(&disconnect_waiting, true);
    if (esp_wifi_disconnect() != ESP_OK) atomic_store(&disconnect_received, true);
  }
  int64_t until = wifi_now_ms() + XZ_WIFI_DISCONNECT_MS, quiet_since = wifi_now_ms();
  unsigned observed = atomic_load(&disconnect_events);
  if (until > deadline) until = deadline;
  while (!atomic_load(cancel) && wifi_now_ms() < until) {
    unsigned events = atomic_load(&disconnect_events);
    if (events != observed) { observed = events; quiet_since = wifi_now_ms(); }
    /* A queued auth failure and our own leave can be separate callbacks. Drain
     * the burst, including when disconnect reports already-disconnected. */
    if (atomic_load(&disconnect_received) && wifi_now_ms() - quiet_since >= 100) {
      atomic_store(&disconnect_waiting, false);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  /* Cancelling an unassociated in-flight connect may also produce no event.
   * The bounded wait is over: current driver association, rather than an event
   * that is not guaranteed, decides whether scan/connect can proceed. Late
   * events remain diagnostic and cannot abort the next DHCP attempt. */
  wifi_ap_record_t current;
  if (esp_wifi_sta_get_ap_info(&current) != ESP_OK) {
    atomic_store(&disconnect_waiting, false);
    return !atomic_load(cancel) && wifi_now_ms() < deadline;
  }
  return false;
}
static bool connect_wifi(const wifi_config_t *config, atomic_bool *cancel, int64_t deadline) {
  if (!wifi_quiesce(cancel, deadline)) return false;
  atomic_store(&connect_failed, false);
  atomic_store(&failure_reason, 0);
  wifi_config_t next = *config;
  next.sta.scan_method = WIFI_FAST_SCAN;
  next.sta.channel = recent_ap.primary >= 1 && recent_ap.primary <= 13 &&
                     !memcmp(next.sta.ssid, recent_ap.ssid, 32) ? recent_ap.primary : 0;
  next.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  next.sta.failure_retry_cnt = 0;
  atomic_store(&wifi_connecting, true);
  bool ok = esp_wifi_set_config(WIFI_IF_STA, &next) == ESP_OK;
  if (ok) {
    /* Set before issuing: even a rejected/partially handled request must be
     * cancelled before another scan or connection is started. */
    atomic_store(&wifi_attempt_issued, true);
    ok = esp_wifi_connect() == ESP_OK;
  }
  memset(&next, 0, sizeof next);
  if (!ok) { atomic_store(&connect_failed, true); atomic_store(&wifi_connecting, false); }
  return ok;
}
static bool try_saved(const xz_wifi_profile_t *p, atomic_bool *cancel,
                       int64_t deadline, wifi_config_t *connected) {
  memset(connected, 0, sizeof *connected);
  memcpy(connected->sta.ssid, p->ssid, p->ssid_len);
  memcpy(connected->sta.password, p->password, strlen(p->password));
  if (!connect_wifi(connected, cancel, deadline)) return false;
  int64_t until = wifi_now_ms() + XZ_WIFI_ATTEMPT_MS;
  if (until > deadline) until = deadline;
  while (!atomic_load(cancel) && !atomic_load(&got_ip) &&
         wifi_now_ms() < until)
    vTaskDelay(pdMS_TO_TICKS(50));
  bool connected_ok = atomic_load(&got_ip) && !atomic_load(cancel) && wifi_current_link();
  if (connected_ok) {
    memset(&recent_ap, 0, sizeof recent_ap);
    (void)esp_wifi_sta_get_ap_info(&recent_ap);
  }
  return connected_ok;
}
static bool auto_wifi(xz_wifi_profiles_t *profiles, atomic_bool *cancel,
                       xz_status_fn status, wifi_config_t *connected) {
  const int64_t deadline = wifi_now_ms() + XZ_WIFI_AUTO_BUDGET_MS;
  status(XZ_WIFI, "正在连接已保存的 Wi-Fi");
  /* The last successful profile is authoritative; a phone-style scan can miss
   * hidden/temporarily absent beacons and must never gate reconnect. */
  if (profiles->count && !atomic_load(cancel) &&
      try_saved(&profiles->items[0], cancel, deadline, connected)) return true;
  int8_t signals[XZ_WIFI_PROFILE_MAX]; memset(signals, INT8_MIN, sizeof signals);
  if (!atomic_load(cancel) && wifi_now_ms() < deadline && wifi_quiesce(cancel, deadline)) {
    wifi_scan_config_t scan = {.show_hidden=false, .scan_type=WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active={.min=50,.max=100}};
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
      for (unsigned i=0; i<128; i++) {
        wifi_ap_record_t record;
        if (esp_wifi_scan_get_ap_record(&record) != ESP_OK) break;
        size_t length = strnlen((char *)record.ssid, 32);
        for (unsigned j=1; j<profiles->count; j++) {
          const xz_wifi_profile_t *p = &profiles->items[j];
          if (record.primary <= 14 && p->ssid_len == length &&
              !memcmp(p->ssid, record.ssid, length) && record.rssi > signals[j])
            signals[j] = record.rssi;
        }
      }
    }
    esp_wifi_clear_ap_list();
  }
  uint8_t order[XZ_WIFI_PROFILE_MAX];
  size_t count = xz_wifi_profiles_rank(profiles, signals, order);
  /* Try non-visible saved profiles too if scan failed/empty, after visible ones. */
  for (unsigned j=1; j<profiles->count; j++)
    if (signals[j] == INT8_MIN) order[count++] = j;
  for (size_t i=0; i<count && !atomic_load(cancel) && wifi_now_ms()<deadline; i++) {
    if (try_saved(&profiles->items[order[i]], cancel, deadline, connected)) return true;
  }
  wifi_quiesce(cancel, deadline);
  memset(connected, 0, sizeof *connected);
  return false;
}
static portal_error_t wifi_failure(void) {
  int reason = atomic_load(&failure_reason);
  if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_AUTH_EXPIRE ||
      reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
    return PORTAL_AUTH;
  if (reason == WIFI_REASON_NO_AP_FOUND ||
      reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
      reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
      reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) return PORTAL_NOT_FOUND;
  return atomic_load(&connect_failed) ? PORTAL_UNKNOWN : PORTAL_TIMEOUT;
}
bool xz_wifi_open(bool setup, atomic_bool *cancel, xz_status_fn status,
                  char hotspot[32], char password[12]) {
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
    return false;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
    return false;
  sta = esp_netif_create_default_wifi_sta();
  if (!sta)
    return false;
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&init) != ESP_OK)
    return false;
  wifi_inited = true;
  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_event, NULL,
                                          &wifi_handler) != ESP_OK ||
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          wifi_event, NULL,
                                          &ip_handler) != ESP_OK)
    return false;
  wifi_config_t config = {0};
  xz_wifi_profiles_t profiles;
  load_wifi_profiles(&profiles);
  // Includes legacy SDK credentials. An explicit reconfiguration or a wake
  // back into a previous portal goes straight through to the existing form.
  if (!setup && !profiles.count && !xz_wait_for_setup(cancel, status)) return false;
  /* Read any SDK-persisted profile before switching future writes to RAM. */
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    memset(&profiles, 0, sizeof profiles);
    return false;
  }
  atomic_store(&got_ip, false);
  atomic_store(&credentials_ready, false);
  atomic_store(&wifi_connecting, false);
  atomic_store(&disconnect_waiting, false);
  atomic_store(&disconnect_received, false);
  atomic_store(&disconnect_events, 0);
  atomic_store(&wifi_attempt_issued, false);
  atomic_store(&connect_failed, false);
  portal_outcome_set(PORTAL_IDLE, PORTAL_NONE);
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK)
    return false;
  net_started = true;
  // This network exists only for the active voice/provisioning session.
  // Modem sleep buffers downlink until DTIM, producing bursts and long gaps
  // instead of the steady UDP cadence needed by the small duplex audio ring.
  // xz_wifi_close stops/deinitializes Wi-Fi when the play exits.
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
    memset(&profiles, 0, sizeof profiles);
    return false;
  }
  if (!setup && profiles.count && auto_wifi(&profiles, cancel, status, &config)) {
    bool saved = save_wifi_profile(&profiles, &config);
    memset(&config, 0, sizeof config);
    if (saved) {
      memset(&profiles, 0, sizeof profiles);
      return true;
    }
    wifi_quiesce(cancel, wifi_now_ms() + XZ_WIFI_DISCONNECT_MS);
    status(XZ_SETUP, "网络信息保存失败，请重新配网");
  }
  if (atomic_load(cancel)) {
    memset(&profiles, 0, sizeof profiles); return false;
  }
  {
    credential_guard = xSemaphoreCreateMutex();
    if (!credential_guard)
      return false;
    ap = esp_netif_create_default_wifi_ap();
    if (!ap)
      return false;
    uint8_t id[6];
    esp_read_mac(id, ESP_MAC_WIFI_STA);
    snprintf(hotspot, 32, "AI-PASSPORT-%02X%02X", id[4], id[5]);
    password[0] = 0;
    wifi_config_t ac = {0};
    strlcpy((char *)ac.ap.ssid, hotspot, sizeof ac.ap.ssid);
    ac.ap.ssid_len = strlen(hotspot);
    ac.ap.max_connection = 1;
    ac.ap.authmode = WIFI_AUTH_OPEN;
    ac.ap.channel = 1;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ac) != ESP_OK)
      return false;
    if (!portal_dns_start()) {
      status(XZ_ERROR, "配网页面未启动，请重试");
      return false;
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 4096;
    hc.max_open_sockets = 2;
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 3;
    if (httpd_start(&http, &hc) != ESP_OK)
      return false;
    httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = portal_get},
                networks = {.uri="/networks",.method=HTTP_GET,.handler=portal_networks},
                outcome = {.uri="/wifi/status",.method=HTTP_GET,.handler=portal_status},
                post = {.uri = "/wifi",
                        .method = HTTP_POST,
                        .handler = portal_post};
    if (httpd_register_uri_handler(http, &get) != ESP_OK ||
        httpd_register_uri_handler(http,&networks)!=ESP_OK ||
        httpd_register_uri_handler(http, &post) != ESP_OK ||
        httpd_register_uri_handler(http, &outcome) != ESP_OK ||
        httpd_register_err_handler(http, HTTPD_404_NOT_FOUND, portal_redirect) != ESP_OK)
      return false;
    status(XZ_SETUP, !setup && profiles.count ? "未连上，请重新配网" : "");
  }
  int64_t attempt_deadline = 0, success_until = 0;
  bool trying = false;
  // The device idle policy stops the worker for sleep. Merely waiting for
  // a phone must never close an otherwise healthy setup hotspot.
  while (!atomic_load(cancel)) {
    portal_dns_poll();
    if (!trying && !success_until && atomic_load(&credentials_ready) &&
        xSemaphoreTake(credential_guard, 0) == pdTRUE) {
      config = pending;
      memset(&pending, 0, sizeof pending);
      atomic_store(&credentials_ready, false);
      atomic_store(&wifi_connecting, true); // Lock scans/POST before releasing guard.
      xSemaphoreGive(credential_guard);
      trying = true;
      attempt_deadline = wifi_now_ms() + XZ_WIFI_AUTO_BUDGET_MS;
      if (!connect_wifi(&config, cancel, attempt_deadline)) atomic_store(&connect_failed, true);
      status(XZ_WIFI, "正在连接 Wi-Fi");
    }
    if (trying && atomic_load(&got_ip) && wifi_current_link()) {
      trying = false;
      bool saved = save_wifi_profile(&profiles, &config);
      memset(&config, 0, sizeof config);
      if (saved) {
        portal_outcome_set(PORTAL_CONNECTED, PORTAL_NONE);
        success_until = wifi_now_ms() + 4000;
        status(XZ_WIFI, "Wi-Fi 已连接并保存");
      } else {
        /* Keep AP and the old profile list available for another submission. */
        wifi_quiesce(cancel, wifi_now_ms() + XZ_WIFI_DISCONNECT_MS);
        portal_outcome_set(PORTAL_FAILED, PORTAL_SAVE_FAILED);
        status(XZ_SETUP, "网络信息保存失败，请重试");
      }
    }
    if (trying && wifi_now_ms() >= attempt_deadline) {
      portal_error_t error = wifi_failure();
      wifi_quiesce(cancel, attempt_deadline);
      trying = false;
      memset(&config, 0, sizeof config);
      portal_outcome_set(PORTAL_FAILED, error);
      status(XZ_SETUP, "连接失败，请检查网络和密码后重试");
    }
    if (success_until && (!atomic_load(&got_ip) || !wifi_current_link())) {
      success_until = 0;
      portal_error_t error = wifi_failure();
      wifi_quiesce(cancel, wifi_now_ms() + XZ_WIFI_DISCONNECT_MS);
      portal_outcome_set(PORTAL_FAILED, error);
      status(XZ_SETUP, "Wi-Fi 已断开，请重试");
    }
    if (success_until && wifi_now_ms() >= success_until) {
      /* The phone gets several polls before AP/DNS/HTTP are freed for cloud TLS. */
      portal_dns_stop();
      if (http) { httpd_stop(http); http = NULL; }
      if (ap) { esp_wifi_set_mode(WIFI_MODE_STA); esp_netif_destroy_default_wifi(ap); ap = NULL; }
      memset(&profiles, 0, sizeof profiles);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  wifi_quiesce(cancel, wifi_now_ms() + XZ_WIFI_DISCONNECT_MS);
  memset(&config, 0, sizeof config);
  memset(&profiles, 0, sizeof profiles);
  return false;
}
// Called by the single Wi-Fi owner. Check the actual association and address,
// not the fact that the device has credentials saved in NVS.
int xz_wifi_rssi(void) {
  wifi_ap_record_t record;
  return esp_wifi_sta_get_ap_info(&record)==ESP_OK ? record.rssi : 0;
}
bool xz_wifi_online(void) {
  wifi_ap_record_t record;
  esp_netif_ip_info_t info;
  return net_started && sta && esp_wifi_sta_get_ap_info(&record) == ESP_OK &&
         esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0;
}
void xz_wifi_idle_power(bool idle) {
  if (net_started) (void)esp_wifi_set_ps(idle ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}
void xz_wifi_close(void) {
  atomic_store(&wifi_connecting,false);
  atomic_store(&credentials_ready,false);
  atomic_store(&disconnect_waiting,false);
  atomic_store(&wifi_attempt_issued,false);
  portal_outcome_set(PORTAL_IDLE, PORTAL_NONE);
  portal_dns_stop();
  if (http) {
    httpd_stop(http);
    http = NULL;
  }
  if (sntp_started) {
    esp_sntp_stop();
    sntp_started = false;
  }
  if (wifi_handler) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_handler);
    wifi_handler = NULL;
  }
  if (ip_handler) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          ip_handler);
    ip_handler = NULL;
  }
  if (net_started) {
    esp_wifi_stop();
    net_started = false;
  }
  if (wifi_inited) {
    esp_wifi_deinit();
    wifi_inited = false;
  }
  if (ap) {
    esp_netif_destroy_default_wifi(ap);
    ap = NULL;
  }
  if (sta) {
    esp_netif_destroy_default_wifi(sta);
    sta = NULL;
  }
  if (credential_guard) {
    vSemaphoreDelete(credential_guard);
    credential_guard = NULL;
  }
  memset(&pending, 0, sizeof pending);
}
typedef struct {
  char *body;
  size_t used;
  bool overflow;
} response_t;
static esp_err_t http_event(esp_http_client_event_t *event) {
  response_t *r = event->user_data;
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    if ((size_t)event->data_len > XZ_JSON_MAX - r->used) {
      r->overflow = true;
      return ESP_FAIL;
    }
    memcpy(r->body + r->used, event->data, event->data_len);
    r->used += event->data_len;
    r->body[r->used] = 0;
  }
  return ESP_OK;
}
typedef enum { CLOUD_OK, CLOUD_IDENTITY, CLOUD_MEMORY, CLOUD_NETWORK,
               CLOUD_TLS, CLOUD_HTTP, CLOUD_RESPONSE } cloud_error_t;
static cloud_error_t cloud_failure;
static bool cloud_retryable;
static cJSON *cloud_request(const char *payload) {
  cloud_failure=CLOUD_OK;cloud_retryable=false;
  char mac[18], uuid[37];
  xz_identity(mac, uuid);
  if (!uuid[0]) {
    cloud_failure=CLOUD_IDENTITY;
    return NULL;
  }
  response_t r = {.body = calloc(1, XZ_JSON_MAX + 1)};
  if (!r.body) {
    cloud_failure=CLOUD_MEMORY;cloud_retryable=true;
    return NULL;
  }
  esp_http_client_config_t cfg = {.url =
                                      "https://api.tenclass.net/xiaozhi/ota/",
                                  .method = HTTP_METHOD_POST,
                                  .crt_bundle_attach = esp_crt_bundle_attach,
                                  .timeout_ms = 6000,
                                  .event_handler = http_event,
                                  .user_data = &r,
                                  .buffer_size = 1024,
                                  .buffer_size_tx = 1024,
                                  .disable_auto_redirect = true};
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) {
    cloud_failure=CLOUD_MEMORY;cloud_retryable=true;
    free(r.body);
    return NULL;
  }
  esp_http_client_set_header(h, "Activation-Version", "1");
  esp_http_client_set_header(h, "Device-Id", mac);
  esp_http_client_set_header(h, "Client-Id", uuid);
  esp_http_client_set_header(h, "Content-Type", "application/json");
  esp_http_client_set_header(h, "Accept-Language", "zh-CN");
  esp_http_client_set_header(h, "User-Agent", "ai-passport/" XZ_CONSOLE_VERSION);
  esp_http_client_set_post_field(h, payload, strlen(payload));
  esp_err_t e = esp_http_client_perform(h);
  int code = esp_http_client_get_status_code(h);
  int tls_error=0,tls_flags=0;
  esp_err_t tls_status=esp_http_client_get_and_clear_last_tls_error(h,&tls_error,&tls_flags);
  esp_http_client_cleanup(h);
  cJSON *root =
      e == ESP_OK && code == 200 && !r.overflow ? cJSON_Parse(r.body) : NULL;
  if (!root) {
    // ESP-TLS normally stores -ret (positive); accept either sign from the
    // transport and also its separate allocator status before HTTP wraps it.
    int64_t tls_magnitude=tls_error<0?-(int64_t)tls_error:tls_error;
    if(e==ESP_ERR_NO_MEM || tls_status==ESP_ERR_NO_MEM ||
       tls_magnitude==-(int64_t)MBEDTLS_ERR_SSL_ALLOC_FAILED ||
       tls_magnitude==-(int64_t)MBEDTLS_ERR_X509_ALLOC_FAILED) cloud_failure=CLOUD_MEMORY;
    else if(tls_error || tls_flags) cloud_failure=CLOUD_TLS;
    else if(e!=ESP_OK) cloud_failure=CLOUD_NETWORK;
    else if(code!=200) cloud_failure=CLOUD_HTTP;
    else cloud_failure=CLOUD_RESPONSE;
    bool certificate_failed=tls_flags != 0 ||
        tls_magnitude==-(int64_t)MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    cloud_retryable=!certificate_failed && (cloud_failure==CLOUD_MEMORY ||
                    cloud_failure==CLOUD_NETWORK || cloud_failure==CLOUD_TLS ||
                    code==429 || code>=500);
    // Diagnostics contain only numeric status/allocation metadata, never body,
    // identity, speech, cloud credentials or the activation code.
    ESP_LOGW("xiaozhi","cloud request failed err=%s http=%d tls=%d flags=%d bytes=%u overflow=%d heap=%u largest=%u",
             esp_err_to_name(e),code,tls_error,tls_flags,(unsigned)r.used,r.overflow,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  memset(r.body, 0, XZ_JSON_MAX + 1);
  free(r.body);
  return root;
}
static const char *cloud_failure_text(void) {
  switch(cloud_failure) {
  case CLOUD_MEMORY:return "连接内存不足\n请返回玩法后重试";
  case CLOUD_IDENTITY:return "设备信息未就绪\n请重试";
  case CLOUD_TLS:return "小智安全连接失败\nWi-Fi 已保存，请重试";
  case CLOUD_HTTP:return "Wi-Fi 配置已保存\n小智服务暂不可用，请重试";
  case CLOUD_RESPONSE:return "小智返回信息暂不可用\n请重试";
  default:return "Wi-Fi 配置已保存\n无法连接小智，请重试";
  }
}
bool xz_cloud_time_ready(atomic_bool *cancel, xz_status_fn status) {
  // Certificate validity requires real wall time. Never disable TLS
  // verification.
  if (time(NULL) < 1735689600) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    sntp_started = true;
    for (int i = 0; i < 150 && !atomic_load(cancel) && time(NULL) < 1735689600;
         i++)
      vTaskDelay(pdMS_TO_TICKS(100));
    if (atomic_load(cancel)) return false;
    if (time(NULL) < 1735689600) {
      status(XZ_ERROR, "时间未校准\nWi-Fi 已保存，请重试");
      return false;
    }
  }
  return !atomic_load(cancel);
}
static cJSON *request_device_report(void) {
  char mac[18], uuid[37], payload[640];
  xz_identity(mac, uuid);
  int size = snprintf(payload, sizeof payload,
           "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":8388608,"
           "\"psram_size\":0,\"mac_address\":\"%s\",\"uuid\":\"%s\",\"chip_"
           "model_name\":\"esp32c3\",\"application\":{\"name\":\"ai-passport\","
           "\"version\":\"%s\",\"idf_version\":\"%s\"},\"board\":{\"type\":"
           "\"ai-passport-leogie\",\"name\":\"ai-passport\",\"mac\":\"%s\"}}",
           mac, uuid, XZ_CONSOLE_VERSION,
           esp_app_get_description()->idf_ver, mac);
  if (size < 0 || (size_t)size >= sizeof payload) {
    cloud_failure = CLOUD_RESPONSE;
    cloud_retryable = false;
    return NULL;
  }
  return cloud_request(payload);
}

cJSON *xz_cloud_refresh_version(cJSON *cached, const atomic_bool *cancel) {
  static uint64_t next_attempt_ms;
  uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
  if (atomic_load(cancel) || xz_cloud_cache_version_current(cached) || now < next_attempt_ms)
    return cached;
  next_attempt_ms = now + 300000;
  // The previous connection profile remains in NVS. Release its parsed copy
  // before HTTPS so the TLS handshake has the normal amount of contiguous RAM.
  cJSON_Delete(cached);
  cJSON *fresh = request_device_report();
  bool saved = !atomic_load(cancel) && xz_cloud_cache_save(fresh);
  ESP_LOGI("xiaozhi", "console_report version=%s saved=%d", XZ_CONSOLE_VERSION, saved);
  if (saved) return fresh;
  cJSON_Delete(fresh);
  return atomic_load(cancel) ? NULL : xz_cloud_cache_load();
}

cJSON *xz_cloud_config(atomic_bool *cancel, xz_status_fn status,
                       char code[24]) {
  status(XZ_CLOUD, "正在连接小智");
  ESP_LOGI("xiaozhi", "cloud_config_request=1 version=%s", XZ_CONSOLE_VERSION);
  if (!xz_cloud_time_ready(cancel,status)) return NULL;
  // Wall-clock window, independent of HTTP speed: older users have time to
  // log in and bind. A transient poll failure must not instantly end setup.
  const uint64_t deadline=(uint64_t)esp_timer_get_time()/1000+30u*60u*1000u;
  bool have_code=false;
  while (!atomic_load(cancel) && (uint64_t)esp_timer_get_time()/1000<deadline) {
    cJSON *root=NULL;
    for(unsigned retry=0;retry<3&&!atomic_load(cancel);retry++) {
      root=request_device_report();
      if(root || !cloud_retryable || retry==2)break;
      if(!have_code)status(XZ_CLOUD,"小智连接较慢\n正在重试，请稍候");
      for(unsigned wait=0;wait<20&&!atomic_load(cancel);wait++)vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(atomic_load(cancel)){cJSON_Delete(root);return NULL;}
    if (!root) {
      // Once the user has a binding code, a transient retry batch is not the
      // end of the 30-minute binding window. Keep the code and retry without
      // making them re-enter Wi-Fi. Permanent errors and memory failures still
      // return their precise, actionable error to the caller.
      if (have_code && cloud_retryable && cloud_failure != CLOUD_MEMORY) {
        status(XZ_BIND, "网络暂时波动\nWi-Fi 已保存，正在重试");
        for (unsigned wait=0; wait<50 && !atomic_load(cancel) &&
             (uint64_t)esp_timer_get_time()/1000<deadline; ++wait)
          vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      status(XZ_ERROR, cloud_failure_text());
      return NULL;
    }
    cJSON *activation = cJSON_GetObjectItem(root, "activation"),
          *v = cJSON_GetObjectItem(activation, "code");
    if (cJSON_IsString(v) && v->valuestring[0]) {
      if(strlen(v->valuestring)!=6 || strspn(v->valuestring,"0123456789")!=6) {
        cJSON_Delete(root);status(XZ_ERROR,"绑定信息暂不可用\n请重新获取");return NULL;
      }
      if(!have_code || strcmp(code,v->valuestring)) {
        (void)xz_cloud_cache_clear();
        xz_text_copy(code, 24, v->valuestring);
        status(XZ_BIND,"请前往 xiaozhi.me 登录并绑定设备\n绑定时需要输入下方数字");
      }
      have_code=true;
      cJSON_Delete(root);
      for (int i = 0; i < 100 && !atomic_load(cancel) &&
                      (uint64_t)esp_timer_get_time()/1000<deadline; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (cJSON_IsObject(cJSON_GetObjectItem(root, "mqtt"))) {
      code[0] = 0; // Later voice errors must not display an obsolete binding code.
      return root;
    }
    cJSON_Delete(root);
    status(XZ_ERROR, "小智返回信息不完整\n请重试");
    return NULL;
  }
  if(!atomic_load(cancel))status(XZ_ERROR,have_code?
      "绑定等待已超时\n请点重新获取再绑定设备":"小智连接等待超时\n请重试");
  return NULL;
}
