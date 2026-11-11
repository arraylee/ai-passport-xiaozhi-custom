#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define XZ_PACKET_MAX 1275
#define XZ_JSON_MAX 4096
bool xz_hex16(const char *text, uint8_t out[16]);
bool xz_udp_valid(const uint8_t *data, size_t size, uint32_t previous,
                  uint32_t *sequence);
void xz_text_copy(char *out, size_t cap, const char *text);
// Replace accidental tool-call markup in server subtitles with a player-facing
// progress hint. Ordinary speech, numbers and non-tool code remain unchanged.
const char *xz_display_text(const char *text);
/* Fragment accumulator: a new offset=0 replaces any incomplete message. */
typedef struct {
  char data[XZ_JSON_MAX + 1];
  size_t used, total;
} xz_json_buffer_t;
bool xz_fragment(xz_json_buffer_t *b, const char *data, int length, int offset,
                 int total);

/* Match a complete farewell utterance, ignoring spacing and final punctuation. */
bool xz_is_farewell(const char *text);
