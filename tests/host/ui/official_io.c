/* The native LVGL runs the real theme view; only the board's mapped Flash
 * and persisted palette are replaced for these generic navigation tests. */
#include "xz_theme.h"
#include <string.h>
static xz_theme_t sample;
static bool active, dark = true;
static uint8_t icon[XZ_ICON_BYTES];
const xz_theme_t *xz_theme_installed(void) { return active ? &sample : NULL; }
bool xz_theme_dark(void) { return dark; }
void native_official_install(bool installed) {
  active = installed;
  sample.skin[1] = (xz_skin_t){.ink = 0xffffff, .background = 0x193749};
  sample.skin[0] = (xz_skin_t){.ink = 0x111111, .background = 0xaadddd};
}
const uint8_t *companion_official_icon(unsigned index) {
  if (index >= 6)
    return NULL;
  memset(icon + 2048, 255, 1024);
  return icon;
}
const uint8_t *xz_theme_icon(unsigned emotion, unsigned tick, unsigned *delay) {
  (void)emotion;
  (void)tick;
  *delay = 0;
  return active ? companion_official_icon(0) : NULL;
}
