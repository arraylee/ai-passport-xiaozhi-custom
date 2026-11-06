// components/core/services/include/services/battery.h —— 电量纯逻辑(SOC 直接分档 / ADC 曲线及平滑)
#pragma once
#include <stdbool.h>

#define BATTERY_LEVELS   5      // 5 档
#define BATTERY_FULL_MV  4200   // 3.7V 锂电满电电压
#define BATTERY_EMPTY_MV 3000   // 视为 0% 的截止电压

// 颜色分档(供 UI 用):档位 <= CRIT 红,== 中间 WARN 黄,其余正常。
#define BATTERY_LVL_CRIT 1      // <=1 危急(红)
#define BATTERY_LVL_WARN 2      // ==2 低电(黄)

typedef struct {
    int  ema;       // 仅 ADC 路径使用的电压 EMA 累加器(mV)
    bool primed;    // ADC 电压 EMA 是否已有初值
    int  pct;       // 百分比 0..100:CW2017 原始 SOC / ADC 估算值
    int  level;     // 当前档位 1..5:仅 ADC 路径带迟滞
} battery_filter_t;

void battery_init(battery_filter_t *f);

// 虚拟 OCV 曲线:电池电压(mV)→ 0..100 百分比。分段线性,单调。
// ⚠ 仅 ADC 方案(无电量计)时用;CW2017 电量计直接给 SOC,不经此曲线。
int  battery_curve_pct(int mv);

// 【电量计 SOC 方案,CW2017】直接采用百分比并按 20/40/60/80 分档,不做平滑或迟滞。
// raw_pct 不在 0..100(读失败/无效)时维持旧值不变。
int  battery_feed_pct(battery_filter_t *f, int raw_pct);

// 【ADC 电压方案】喂一次原始电压 mV:EMA 防突变 → 曲线 → 迟滞档位。返回平滑百分比。
// raw_mv<0(读失败)时维持旧值不变。
int  battery_feed(battery_filter_t *f, int raw_mv);

int  battery_percent(const battery_filter_t *f);   // 当前有效百分比 0..100
int  battery_level(const battery_filter_t *f);      // 当前档位 1..5
