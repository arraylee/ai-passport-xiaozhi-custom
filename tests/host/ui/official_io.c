/* The native LVGL runs the real theme view; only the board's mapped Flash
 * and persisted palette are replaced for these generic navigation tests. */
#include "xz_theme.h"
#include <string.h>
static xz_theme_t sample;
static bool active, dark = true;
static uint8_t icon[XZ_ICON_BYTES];
static uint8_t portrait[64*64*3];
static bool large;
const xz_theme_t *xz_theme_installed(void) { return active ? &sample : NULL; }
bool xz_theme_dark(void) { return dark; }
void native_official_install(bool installed) {
  active = installed;
  large = false;
  sample.skin[1] = (xz_skin_t){.ink = 0xffffff, .background = 0x193749};
  sample.skin[0] = (xz_skin_t){.ink = 0x111111, .background = 0xaadddd};
}
void native_official_large(bool value) {
  large = value;
  for(unsigned i=0;i<64*64;i++) {
    portrait[2*i]=0;portrait[2*i+1]=0xf8;
    portrait[2*64*64+i]=(i%64)<32?0:255;
  }
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
  return active ? (large ? portrait : companion_official_icon(0)) : NULL;
}
unsigned xz_theme_icon_edge(unsigned emotion, unsigned *display_edge) {
  (void)emotion;
  *display_edge = active&&large ? 128 : XZ_ICON_EDGE;
  return active&&large ? 64 : XZ_ICON_EDGE;
}
