#pragma once
#include "cJSON.h"
#include "xiaozhi.h"

typedef struct {
  bool (*queue_url)(const char *);
  bool (*queue_reboot)(void);
  bool (*set_dark)(bool);
  const char *version;
  unsigned assets_address, assets_size;
} xz_mcp_device_t;
/* Returns an owned, serialized MQTT reply; notifications do not have a reply. */
char *xz_mcp_reply(const cJSON *request, const char *session, const xz_config_t *settings,
                   const xz_mcp_device_t *device);
