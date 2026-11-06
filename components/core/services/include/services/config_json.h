// components/core/services/include/services/config_json.h
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "services/profile_ctl.h"

// 本次 JSON 改动了哪些字段(供上层按需刷新 UI)。level/xp/xp_max 合并为 level。
// time 特殊:不写入 profile(时间是实时值,不持久化),仅通过 time/time_epoch 上报给上层去设系统时钟。
typedef struct {
    bool name, role, subtitle, battery, level, online, token, token_max, sleep_min, img_mode;
    bool avatar_name;
    bool volume;           // "volume":0..100 → 调整 codec 输出音量(持久化)
    bool game_clear;       // "game_clear":true → 清零本地游戏积分(累计/最高);动作,不写 profile
    bool     time;         // 是否带了 "time" 键
    int64_t  time_epoch;   // "time" 值:本地时间的 Unix 秒(见协议;仅 time=true 时有效)
} cfg_changed_t;

// 对 profile 应用一条 JSON(可能无 NUL 结尾)的部分更新。
// 整条输入通过 UTF-8/有限数值/时间范围验证后才提交;失败不修改 profile，chg 清零。
// 有限数值保留截断与 clamp;xp/xp_max 最大 INT_MAX。
// time>0 且 <=253402300799(9999-12-31 23:59:59)时上报，有限非正数保持忽略。
// nickname/name、title/role、intro/subtitle 保留主键优先和兼容别名。
// 返回: >0 = 改动的字段数(notify status 0);
//        0 = 解析成功但无任何已知合法键(notify status 3);
//       -1 = 无效输入、JSON/UTF-8 非法、非有限数值、time 超限或根不是对象(notify status 2)。
// chg 可为 NULL。
int config_json_apply(profile_data_t *d, const char *json, int len, cfg_changed_t *chg);
