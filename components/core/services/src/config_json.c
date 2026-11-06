// components/core/services/src/config_json.c —— cJSON 部分更新 profile(纯逻辑)
#include "services/config_json.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <string.h>

/* Clamp while still a double: converting first is undefined for large JSON
 * numbers. All numbers have passed the finite-value preflight below. */
static int clamp_number(double v, int lo, int hi) {
    if (v <= lo) return lo;
    if (v >= hi) return hi;
    return (int)v;
}

/* Return one Unicode scalar's width, or zero. Check the remaining byte count
 * before looking at continuation bytes, including a truncated F0 lead byte. */
static size_t utf8_step(const unsigned char *s, size_t remaining) {
    if (!remaining) return 0;
    unsigned char lead = s[0];
    size_t width = lead < 0x80 ? 1 :
                   (lead >= 0xc2 && lead <= 0xdf) ? 2 :
                   (lead >= 0xe0 && lead <= 0xef) ? 3 :
                   (lead >= 0xf0 && lead <= 0xf4) ? 4 : 0;
    if (!width || width > remaining) return 0;
    for (size_t i = 1; i < width; ++i) {
        if ((s[i] & 0xc0) != 0x80) return 0;
    }
    if (width == 3 && ((lead == 0xe0 && s[1] < 0xa0) ||
                      (lead == 0xed && s[1] >= 0xa0))) return 0;
    if (width == 4 && ((lead == 0xf0 && s[1] < 0x90) ||
                      (lead == 0xf4 && s[1] > 0x8f))) return 0;
    return width;
}

static bool valid_utf8(const char *s, size_t len) {
    for (size_t i = 0; i < len;) {
        size_t step = utf8_step((const unsigned char *)s + i, len - i);
        if (!step) return false;
        i += step;
    }
    return true;
}

static void copy_utf8_chars(char *dst, int cap, const char *src, int max_chars) {
    size_t len = strlen(src), used = 0;
    int chars = 0;
    while (used < len && chars < max_chars) {
        size_t step = utf8_step((const unsigned char *)src + used, len - used);
        if (!step || used + step >= (size_t)cap) break;
        used += step;
        chars++;
    }
    memcpy(dst, src, used);
    dst[used] = '\0';
}

static void copy_trunc(char *dst, int cap, const char *src) {
    copy_utf8_chars(dst, cap, src, INT_MAX);
}

/* cJSON accepts floating-point overflow and does not validate raw UTF-8.
 * Validate the whole object before any profile field or action is committed. */
static bool valid_values(const cJSON *item) {
    for (; item; item = item->next) {
        if (cJSON_IsNumber(item) && !isfinite(item->valuedouble)) return false;
        if (cJSON_IsString(item) && item->valuestring &&
            !valid_utf8(item->valuestring, strlen(item->valuestring))) return false;
        if (item->child && !valid_values(item->child)) return false;
    }
    return true;
}

static int take_intro(cJSON *o, const char *k, char *dst, int cap, bool *flag) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(it) || !it->valuestring) return 0;
    copy_utf8_chars(dst, cap, it->valuestring, 28);
    if (flag) *flag = true;
    return 1;
}

// 取字符串键:存在且类型为字符串 → 回填 → 返回 1
static int take_str(cJSON *o, const char *k, char *dst, int cap, bool *flag) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(it) || it->valuestring == NULL) return 0;
    copy_trunc(dst, cap, it->valuestring);
    if (flag) *flag = true;
    return 1;
}

static bool valid_avatar_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 15) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) return false;
    }
    return true;
}

int config_json_apply(profile_data_t *d, const char *json, int len, cfg_changed_t *chg) {
    cfg_changed_t local = {0};
    if (chg) *chg = local;
    if (!d || !json || len <= 0 || !valid_utf8(json, (size_t)len)) return -1;
    const char *parsed_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, (size_t)len, &parsed_end, false);
    if (!root) return -1;
    const char *end = json + len;
    while (parsed_end < end && (*parsed_end == ' ' || *parsed_end == '\t' ||
                               *parsed_end == '\r' || *parsed_end == '\n')) parsed_end++;
    /* Accept either an exact length-delimited JSON value or one trailing NUL. */
    if (parsed_end < end && *parsed_end == '\0' && parsed_end + 1 == end) parsed_end++;
    cJSON *time = cJSON_GetObjectItemCaseSensitive(root, "time");
    /* Last second of year 9999. This also fits the platform's uint64_t
     * microsecond clock, unlike accepting the whole int64_t seconds range. */
    if (!cJSON_IsObject(root) || parsed_end != end || !valid_values(root) ||
        (cJSON_IsNumber(time) && time->valuedouble > 253402300799.0)) {
        cJSON_Delete(root);
        return -1;
    }
    profile_data_t *target = d;
    profile_data_t next = *d;
    d = &next;

    int n = 0;
    cfg_changed_t c = {0};

    // `nickname` is the canonical key.  `name` remains a compatibility alias,
    // but must not overwrite nickname when both are present.
    if (take_str(root, "nickname", d->name, (int)sizeof d->name, &c.name)) n++;
    else if (take_str(root, "name", d->name, (int)sizeof d->name, &c.name)) n++;
    if (take_str(root, "title", d->role, (int)sizeof d->role, &c.role)) n++;
    else if (take_str(root, "role", d->role, (int)sizeof d->role, &c.role)) n++;
    if (take_intro(root, "intro", d->subtitle, (int)sizeof d->subtitle, &c.subtitle)) n++;
    else if (take_intro(root, "subtitle", d->subtitle, (int)sizeof d->subtitle, &c.subtitle)) n++;

    cJSON *avatar = cJSON_GetObjectItemCaseSensitive(root, "avatar_name");
    if (cJSON_IsString(avatar) && avatar->valuestring != NULL &&
        valid_avatar_name(avatar->valuestring)) {
        copy_trunc(d->avatar_name, (int)sizeof d->avatar_name, avatar->valuestring);
        c.avatar_name = true;
        n++;
    }

    cJSON *it;
    it = cJSON_GetObjectItemCaseSensitive(root, "battery");
    if (cJSON_IsNumber(it)) { d->battery = clamp_number(it->valuedouble, 0, 100); c.battery = true; n++; }

    it = cJSON_GetObjectItemCaseSensitive(root, "level");
    if (cJSON_IsNumber(it)) { d->level = clamp_number(it->valuedouble, 0, 999); c.level = true; n++; }
    it = cJSON_GetObjectItemCaseSensitive(root, "xp");
    if (cJSON_IsNumber(it)) { d->xp = clamp_number(it->valuedouble, 0, INT_MAX); c.level = true; n++; }
    it = cJSON_GetObjectItemCaseSensitive(root, "xp_max");
    if (cJSON_IsNumber(it)) { d->xp_max = clamp_number(it->valuedouble, 1, INT_MAX); c.level = true; n++; }

    it = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (cJSON_IsNumber(it)) {
        d->token = clamp_number(it->valuedouble, 0, 1000000000); c.token = true; n++;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "online");
    if (cJSON_IsBool(it)) { d->online = cJSON_IsTrue(it); c.online = true; n++; }

    // 时间同步:不写 profile(不持久化),仅上报 epoch 让上层设系统时钟(RTC)。
    // 取值用 valuedouble 以容纳 >2^31 的秒数(int 会溢出)。
    it = cJSON_GetObjectItemCaseSensitive(root, "time");
    if (cJSON_IsNumber(it) && it->valuedouble > 0) {
        c.time = true; c.time_epoch = (int64_t)it->valuedouble; n++;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "token_max");
    if (cJSON_IsNumber(it)) {
        d->token_max = clamp_number(it->valuedouble, 1, 1000000000); c.token_max = true; n++;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "sleep_min");
    if (cJSON_IsNumber(it)) {
        d->sleep_min = clamp_number(it->valuedouble, 0, SLEEP_MIN_MAX); c.sleep_min = true; n++;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if (cJSON_IsNumber(it)) {
        d->volume = clamp_number(it->valuedouble, 0, 100); c.volume = true; n++;
    }

    // 游戏积分清零:动作键,不写 profile。小程序读走 …0013 后下发此键清零工牌端。
    it = cJSON_GetObjectItemCaseSensitive(root, "game_clear");
    if ((cJSON_IsBool(it) && cJSON_IsTrue(it)) || (cJSON_IsNumber(it) && it->valuedouble != 0)) {
        c.game_clear = true; n++;
    }

    it = cJSON_GetObjectItemCaseSensitive(root, "img_mode");
    if (cJSON_IsString(it) && it->valuestring) {
        if (strcmp(it->valuestring, "fullscreen") == 0) { d->img_mode = IMG_MODE_FULLSCREEN; c.img_mode = true; n++; }
        else if (strcmp(it->valuestring, "avatar") == 0) { d->img_mode = IMG_MODE_AVATAR; c.img_mode = true; n++; }
    }

    cJSON_Delete(root);
    *target = next;
    if (chg) *chg = c;
    return n;
}
