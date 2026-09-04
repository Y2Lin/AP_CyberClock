// main/battery_gauge.h
// 电池电量校准：电压→SOC 重映射 + 电压中值滤波 + 显示迟滞。
//
// 背景（v10.4.3，见 docs/TODO.md 电池项）：BSP 的 CW2017 电量计初始化时
// 未写入实际电芯的 OCV profile，芯片跑默认通用 Li-Poly 曲线，读数系统性
// 偏高（实测 3.92V → 73%，典型电芯 OCV 约 60-70%）。本模块不再信任芯片
// SOC 寄存器，改为读 VCELL 电压后由固件自行换算百分比，并叠加两级稳定
// 措施：三点中值滤波（剔除 I2C 偶发坏值）与显示迟滞（小幅变化不上屏）。
//
// 纯逻辑、零 ESP-IDF/LVGL 依赖，由 tests/test_battery_gauge.c 覆盖。
#ifndef BATTERY_GAUGE_H
#define BATTERY_GAUGE_H

// 校准器状态。全部字段由本模块维护，调用方只需持有并透传。
typedef struct {
    int window[3];   // 最近 3 次有效电压采样（mV），循环覆盖；窗口满后始终为最新 3 个
    int count;       // 已积累的有效采样总数（只增；取模访问 window）
    int shown_soc;   // 迟滞后的显示 SOC（0-100）；-1 = 尚无任何有效读数
} battery_gauge_t;

// 复位到初始状态（无样本、shown_soc = -1）。
void battery_gauge_init(battery_gauge_t *g);

// 喂入一次电压采样（mV）。有效窗口 [3000, 4500]，越界值视为坏读数丢弃。
// 返回当前应显示的 SOC（0-100）；尚无任何有效采样时返回 -1。
int battery_gauge_feed(battery_gauge_t *g, int mv);

// OCV 分段线性查表：静置电压（mV）→ SOC（0-100）。
// 表为典型 4.2V Li-Po 放电曲线，拿到实际电芯规格书或放电标定后应替换
// 校准点（见 docs/TODO.md）。无滤波，纯函数。
int battery_gauge_mv_to_soc(int mv);

// 显示迟滞：变化不足 3 个百分点时保持旧值；首次（shown < 0）与跨越 20%
// 低电量阈值（红线告警依据）两种情况立即放行，保证告警及时。
// shown/fresh 任一为负时直接透传另一侧。
int battery_gauge_hysteresis(int shown, int fresh);

#endif // BATTERY_GAUGE_H
