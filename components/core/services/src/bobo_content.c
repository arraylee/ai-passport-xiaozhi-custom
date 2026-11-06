#include "services/bobo_content.h"

// Original imagined exhibits. These observations do not identify real museum objects.
const char *const bobo_destinations[] = {"纹样小馆", "造物小馆", "自然小馆"};
const char *const bobo_tools[] = {"轻装出发", "速写本", "观察镜"};
const char *const bobo_item_names[] = {"水壶", "小毛巾", "点心", "速写本", "观察镜"};
const bobo_card_text_t bobo_cards[] = {
    {"波浪线", "一高一低，像水面在呼吸。", "博博想把小浪花带回家。"},
    {"螺旋线", "从中心出发，一圈又一圈。", "拐个弯，也有新的风景。"},
    {"重复格", "同样的方格，排出了节奏。", "每一格都装着一点好奇。"},
    {"对称花", "沿中间对折，两边一样吗？", "花瓣里藏着一个小秘密。"},
    {"圆拱", "弯弯的线，连起了两边。", "博博想从小拱门下走过。"},
    {"折扇", "折起是细条，展开像半圆。", "一开一合，风就来了。"},
    {"齿轮", "小齿挨着小齿，一起转动。", "原来转动也需要好搭档。"},
    {"榫扣", "凸起和凹口，刚好抱在一起。", "拼在一起，比一个更稳。"},
    {"叶脉", "细细的线，分出许多小岔路。", "博博在叶子上找到地图。"},
    {"年轮", "一圈套着一圈，慢慢向外。", "成长，也可以安安静静。"},
    {"贝壳纹", "线从小小一点，向外散开。", "把耳朵凑近，想一想海。"},
    {"羽毛纹", "中间一根轴，两旁排着细线。", "小小的羽毛，也很整齐。"},
};
const char *bobo_title(const bobo_state_t *s) {
    unsigned count = bobo_count(s);
    return count >= 12 ? "博博的好搭档" : count >= 6 ? "发现能手" :
           count >= 3 ? "观察新手" : "好奇旅伴";
}
const char *bobo_growth_name(const bobo_state_t *s) {
    return bobo_pet_stage_name(s->active_pet, bobo_growth_stage(s));
}
