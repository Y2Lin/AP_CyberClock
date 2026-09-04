// main/battery_gauge.c
// 电池电量校准实现（背景与取舍见 battery_gauge.h 头注释）。
#include "battery_gauge.h"

// 典型 4.2V Li-Po OCV 放电曲线（静置电压），按电压降序排列，分段线性插值。
// 关键校准点：3.92V ≈ 68%（修正 TODO 记录的 3.92V→73% 偏高数据点，目标
// 区间 60-70%）；3730mV 对齐 20% 低电量红线（<20% 表盘转警告色）。
// TODO：拿到实际电芯规格书或完整放电标定后替换本表（docs/TODO.md 电池项）。
// 注意：负载电压低于静置电压，本表未加负载偏置——本机负载小且恒定，
// 首版不加；若实测整体偏低再按压降补固定偏置。
static const struct {
    int mv;   // 段上边界电压（mV）
    int soc;  // 对应 SOC（%）
} OCV_TABLE[] = {
    { 4200, 100 },
    { 4120,  90 },
    { 4050,  85 },
    { 3990,  78 },
    { 3950,  72 },
    { 3900,  64 },   // 3.92V 插值 ≈ 68
    { 3860,  55 },
    { 3830,  45 },
    { 3800,  35 },
    { 3770,  28 },
    { 3730,  20 },   // 低电量红线对齐
    { 3680,  12 },
    { 3600,   5 },
    { 3500,   0 },
};
#define OCV_N (sizeof(OCV_TABLE) / sizeof(OCV_TABLE[0]))

// 电压采样有效窗口：低于 3000mV（截止电压以下，多为读坏值）或高于
// 4500mV（超过满充，同为坏值）一律丢弃，不进中值窗口。
#define MV_MIN_VALID 3000
#define MV_MAX_VALID 4500

// 迟滞阈值：|fresh - shown| >= 3 才放行上屏。
#define HYST_STEP 3

// 低电量阈值：跨过 20% 边界（任一方向）立即放行，保证红警/解除及时。
#define SOC_LOW 20

void battery_gauge_init(battery_gauge_t *g)
{
    g->window[0] = g->window[1] = g->window[2] = 0;
    g->count = 0;
    g->shown_soc = -1;
}

int battery_gauge_mv_to_soc(int mv)
{
    // 低于表底（3500mV 截止）一律 0%，防深放区间误读出虚高电量
    if (mv < OCV_TABLE[OCV_N - 1].mv) return 0;
    // 落在相邻两校准点之间：线性插值（整数运算，向下取整）
    for (int i = 0; i < (int)OCV_N - 1; i++) {
        int hi_mv = OCV_TABLE[i].mv, lo_mv = OCV_TABLE[i + 1].mv;
        if (mv >= lo_mv && mv <= hi_mv) {
            int span = hi_mv - lo_mv;             // 该段电压宽度（必 > 0）
            int d = hi_mv - mv;                   // 距段上边界的电压距离
            int drop = d * (OCV_TABLE[i].soc - OCV_TABLE[i + 1].soc) / span;
            return OCV_TABLE[i].soc - drop;
        }
    }
    return 100;   // 高于表顶（>4200mV）钳满
}

int battery_gauge_hysteresis(int shown, int fresh)
{
    if (shown < 0) return fresh;                        // 首次读数直通
    if (fresh < 0) return shown;                        // 无新值保持
    if ((shown >= SOC_LOW) != (fresh >= SOC_LOW)) return fresh;  // 跨低电阈值
    if (fresh > shown - HYST_STEP && fresh < shown + HYST_STEP)
        return shown;                                   // 变化 < 3% 不上屏
    return fresh;
}

// 三数取中：排序网络实现，无分支爆炸，host 可测。
static int median3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

int battery_gauge_feed(battery_gauge_t *g, int mv)
{
    // 坏读数只跳过，不清窗口、不改显示
    if (mv >= MV_MIN_VALID && mv <= MV_MAX_VALID) {
        g->window[g->count % 3] = mv;
        g->count++;
    }
    if (g->count == 0) return -1;                 // 尚无任何有效样本

    // 中值滤波：满窗口取中值；不足 3 个时 1 个直用、2 个取平均
    int med;
    if (g->count == 1)      med = g->window[0];
    else if (g->count == 2) med = (g->window[0] + g->window[1]) / 2;
    else                    med = median3(g->window[0], g->window[1], g->window[2]);

    int fresh = battery_gauge_mv_to_soc(med);
    g->shown_soc = battery_gauge_hysteresis(g->shown_soc, fresh);
    return g->shown_soc;
}
