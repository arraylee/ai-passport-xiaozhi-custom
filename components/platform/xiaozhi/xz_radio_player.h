#pragma once
#include "xiaozhi.h"
#include "xz_services.h"

/* The worker has closed MQTT/Opus and lent its reserved arena to radio/TLS. */
bool xz_radio_stream(const xz_config_t *config, const xz_station_t *station);
