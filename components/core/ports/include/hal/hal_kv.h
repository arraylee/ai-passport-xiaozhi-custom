// components/core/ports/include/hal/hal_kv.h
#pragma once
#include <stdint.h>

typedef struct hal_kv_s hal_kv_t;
enum { HAL_KV_ERROR = -1, HAL_KV_NOT_FOUND = -2, HAL_KV_TOO_LARGE = -3 };
typedef struct {
    // 读:成功返回0;缺键/缓冲过小/IO错误分别返回上述负值。旧调用仍可用 !=0 判断。
    int (*get)(hal_kv_t *self, const char *key, void *buf, int cap, int *out_len);
    // 写: 返回 0 成功,<0 失败。
    int (*set)(hal_kv_t *self, const char *key, const void *data, int len);
    // 删除单个键:键不存在也视为成功。
    int (*erase)(hal_kv_t *self, const char *key);
} hal_kv_api_t;
struct hal_kv_s { const hal_kv_api_t *api; void *impl; };

static inline int hal_kv_get(hal_kv_t *c, const char *key, void *buf, int cap, int *out_len) {
    return c->api->get(c, key, buf, cap, out_len);
}
static inline int hal_kv_set(hal_kv_t *c, const char *key, const void *data, int len) {
    return c->api->set(c, key, data, len);
}
static inline int hal_kv_erase(hal_kv_t *c, const char *key) {
    return c->api->erase(c, key);
}
