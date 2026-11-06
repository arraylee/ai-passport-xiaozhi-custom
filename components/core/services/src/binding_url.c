#include "services/binding_url.h"
#include "hal/hal_identity.h"
#include <string.h>

static int append(char *out, size_t cap, size_t *n, const char *text, int encode) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        unsigned char c = *p;
        int plain = !encode || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        size_t width = plain ? 1u : 3u;
        if (*n + width >= cap) return -1;
        if (plain) out[(*n)++] = (char)c;
        else { out[(*n)++] = '%'; out[(*n)++] = hex[c >> 4]; out[(*n)++] = hex[c & 15]; }
    }
    out[*n] = '\0';
    return 0;
}

int binding_url_build(char *out, size_t cap, const char *sn, const char *key) {
    if (!out || !cap) return -1;
    out[0] = '\0';
    if (!sn || !key || !*sn || !*key || strlen(sn) >= HAL_IDENTITY_SN_CAP ||
        strlen(key) >= HAL_IDENTITY_KEY_CAP) return -1;
    size_t n = 0;
    if (append(out, cap, &n, "https://ai-passport.folotoy.cn?s=", 0) ||
        append(out, cap, &n, sn, 1) || append(out, cap, &n, "&k=", 0) ||
        append(out, cap, &n, key, 1)) {
        out[0] = '\0';
        return -1;
    }
    return (int)n;
}
