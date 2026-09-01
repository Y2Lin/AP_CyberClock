// main/cyber_clock.c —— 赛博朋克风格时钟 + BLE 时间同步（v3 方案C布局）
//
// v3 方案C布局（240x320 竖屏）：
//   - 顶部状态栏：蓝牙图标 + 同步状态 + 电池图标
//   - 中上部：大号 HH:MM 偏左，右侧小号 SS 秒数 + 垂直发光进度柱
//   - 中部：日期 + 星期
//   - 中下部：同步状态文字 + 时区
//   - 底部：电池电量条 + 百分比 + 电压
//   - 装饰：四角 L 形角标、侧边刻度线、简洁电路线条
//   - 去掉所有无意义参数文字，只保留有意义的信息
//
// 交互：
//   UP   短按：切换配色主题
//   DOWN 短按：切换显示模式（精简/完整）
//   OK   短按：手动开关 BLE 广播
//   OK   长按：返回菜单
//
// BLE 功耗策略（v6）：
//   - 进入时钟页时，仅当"时间不可信"（本次开机未同步过）才自动开启广播
//   - 同步成功（无论 BLE 还是 USB 写入）5 秒后自动关闭广播与连接（省电）；
//     延迟 5 秒是为了让对端收完最终状态通知、并留出回读时间
//   - OK 短按随时手动开/关，手动开启后不会被打断，直到下一次新的同步事件
//   - 顶部状态栏 BT 指示当前蓝牙状态（ADV=广播中 / LINK=已连接 / OFF=已关闭）
#include "cyber_clock.h"
#include "time_manager.h"
#include "ble_time_sync.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cyber_clk";

// ============================================================================
// 配色主题
// ============================================================================
typedef struct {
    uint32_t bg;
    uint32_t primary;     // 主色（时间数字、主装饰）
    uint32_t secondary;   // 次色（秒数、强调装饰）
    uint32_t text_dim;    // 暗色文字（日期、状态）
    uint32_t warn;        // 警告色（未同步/低电量）
    uint32_t battery_ok;  // 电池正常色
    uint32_t hud_line;    // HUD 装饰线
} cyber_theme_t;

static const cyber_theme_t THEMES[] = {
    { // 0: 蓝紫双色霓虹（方案C 默认）
        .bg = 0x0A0A1A,
        .primary = 0x00FFFF,
        .secondary = 0xFF00FF,
        .text_dim = 0x6688AA,
        .warn = 0xFF3333,
        .battery_ok = 0x00FF88,
        .hud_line = 0x004466,
    },
    { // 1: 青绿单色
        .bg = 0x040808,
        .primary = 0x00FFCC,
        .secondary = 0x00AA88,
        .text_dim = 0x448877,
        .warn = 0xFF6644,
        .battery_ok = 0x44FFAA,
        .hud_line = 0x003322,
    },
    { // 2: 橙红暖色
        .bg = 0x120804,
        .primary = 0xFF8800,
        .secondary = 0xFF2244,
        .text_dim = 0xAA6644,
        .warn = 0xFFFF00,
        .battery_ok = 0xFFAA00,
        .hud_line = 0x442200,
    },
    { // 3: 矩阵绿
        .bg = 0x040804,
        .primary = 0x00FF44,
        .secondary = 0x88FF00,
        .text_dim = 0x448844,
        .warn = 0xFF4400,
        .battery_ok = 0x00FF44,
        .hud_line = 0x003311,
    },
};
#define THEME_COUNT (sizeof(THEMES) / sizeof(THEMES[0]))

// ============================================================================
// 显示模式
// ============================================================================
typedef enum {
    MODE_FULL = 0,    // 完整信息
    MODE_MINIMAL,     // 精简（只显示时间+日期+电池）
    MODE_COUNT,
} display_mode_t;

// ============================================================================
// 全局状态
// ============================================================================
static lv_obj_t *s_scr;
static lv_obj_t *s_time_label;       // HH:MM 大号
static lv_obj_t *s_sec_label;        // SS 小号
static lv_obj_t *s_date_label;       // 日期
static lv_obj_t *s_weekday_label;    // 星期
static lv_obj_t *s_status_label;     // 同步状态+时区
static lv_obj_t *s_uptime_label;     // 运行时间
static lv_obj_t *s_sync_icon;        // 同步状态图标
static lv_obj_t *s_sync_label;       // 同步文字
static lv_obj_t *s_batt_icon_top;    // 顶部电池图标
static lv_obj_t *s_batt_frame;       // 底部电池外框
static lv_obj_t *s_batt_fill;        // 底部电池填充
static lv_obj_t *s_batt_cap;         // 电池正极
static lv_obj_t *s_batt_pct;         // 电池百分比
static lv_obj_t *s_batt_volt;        // 电池电压
static lv_obj_t *s_corners[4];       // 四角装饰
static lv_obj_t *s_divider1;         // 分割线1
static lv_obj_t *s_divider2;         // 分割线2

static lv_timer_t *s_timer;
static int s_theme_idx = 0;
static display_mode_t s_mode = MODE_FULL;
static int s_last_sec = -1;
static int s_batt_soc = -1;
static int s_batt_mv = 0;
static int64_t s_enter_time;
static bool s_ble_started = false;
static volatile bool s_sync_pending;   // BLE 同步事件标志（跨任务传递）
static lv_obj_t *s_bt_label;           // 顶部蓝牙状态文字
static uint32_t s_last_sync_count;     // 上次看到的同步计数（新同步事件边沿检测）
static int64_t  s_ble_off_deadline_ms; // >0：到点自动关闭 BLE
static int      s_bt_disp_state = -1;  // 状态栏 BT 显示缓存（-1 强制刷新）

// 同步成功后延迟关 BLE：给对端留出收最终 FFC2 通知 / 回读状态的时间
#define BLE_AUTO_OFF_DELAY_MS   5000
// "时间可信"判定窗口：本次开机内 1 小时内同步过即可信（重启后单调钟归零，
// last_sync_ms 反而变大 → elapsed 为负 → 不可信 → 自动开广播等待重新同步）
#define BLE_RELIABLE_WINDOW_S   3600

// 星期名称
static const char *WEEKDAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// ============================================================================
// 工具函数
// ============================================================================
static lv_obj_t *make_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static void set_text_color(lv_obj_t *label, uint32_t color)
{
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

// ============================================================================
// 电池组件
// ============================================================================
static void battery_update(void)
{
    if (s_batt_soc < 0) {
        // 首次读取
        int soc = bsp_battery_soc();
        int mv = bsp_battery_mv();
        if (soc >= 0) {
            s_batt_soc = soc;
            s_batt_mv = (mv >= 0) ? mv : 3700;
        } else {
            s_batt_soc = 50;  // 默认值
            s_batt_mv = 3700;
        }
    }

    const cyber_theme_t *t = &THEMES[s_theme_idx];
    int soc = s_batt_soc;
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;

    // 电池填充宽度：外框内 100px，映射 0-100%
    int fill_w = (soc * 100) / 100;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > 100) fill_w = 100;
    lv_obj_set_width(s_batt_fill, fill_w);

    // 颜色：低电量红色，正常用主题色
    uint32_t batt_color = (soc < 20) ? t->warn : t->battery_ok;
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(batt_color), 0);
    lv_obj_set_style_bg_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_batt_cap, lv_color_hex(t->primary), 0);

    // 百分比文字
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", soc);
    lv_label_set_text(s_batt_pct, buf);
    set_text_color(s_batt_pct, batt_color);

    // 电压文字
    snprintf(buf, sizeof(buf), "%d.%02dV", s_batt_mv / 1000, (s_batt_mv % 1000) / 10);
    lv_label_set_text(s_batt_volt, buf);
    set_text_color(s_batt_volt, t->text_dim);

    // 顶部电池图标颜色
    lv_obj_set_style_bg_color(s_batt_icon_top, lv_color_hex(batt_color), 0);
}

// 每 5 秒刷新一次电池读数（避免频繁 I2C 阻塞）
static void battery_refresh(void)
{
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    if (soc >= 0) {
        s_batt_soc = soc;
        s_batt_mv = (mv >= 0) ? mv : s_batt_mv;
    }
    battery_update();
}

// ============================================================================
// UI 构建
// ============================================================================
static void build_ui(void)
{
    const cyber_theme_t *t = &THEMES[s_theme_idx];

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(t->bg), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // ---- 四角 L 形装饰角标 ----
    int corner_size = 12;
    int corner_w = 2;
    // 左上
    s_corners[0] = make_rect(s_scr, 6, 6, corner_size, corner_w, t->hud_line);
    make_rect(s_scr, 6, 6, corner_w, corner_size, t->hud_line);
    // 右上
    s_corners[1] = make_rect(s_scr, 240-6-corner_size, 6, corner_size, corner_w, t->hud_line);
    make_rect(s_scr, 240-6-corner_w, 6, corner_w, corner_size, t->hud_line);
    // 左下
    s_corners[2] = make_rect(s_scr, 6, 320-6-corner_w, corner_size, corner_w, t->hud_line);
    make_rect(s_scr, 6, 320-6-corner_size, corner_w, corner_size, t->hud_line);
    // 右下
    s_corners[3] = make_rect(s_scr, 240-6-corner_size, 320-6-corner_w, corner_size, corner_w, t->hud_line);
    make_rect(s_scr, 240-6-corner_w, 320-6-corner_size, corner_w, corner_size, t->hud_line);

    // ---- 顶部状态栏 (y=8-28) ----
    // 蓝牙文字标识
    // 同步状态图标（圆形，左移因为去掉了BT）
    s_sync_icon = lv_obj_create(s_scr);
    lv_obj_set_pos(s_sync_icon, 20, 12);
    lv_obj_set_size(s_sync_icon, 10, 10);
    lv_obj_set_style_bg_color(s_sync_icon, lv_color_hex(t->warn), 0);
    lv_obj_set_style_bg_opa(s_sync_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sync_icon, 0, 0);
    lv_obj_set_style_radius(s_sync_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_sync_icon, 0, 0);
    // 同步文字
    s_sync_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_sync_label, 36, 10);
    lv_label_set_text(s_sync_label, "SYNC");
    lv_obj_set_style_text_font(s_sync_label, &lv_font_montserrat_14, 0);
    set_text_color(s_sync_label, t->text_dim);
    // 顶部电池图标（右移，因为去掉了百分比文字）
    s_batt_icon_top = make_rect(s_scr, 210, 13, 14, 7, t->battery_ok);
    make_rect(s_scr, 224, 15, 2, 3, t->battery_ok);
    // 蓝牙状态文字（电池图标左侧）
    s_bt_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_bt_label, 150, 10);
    lv_label_set_text(s_bt_label, "BT OFF");
    lv_obj_set_style_text_font(s_bt_label, &lv_font_montserrat_14, 0);
    set_text_color(s_bt_label, t->text_dim);

    // ---- 时间区域 (y=50-170) ----
    // 大号 HH:MM 偏左
    s_time_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_time_label, 16, 60);
    lv_label_set_text(s_time_label, "00:00");
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    set_text_color(s_time_label, t->primary);

    // 右侧秒数 SS（去掉垂直进度柱后右移）
    s_sec_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_sec_label, 180, 72);
    lv_label_set_text(s_sec_label, "00");
    lv_obj_set_style_text_font(s_sec_label, &lv_font_montserrat_24, 0);
    set_text_color(s_sec_label, t->secondary);

    // ---- 分割线 1 (y=180) ----
    s_divider1 = make_rect(s_scr, 20, 182, 200, 1, t->hud_line);

    // ---- 日期星期 (y=190-220) ----
    s_date_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_date_label, 20, 192);
    lv_label_set_text(s_date_label, "2024-01-01");
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_16, 0);
    set_text_color(s_date_label, t->text_dim);

    s_weekday_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_weekday_label, 175, 192);
    lv_label_set_text(s_weekday_label, "MON");
    lv_obj_set_style_text_font(s_weekday_label, &lv_font_montserrat_16, 0);
    set_text_color(s_weekday_label, t->primary);

    // ---- 同步状态+时区 (y=225-245) ----
    s_status_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_status_label, 20, 228);
    lv_label_set_text(s_status_label, "NOT SYNCED  UTC+0");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    set_text_color(s_status_label, t->warn);

    // 运行时间（右侧）
    s_uptime_label = lv_label_create(s_scr);
    lv_obj_set_pos(s_uptime_label, 165, 228);
    lv_label_set_text(s_uptime_label, "UP 00:00");
    lv_obj_set_style_text_font(s_uptime_label, &lv_font_montserrat_14, 0);
    set_text_color(s_uptime_label, t->text_dim);

    // ---- 分割线 2 (y=255) ----
    s_divider2 = make_rect(s_scr, 20, 258, 200, 1, t->hud_line);

    // ---- 底部电池区域 (y=268-310) ----
    // 电池外框
    s_batt_frame = make_rect(s_scr, 20, 272, 104, 20, t->primary);
    lv_obj_set_style_bg_opa(s_batt_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_batt_frame, 2, 0);
    lv_obj_set_style_border_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_radius(s_batt_frame, 2, 0);
    // 电池正极
    s_batt_cap = make_rect(s_scr, 124, 278, 4, 8, t->primary);
    // 电池填充（在内框内）
    s_batt_fill = make_rect(s_scr, 22, 274, 0, 16, t->battery_ok);
    // 百分比
    s_batt_pct = lv_label_create(s_scr);
    lv_obj_set_pos(s_batt_pct, 140, 274);
    lv_label_set_text(s_batt_pct, "--%");
    lv_obj_set_style_text_font(s_batt_pct, &lv_font_montserrat_16, 0);
    set_text_color(s_batt_pct, t->battery_ok);
    // 电压
    s_batt_volt = lv_label_create(s_scr);
    lv_obj_set_pos(s_batt_volt, 140, 292);
    lv_label_set_text(s_batt_volt, "--.--V");
    lv_obj_set_style_text_font(s_batt_volt, &lv_font_montserrat_12, 0);
    set_text_color(s_batt_volt, t->text_dim);

    // 侧边刻度线装饰（左侧）
    for (int i = 0; i < 8; i++) {
        make_rect(s_scr, 2, 50 + i * 30, 2, 12, t->hud_line);
    }
    // 侧边刻度线装饰（右侧）
    for (int i = 0; i < 8; i++) {
        make_rect(s_scr, 236, 50 + i * 30, 2, 12, t->hud_line);
    }
}

// ============================================================================
// 蓝牙状态栏指示：OFF / ADV / LINK（带缓存，状态不变不重绘）
// ============================================================================
static void bt_status_update(void)
{
    if (!s_bt_label) return;
    const cyber_theme_t *t = &THEMES[s_theme_idx];
    int st;
    const char *text;
    uint32_t color;
    if (!s_ble_started) {
        st = 0; text = "BT OFF"; color = t->text_dim;
    } else {
        ble_ts_state_t bs = ble_time_sync_get_state();
        if (bs == BLE_TS_CONNECTED || bs == BLE_TS_SYNCED) {
            st = 2; text = "BT LINK"; color = t->battery_ok;
        } else {
            st = 1; text = "BT ADV"; color = t->primary;
        }
    }
    if (st != s_bt_disp_state) {
        s_bt_disp_state = st;
        lv_label_set_text(s_bt_label, text);
        set_text_color(s_bt_label, color);
    }
}

// ============================================================================
// 主题应用
// ============================================================================
static void apply_theme(void)
{
    const cyber_theme_t *t = &THEMES[s_theme_idx];
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(t->bg), 0);
    set_text_color(s_time_label, t->primary);
    set_text_color(s_sec_label, t->secondary);
    set_text_color(s_weekday_label, t->primary);
    set_text_color(s_date_label, t->text_dim);
    set_text_color(s_sync_label, t->text_dim);
    set_text_color(s_uptime_label, t->text_dim);
    lv_obj_set_style_border_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_batt_cap, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_divider1, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_divider2, lv_color_hex(t->hud_line), 0);
    s_bt_disp_state = -1;   // 强制按新主题重绘 BT 状态
    bt_status_update();
    battery_update();
}

// ============================================================================
// 时间显示更新
// ============================================================================
static void update_time_display(void)
{
    time_t now = time_manager_get_unix_utc();
    time_manager_state_t st = time_manager_get_state();
    time_t local = now + st.tz_offset;
    struct tm tm;
    localtime_r(&local, &tm);

    // HH:MM
    // 48 字节：容纳各格式理论最大值（含 "NOT SYNCED UTC%+d" 23 字节、
    // 日期最坏 35 字节），避免 -Werror=format-truncation 与实际截断。
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_time_label, buf);

    // SS
    snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
    lv_label_set_text(s_sec_label, buf);

    // 日期
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    lv_label_set_text(s_date_label, buf);

    // 星期
    lv_label_set_text(s_weekday_label, WEEKDAYS[tm.tm_wday]);

    // 同步状态+时区
    const cyber_theme_t *t = &THEMES[s_theme_idx];
    int tz_hours = st.tz_offset / 3600;
    if (st.synced) {
        snprintf(buf, sizeof(buf), "SYNCED  UTC%+d", tz_hours);
        lv_label_set_text(s_status_label, buf);
        set_text_color(s_status_label, t->primary);
        lv_obj_set_style_bg_color(s_sync_icon, lv_color_hex(t->battery_ok), 0);
    } else {
        snprintf(buf, sizeof(buf), "NOT SYNCED  UTC%+d", tz_hours);
        lv_label_set_text(s_status_label, buf);
        set_text_color(s_status_label, t->warn);
        lv_obj_set_style_bg_color(s_sync_icon, lv_color_hex(t->warn), 0);
    }

    // 运行时间（UP HH:MM）
    int64_t uptime_us = esp_timer_get_time() - s_enter_time;
    int uptime_sec = (int)(uptime_us / 1000000);
    int up_h = uptime_sec / 3600;
    int up_m = (uptime_sec % 3600) / 60;
    snprintf(buf, sizeof(buf), "UP %02d:%02d", up_h, up_m);
    lv_label_set_text(s_uptime_label, buf);
}

// ============================================================================
// 定时器回调（每秒触发）
// ============================================================================
static void tick(lv_timer_t *timer)
{
    (void)timer;
    // BLE 同步回调运行在 NimBLE host 任务：LVGL 非线程安全且彼时未持
    // bsp_lvgl 锁，直接调 update_time_display() 会与 LVGL 任务的渲染
    // 并发操作 LVGL 堆，导致死锁/堆损坏（表现为写入成功后整机卡死）。
    // 因此回调只置标志，在这里（LVGL 定时器上下文，已持锁）完成刷新。
    if (s_sync_pending) {
        s_sync_pending = false;
        s_last_sec = -1;   // 强制立即刷新显示
    }

    time_t now = time_manager_get_unix_utc();
    time_manager_state_t st = time_manager_get_state();
    time_t local = now + st.tz_offset;
    struct tm tm;
    localtime_r(&local, &tm);

    // ---- BLE 功耗策略（v6，全部在 LVGL 上下文执行，跨任务安全）----
    // 1) 新同步事件（BLE / USB 写时间都会让 sync_count 自增）：
    //    安排 5 秒后自动关蓝牙，给对端留出收最终通知、回读状态的时间。
    if (st.sync_count != s_last_sync_count) {
        s_last_sync_count = st.sync_count;
        if (s_ble_started && s_ble_off_deadline_ms == 0) {
            s_ble_off_deadline_ms = esp_timer_get_time() / 1000 + BLE_AUTO_OFF_DELAY_MS;
            ESP_LOGI(TAG, "时间同步成功，%d 秒后自动关闭 BLE 省电",
                     BLE_AUTO_OFF_DELAY_MS / 1000);
        }
    }
    // 2) 到点自动关：断开连接、停广播、释放 NimBLE（stop 为同步操作）。
    if (s_ble_off_deadline_ms > 0 &&
        (esp_timer_get_time() / 1000) >= s_ble_off_deadline_ms) {
        s_ble_off_deadline_ms = 0;
        if (s_ble_started) {
            ble_time_sync_stop();
            s_ble_started = false;
            ESP_LOGI(TAG, "同步完成，BLE 已自动关闭（OK 键可重新开启）");
        }
    }
    // 3) 状态栏 BT 指示
    bt_status_update();

    if (tm.tm_sec != s_last_sec) {
        s_last_sec = tm.tm_sec;
        update_time_display();
        // 每 5 秒刷新电池
        if (tm.tm_sec % 5 == 0) {
            battery_refresh();
        }
    }
}

// ============================================================================
// BLE 同步回调
// ============================================================================
static void on_ble_sync(void)
{
    // 运行在 NimBLE host 任务中，禁止直接调用任何 LVGL API（见 tick() 内注释）。
    ESP_LOGI(TAG, "BLE 时间同步完成");
    s_sync_pending = true;
}

// ============================================================================
// 进入/退出
// ============================================================================
void demo_cyber_clock_enter(void)
{
    ESP_LOGI(TAG, "进入赛博朋克时钟");
    s_enter_time = esp_timer_get_time();
    s_last_sec = -1;
    s_batt_soc = -1;
    s_theme_idx = 0;
    s_mode = MODE_FULL;

    build_ui();
    lv_scr_load(s_scr);

    // 初始化电池读数
    battery_refresh();
    update_time_display();

    // 启动 BLE 时间同步服务
    esp_err_t err = ble_time_sync_start();
    if (err == ESP_OK) {
        s_ble_started = true;
        ble_time_sync_set_sync_callback(on_ble_sync);
        ESP_LOGI(TAG, "BLE 时间同步服务已启动");
    } else {
        ESP_LOGE(TAG, "BLE 启动失败: %s", esp_err_to_name(err));
    }

    // 启动 LVGL 定时器（100ms 间隔，内部判断秒数变化）
    s_timer = lv_timer_create(tick, 100, NULL);
}

void demo_cyber_clock_exit(void)
{
    ESP_LOGI(TAG, "退出赛博朋克时钟");
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_ble_started) {
        ble_time_sync_stop();
        s_ble_started = false;
    }
    s_ble_off_deadline_ms = 0;
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
    s_time_label = NULL;
    s_sec_label = NULL;
    s_date_label = NULL;
    s_weekday_label = NULL;
    s_status_label = NULL;
    s_uptime_label = NULL;
    s_sync_icon = NULL;
    s_sync_label = NULL;
    s_bt_label = NULL;
    s_batt_icon_top = NULL;
    s_batt_frame = NULL;
    s_batt_fill = NULL;
    s_batt_cap = NULL;
    s_batt_pct = NULL;
    s_batt_volt = NULL;
    s_divider1 = NULL;
    s_divider2 = NULL;
    memset(s_corners, 0, sizeof(s_corners));
}

// ============================================================================
// 按键处理
// ============================================================================
void demo_cyber_clock_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    switch (btn) {
    case BSP_BTN_UP:
        // 切换配色主题
        s_theme_idx = (s_theme_idx + 1) % THEME_COUNT;
        ESP_LOGI(TAG, "切换主题: %d", s_theme_idx);
        apply_theme();
        update_time_display();
        break;

    case BSP_BTN_DOWN:
        // 切换显示模式
        s_mode = (s_mode + 1) % MODE_COUNT;
        ESP_LOGI(TAG, "切换模式: %d", s_mode);
        if (s_mode == MODE_MINIMAL) {
            // 精简模式：隐藏部分信息
            lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_batt_volt, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_divider2, LV_OBJ_FLAG_HIDDEN);
        } else {
            // 完整模式
            lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_batt_volt, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_divider2, LV_OBJ_FLAG_HIDDEN);
        }
        break;

    case BSP_BTN_OK:
        // 短按：手动开关 BLE（默认策略下同步后蓝牙已关，此键随时开启再同步）
        if (s_ble_started) {
            ble_time_sync_stop();   // 同步操作：等 NimBLE host 真正退出
            s_ble_started = false;
            s_ble_off_deadline_ms = 0;
            ESP_LOGI(TAG, "BLE 已手动关闭");
        } else {
            esp_err_t err = ble_time_sync_start();
            if (err == ESP_OK) {
                s_ble_started = true;
                s_ble_off_deadline_ms = 0;
                ESP_LOGI(TAG, "BLE 已手动开启（等待连接/同步）");
            } else {
                ESP_LOGE(TAG, "BLE 启动失败: %s", esp_err_to_name(err));
            }
        }
        bt_status_update();
        break;

    default:
        break;
    }
}
