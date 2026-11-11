#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include "xz_services.h"
#include "xz_codec_memory.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>

uint64_t xz_online_seconds(void) { return (uint64_t)time(NULL); }
bool xz_home_load(xz_city_t *city) {
  nvs_handle_t h;
  size_t len = sizeof *city;
  if (nvs_open("xiaozhi", NVS_READONLY, &h) != ESP_OK)
    return false;
  bool ok = nvs_get_blob(h, "weather_city_v1", city, &len) == ESP_OK && len == sizeof *city &&
            city->name[0] && memchr(city->name, 0, sizeof city->name) &&
            memchr(city->region, 0, sizeof city->region) && city->lat_e5 >= -9000000 &&
            city->lat_e5 <= 9000000 && city->lon_e5 >= -18000000 && city->lon_e5 <= 18000000;
  nvs_close(h);
  return ok;
}
bool xz_home_save(const xz_city_t *city) {
#ifdef PASSPORT_TEST_SERVICES
  extern bool xiaozhi_test_active(void);
  if (xiaozhi_test_active()) return true; // Fixed bench city must not change user preferences.
#endif
  nvs_handle_t h;
  if (nvs_open("xiaozhi", NVS_READWRITE, &h) != ESP_OK)
    return false;
  bool ok =
      nvs_set_blob(h, "weather_city_v1", city, sizeof *city) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}
static bool public_dns(const char *url) {
  if (!xz_url_public(url))
    return false;
  const char *start = strstr(url, "://") + 3;
  size_t n = strcspn(start, ":/?");
  char host[201];
  memcpy(host, start, n);
  host[n] = 0;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *list = NULL;
  if (getaddrinfo(host, NULL, &hints, &list) || !list)
    return false;
  bool valid = true;
  for (struct addrinfo *p = list; p; p = p->ai_next) {
    uint32_t ip = ntohl(((struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr);
#ifdef PASSPORT_TEST_SERVICES
    ESP_LOGI("xiaozhi", "online_dns_v4=%u", (unsigned)ip);
#endif
    unsigned a = ip >> 24, b = (ip >> 16) & 255;
    if (!a || a == 10 || a == 127 || a >= 224 || (a == 169 && b == 254) ||
        (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
        (a == 100 && b >= 64 && b <= 127) || (a == 198 && (b == 18 || b == 19)))
      valid = false;
  }
  freeaddrinfo(list);
  return valid;
}
typedef struct {
  char location[256];
  bool icy;
} header_t;
static esp_err_t header(esp_http_client_event_t *e) {
  header_t *h = e->user_data;
  if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
    if (!strcasecmp(e->header_key, "Location") && strlen(e->header_value) < sizeof h->location)
      strcpy(h->location, e->header_value);
    if (!strcasecmp(e->header_key, "icy-metaint") && strcmp(e->header_value, "0"))
      h->icy = true;
  }
  return ESP_OK;
}
static bool visit(const char *url, xz_body_fn receive, void *ctx) {
  char current[640];
  if (strlen(url) >= sizeof current)
    return false;
  /* The weather/directory query can exceed the station URL limit after percent encoding.
   * Validate its HTTPS origin separately. */
  strcpy(current, url);
  for (unsigned hops = 0; hops <= 3 && !xz_online_cancelled(); hops++) {
    const char *authority = strstr(current, "://");
    if (!authority)
      return false;
    size_t origin = strcspn(authority + 3, "/?") + (size_t)(authority + 3 - current);
    char base[232];
    if (origin >= sizeof base)
      return false;
    memcpy(base, current, origin);
    base[origin] = 0;
    if (!public_dns(base))
      return false;
    header_t h = {0};
    esp_http_client_config_t cfg = {.url = current,
                                    .timeout_ms = 6000,
                                    .buffer_size = 1024,
                                    .buffer_size_tx = 512,
                                    .crt_bundle_attach = esp_crt_bundle_attach,
                                    // Long streams must not resize TLS input on every
                                    // record while the MP3 decoder owns heap blocks.
                                    .tls_dyn_buf_strategy = HTTP_TLS_DYN_BUF_RX_STATIC,
                                    .disable_auto_redirect = true,
                                    .keep_alive_enable = true,
                                    .event_handler = header,
                                    .user_data = &h,
                                    .user_agent = "FoloToy-AI-Passport/2.1"};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
      return false;
    ESP_LOGI("xiaozhi", "online_transport_tls=%u", !strncmp(current, "https://", 8));
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    bool ok = false, redirect = false;
    int last_read = 0, read_errno = 0;
    size_t total = 0;
    int64_t request_start = esp_timer_get_time();
    if (!xz_online_cancelled() && esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0) {
      int status = esp_http_client_get_status_code(client);
      if (status >= 300 && status < 400 && h.location[0]) {
        if (h.location[0] == '/' && h.location[1] != '/') {
          snprintf(current, sizeof current, "%s%s", base, h.location);
          redirect = true;
        } else if (xz_url_public(h.location)) {
          strcpy(current, h.location);
          redirect = true;
        }
      } else if (status == 200 && !h.icy) {
        char chunk[1024];
        int64_t progress = esp_timer_get_time(), start = progress;
        bool api = strstr(base, "open-meteo.com") || strstr(base, ".api.radio-browser.info");
        // Radio has an independent audio consumer and a compressed jitter
        // buffer. Bound a stalled socket read so the down key stays responsive.
        if (!api) esp_http_client_set_timeout_ms(client, 300);
        while (!xz_online_cancelled()) {
          if (api && (total > 65536 || esp_timer_get_time() - start > 9000000))
            break;
          int n = esp_http_client_read(client, chunk, sizeof chunk);
          last_read = n;
          read_errno = errno;
          if (n > 0) {
            progress = esp_timer_get_time();
            total += (unsigned)n;
            if (!receive(chunk, (size_t)n, ctx)) {
              ok = true;
              break;
            }
          } else if (n == 0) {
            ok = esp_http_client_is_complete_data_received(client);
            break;
          } else if (n != -ESP_ERR_HTTP_EAGAIN || esp_timer_get_time() - progress >= 8000000)
            break;
        }
      }
    }
    int http_status=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI("xiaozhi","online_http=%d ok=%u redirect=%u",http_status,ok,redirect);
    ESP_LOGI("xiaozhi","online_body=%u last_read=%d errno=%d elapsed_ms=%u",(unsigned)total,last_read,read_errno,
             (unsigned)((esp_timer_get_time()-request_start)/1000));
    if (!redirect)
      return ok && !xz_online_cancelled();
  }
  return false;
}
bool xz_http_visit(const char *url, xz_body_fn receive, void *ctx) {
  bool loan = xz_codec_memory_loan_begin();
  bool ok = visit(url, receive, ctx);
  // visit cleans up the HTTP/TLS client on success, redirect, cancellation and
  // every failure path. No network allocation may outlive this ownership scope.
  if (loan && !xz_codec_memory_loan_end()) return false;
  return ok;
}
