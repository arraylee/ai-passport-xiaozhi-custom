#include "xz_theme_transfer.h"
#include <stdlib.h>
#include <string.h>
bool xz_theme_receive(const xz_transfer_t *io, void *ctx, int64_t length, size_t capacity) {
  uint8_t head[12];
  size_t got = 0;
  while (got < sizeof head) {
    int k = io->read(ctx, head + got, sizeof head - got);
    if (k <= 0 || (size_t)k > sizeof head - got)
      return false;
    got += k;
  }
  uint32_t body = xz_u32(head + 8), count = xz_u32(head);
  if (capacity < sizeof head || body > capacity - sizeof head || !count || count > 80 ||
      body < 44 * count || xz_u32(head + 4) > 65535 ||
      (length > 0 && (uint64_t)length != body + sizeof head))
    return false;
  size_t total = sizeof head + body, erased = 0;
  uint32_t sum = 0;
  uint8_t *buf = malloc(1024);
  if (!buf)
    return false;
  bool ok = true;
  while (got < total && ok) {
    size_t want = total - got;
    if (want > 1024)
      want = 1024;
    int k = io->read(ctx, buf, want);
    if (k <= 0 || (size_t)k > want) {
      ok = false;
      break;
    }
    while (got + (size_t)k > erased) {
      if (erased + 4096 > capacity || !io->erase(ctx, erased, 4096)) {
        ok = false;
        break;
      }
      erased += 4096;
    }
    if (!ok || !io->write(ctx, got, buf, k)) {
      ok = false;
      break;
    }
    for (int i = 0; i < k; i++)
      sum += buf[i];
    got += (size_t)k;
  }
  // An incomplete download never advertises a valid pack after reset.
  if (ok)
    ok = (sum & 65535) == xz_u32(head + 4) && io->complete(ctx) &&
         io->write(ctx, 0, head, sizeof head);
  free(buf);
  return ok;
}
