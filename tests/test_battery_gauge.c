// tests/test_battery_gauge.c
// battery_gauge 的 host 测试：OCV 查表（含 TODO 数据点修正）、迟滞、
// 中值滤波与坏读数剔除。编译运行方式见 tools/validate.sh（run_static_checks）。
#include <assert.h>
#include "battery_gauge.h"

int main(void)
{
    // ---- OCV 查表：关键校准点 ----
    assert(battery_gauge_mv_to_soc(4200) == 100);   // 满充表顶
    assert(battery_gauge_mv_to_soc(4300) == 100);   // 高于表顶钳满
    assert(battery_gauge_mv_to_soc(3920) == 68);    // TODO 数据点：旧映射 73% → 68%（目标 60-70）
    assert(battery_gauge_mv_to_soc(3730) == 20);    // 低电量红线对齐
    assert(battery_gauge_mv_to_soc(3500) == 0);     // 截止电压
    assert(battery_gauge_mv_to_soc(3400) == 0);     // 截止以下钳 0（防深放虚高）
    assert(battery_gauge_mv_to_soc(3000) == 0);
    assert(battery_gauge_mv_to_soc(-1) == 0);       // 非法输入不越界

    // ---- OCV 查表：全程单调不减 ----
    int prev = 0;
    for (int mv = 3300; mv <= 4250; mv += 5) {
        int soc = battery_gauge_mv_to_soc(mv);
        assert(soc >= prev);
        assert(soc >= 0 && soc <= 100);
        prev = soc;
    }

    // ---- 显示迟滞 ----
    assert(battery_gauge_hysteresis(-1, 65) == 65);  // 首次直通
    assert(battery_gauge_hysteresis(65, 67) == 65);  // 差 2%：保持
    assert(battery_gauge_hysteresis(65, 68) == 68);  // 差 3%：放行
    assert(battery_gauge_hysteresis(65, 62) == 62);  // 反向差 3%：同样放行
    assert(battery_gauge_hysteresis(65, 63) == 65);  // 反向差 2%：保持
    assert(battery_gauge_hysteresis(21, 19) == 19);  // 跌破 20% 立即放行（红警及时）
    assert(battery_gauge_hysteresis(19, 21) == 21);  // 回升跨 20% 立即放行（解除及时）
    assert(battery_gauge_hysteresis(65, -1) == 65);  // 无新值保持

    // ---- feed：滤波 + 迟滞 + 坏读数剔除（串联行为） ----
    battery_gauge_t g;
    battery_gauge_init(&g);
    assert(g.shown_soc == -1);

    assert(battery_gauge_feed(&g, -1) == -1);        // 无有效样本
    assert(battery_gauge_feed(&g, 3920) == 68);      // 单样本直通：68%
    assert(battery_gauge_feed(&g, 5000) == 68);      // 越界坏值丢弃，显示不变
    assert(battery_gauge_feed(&g, 3924) == 68);      // 双样本平均 3922 → 68%
    assert(battery_gauge_feed(&g, 3916) == 68);      // 满窗中值 3920 → 68%
    assert(battery_gauge_feed(&g, 3100) == 68);      // 异常低压进窗，被中值剔除 + 迟滞保持

    // ---- feed：真实放电序列（电压缓降，SOC 跟随但不逐格抖动） ----
    battery_gauge_init(&g);
    int last = -1;
    for (int mv = 4150; mv >= 3700; mv -= 10) {
        int soc = battery_gauge_feed(&g, mv);
        assert(soc >= 0 && soc <= 100);
        if (last >= 0) assert(soc <= last);          // 放电方向显示不回升
        last = soc;
    }
    assert(last >= 10 && last <= 20);                // 3700mV 一带应落在低电区（表值约 16-17%）

    return 0;
}
