// main/cyber_clock.c —— 赛博朋克时钟：四页面状态机（v8 动线重构）
//
// v8 页面动线（240x320 竖屏，三个实体按键）：
//   开机 → PAGE_SYNC（时间同步页：BLE 只在本页开启，USB/蓝牙任一同步成功
//          后 5 秒自动进入表盘；OK 可立即跳过）
//   PAGE_CLOCK（表盘页：纯显示，无蓝牙。UP=主题 DOWN=模式 OK长按=菜单）
//   PAGE_MENU（菜单页：1 表盘 / 2 亮度 / 3 时间同步。UP/DOWN 选择，OK 进入，
//          OK 长按回表盘）
//   PAGE_BRIGHT（亮度页：UP/DOWN 以 10% 步进调整 10..100，NVS 持久化，
//          OK/长按返回菜单）
//
// 设计要点：
//   - 表盘页彻底不再管蓝牙（v6 的"表盘内自动开关广播"策略整体迁移到同步页：
//     进同步页必开广播，离开同步页即停，同步成功后 5 秒自动跳表盘顺带关 BLE）
//   - USB 串口对时是常驻服务，任何页面都可完成同步；非同步页收到同步事件
//     只是时间刷新，不发生页面跳转
//   - LVGL 线程安全：BLE 回调只置 s_sync_pending 标志，所有 UI 更新都在
//     LVGL 定时器上下文（已持 bsp_lvgl 锁）完成
//   - 字体为 montserrat（无中文字形），界面文案一律英文
#include "cyber_clock.h"
#include "time_manager.h"
#include "ble_time_sync.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
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
    { // 0: 蓝紫双色霓虹（默认）
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
// 显示模式（表盘页）
// ============================================================================
typedef enum {
    MODE_FULL = 0,    // 完整信息
    MODE_MINIMAL,     // 精简（只显示时间+日期+电池）
    MODE_COUNT,
} display_mode_t;

// ============================================================================
// 页面状态机
// ============================================================================
typedef enum {
    PAGE_SYNC = 0,    // 时间同步页（开机首屏）
    PAGE_CLOCK,       // 表盘页
    PAGE_MENU,        // 菜单页
    PAGE_BRIGHT,      // 亮度页
    PAGE_COUNT_,
} app_page_t;

#define MENU_ITEM_COUNT 3

// 同步成功后自动进表盘的延迟：给对端留出收最终 FFC2 通知 / 回读状态的时间
#define SYNC_AUTO_EXIT_MS   5000

// 亮度范围与步进
#define BRIGHT_MIN   10
#define BRIGHT_MAX   100
#define BRIGHT_STEP  10

// NVS（与 time_manager 同命名空间，NVS 已在其 init 中完成 flash 初始化）
#define NVS_NS_BRIGHT    "cyber_clk"
#define NVS_KEY_BRIGHT   "bright"

// 星期名称
static const char *WEEKDAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// ============================================================================
// 全局状态
// ============================================================================
static lv_obj_t *s_scr;
static lv_obj_t *s_pg[PAGE_COUNT_];       // 四个页面容器（互斥显示）

// ---- 表盘页控件 ----
static lv_obj_t *s_time_label;       // HH:MM 大号
static lv_obj_t *s_sec_label;        // SS 小号
static lv_obj_t *s_date_label;       // 日期
static lv_obj_t *s_weekday_label;    // 星期
static lv_obj_t *s_status_label;     // 同步状态+时区
static lv_obj_t *s_uptime_label;     // 运行时间
static lv_obj_t *s_sync_icon;        // 同步状态图标
static lv_obj_t *s_sync_label;       // 顶部 SYNC 文字
static lv_obj_t *s_batt_icon_top;    // 顶部电池图标
static lv_obj_t *s_batt_frame;       // 底部电池外框
static lv_obj_t *s_batt_fill;        // 底部电池填充
static lv_obj_t *s_batt_cap;         // 电池正极
static lv_obj_t *s_batt_pct;         // 电池百分比
static lv_obj_t *s_batt_volt;        // 电池电压
static lv_obj_t *s_corners[4];       // 四角装饰
static lv_obj_t *s_divider1;         // 分割线1
static lv_obj_t *s_divider2;         // 分割线2

// ---- 同步页控件 ----
static lv_obj_t *s_sp_title;         // 标题 TIME SYNC
static lv_obj_t *s_sp_time;          // HH:MM
static lv_obj_t *s_sp_sec;           // SS
static lv_obj_t *s_sp_date;          // 日期+星期
static lv_obj_t *s_sp_status;        // SYNCED / NOT SYNCED
static lv_obj_t *s_sp_bt;            // 蓝牙状态
static lv_obj_t *s_sp_hint_usb;      // USB 提示
static lv_obj_t *s_sp_hint_ble;      // BLE 提示
static lv_obj_t *s_sp_action;        // 底部动作提示/倒计时
static lv_obj_t *s_sp_div;           // 分割线

// ---- 菜单页控件 ----
static lv_obj_t *s_mp_title;
static lv_obj_t *s_mp_panels[MENU_ITEM_COUNT];
static lv_obj_t *s_mp_rows[MENU_ITEM_COUNT];
static lv_obj_t *s_mp_hint1;
static lv_obj_t *s_mp_hint2;
static lv_obj_t *s_mp_div;

// ---- 亮度页控件 ----
static lv_obj_t *s_bp_title;
static lv_obj_t *s_bp_pct;           // 百分比大字
static lv_obj_t *s_bp_frame;         // 进度条外框
static lv_obj_t *s_bp_fill;          // 进度条填充
static lv_obj_t *s_bp_hint1;
static lv_obj_t *s_bp_hint2;
static lv_obj_t *s_bp_div;

static lv_timer_t *s_timer;
static app_page_t s_page = PAGE_COUNT_;   // 当前页面（初始哨兵值）
static int s_menu_sel = 0;                // 菜单选中项
static uint8_t s_brightness = BRIGHT_MAX; // 当前亮度（10..100）
static int s_theme_idx = 0;
static display_mode_t s_mode = MODE_FULL;
static int s_last_sec = -1;
static int s_batt_soc = -1;
static int s_batt_mv = 0;
static int64_t s_enter_time;
static bool s_ble_started = false;
static volatile bool s_sync_pending;      // BLE 同步事件标志（跨任务传递）
static uint32_t s_last_sync_count;        // 上次看到的同步计数（新同步事件边沿检测）
static int64_t  s_auto_exit_ms = 0;       // >0：到点自动离开同步页进表盘

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

// 满屏透明页面容器（不滚动、无边框、无内边距）
static lv_obj_t *make_page_container(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, 240, 320);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// ============================================================================
// 亮度：加载 / 保存 / 应用
// ============================================================================
static void brightness_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRIGHT, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_BRIGHT, &v) == ESP_OK &&
            v >= BRIGHT_MIN && v <= BRIGHT_MAX) {
            s_brightness = v;
        }
        nvs_close(h);
    }
}

static void brightness_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BRIGHT, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_BRIGHT, s_brightness);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void brightness_apply(void)
{
    bsp_display_backlight(s_brightness);
}

// ============================================================================
// 电池组件（表盘页）
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
// 表盘页 UI
// ============================================================================
static void build_clock_page(void)
{
    lv_obj_t *parent = s_pg[PAGE_CLOCK];
    const cyber_theme_t *t = &THEMES[s_theme_idx];

    // ---- 四角 L 形装饰角标 ----
    int corner_size = 12;
    int corner_w = 2;
    // 左上
    s_corners[0] = make_rect(parent, 6, 6, corner_size, corner_w, t->hud_line);
    make_rect(parent, 6, 6, corner_w, corner_size, t->hud_line);
    // 右上
    s_corners[1] = make_rect(parent, 240-6-corner_size, 6, corner_size, corner_w, t->hud_line);
    make_rect(parent, 240-6-corner_w, 6, corner_w, corner_size, t->hud_line);
    // 左下
    s_corners[2] = make_rect(parent, 6, 320-6-corner_w, corner_size, corner_w, t->hud_line);
    make_rect(parent, 6, 320-6-corner_size, corner_w, corner_size, t->hud_line);
    // 右下
    s_corners[3] = make_rect(parent, 240-6-corner_size, 320-6-corner_w, corner_size, corner_w, t->hud_line);
    make_rect(parent, 240-6-corner_w, 320-6-corner_size, corner_w, corner_size, t->hud_line);

    // ---- 顶部状态栏 (y=8-28) ----
    // 同步状态图标
    s_sync_icon = lv_obj_create(parent);
    lv_obj_set_pos(s_sync_icon, 20, 12);
    lv_obj_set_size(s_sync_icon, 10, 10);
    lv_obj_set_style_bg_color(s_sync_icon, lv_color_hex(t->warn), 0);
    lv_obj_set_style_bg_opa(s_sync_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sync_icon, 0, 0);
    lv_obj_set_style_radius(s_sync_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_sync_icon, 0, 0);
    // 同步文字
    s_sync_label = lv_label_create(parent);
    lv_obj_set_pos(s_sync_label, 36, 10);
    lv_label_set_text(s_sync_label, "SYNC");
    lv_obj_set_style_text_font(s_sync_label, &lv_font_montserrat_14, 0);
    set_text_color(s_sync_label, t->text_dim);
    // 顶部电池图标
    s_batt_icon_top = make_rect(parent, 210, 13, 14, 7, t->battery_ok);
    make_rect(parent, 224, 15, 2, 3, t->battery_ok);

    // ---- 时间区域 (y=50-170) ----
    s_time_label = lv_label_create(parent);
    lv_obj_set_pos(s_time_label, 16, 60);
    lv_label_set_text(s_time_label, "00:00");
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    set_text_color(s_time_label, t->primary);

    // 右侧秒数 SS
    s_sec_label = lv_label_create(parent);
    lv_obj_set_pos(s_sec_label, 180, 72);
    lv_label_set_text(s_sec_label, "00");
    lv_obj_set_style_text_font(s_sec_label, &lv_font_montserrat_24, 0);
    set_text_color(s_sec_label, t->secondary);

    // ---- 分割线 1 (y=182) ----
    s_divider1 = make_rect(parent, 20, 182, 200, 1, t->hud_line);

    // ---- 日期星期 (y=190-220) ----
    s_date_label = lv_label_create(parent);
    lv_obj_set_pos(s_date_label, 20, 192);
    lv_label_set_text(s_date_label, "2024-01-01");
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_16, 0);
    set_text_color(s_date_label, t->text_dim);

    s_weekday_label = lv_label_create(parent);
    lv_obj_set_pos(s_weekday_label, 175, 192);
    lv_label_set_text(s_weekday_label, "MON");
    lv_obj_set_style_text_font(s_weekday_label, &lv_font_montserrat_16, 0);
    set_text_color(s_weekday_label, t->primary);

    // ---- 同步状态+时区 (y=225-245) ----
    s_status_label = lv_label_create(parent);
    lv_obj_set_pos(s_status_label, 20, 228);
    lv_label_set_text(s_status_label, "NOT SYNCED  UTC+0");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    set_text_color(s_status_label, t->warn);

    // 运行时间（右侧）
    s_uptime_label = lv_label_create(parent);
    lv_obj_set_pos(s_uptime_label, 165, 228);
    lv_label_set_text(s_uptime_label, "UP 00:00");
    lv_obj_set_style_text_font(s_uptime_label, &lv_font_montserrat_14, 0);
    set_text_color(s_uptime_label, t->text_dim);

    // ---- 分割线 2 (y=258) ----
    s_divider2 = make_rect(parent, 20, 258, 200, 1, t->hud_line);

    // ---- 底部电池区域 (y=268-310) ----
    s_batt_frame = make_rect(parent, 20, 272, 104, 20, t->primary);
    lv_obj_set_style_bg_opa(s_batt_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_batt_frame, 2, 0);
    lv_obj_set_style_border_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_radius(s_batt_frame, 2, 0);
    s_batt_cap = make_rect(parent, 124, 278, 4, 8, t->primary);
    s_batt_fill = make_rect(parent, 22, 274, 0, 16, t->battery_ok);
    s_batt_pct = lv_label_create(parent);
    lv_obj_set_pos(s_batt_pct, 140, 274);
    lv_label_set_text(s_batt_pct, "--%");
    lv_obj_set_style_text_font(s_batt_pct, &lv_font_montserrat_16, 0);
    set_text_color(s_batt_pct, t->battery_ok);
    s_batt_volt = lv_label_create(parent);
    lv_obj_set_pos(s_batt_volt, 140, 292);
    lv_label_set_text(s_batt_volt, "--.--V");
    lv_obj_set_style_text_font(s_batt_volt, &lv_font_montserrat_12, 0);
    set_text_color(s_batt_volt, t->text_dim);

    // 侧边刻度线装饰（左右）
    for (int i = 0; i < 8; i++) {
        make_rect(parent, 2, 50 + i * 30, 2, 12, t->hud_line);
        make_rect(parent, 236, 50 + i * 30, 2, 12, t->hud_line);
    }
}

// ============================================================================
// 时间同步页 UI
// ============================================================================
static void build_sync_page(void)
{
    lv_obj_t *parent = s_pg[PAGE_SYNC];
    const cyber_theme_t *t = &THEMES[s_theme_idx];

    // 标题
    s_sp_title = lv_label_create(parent);
    lv_obj_align(s_sp_title, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(s_sp_title, "TIME SYNC");
    lv_obj_set_style_text_font(s_sp_title, &lv_font_montserrat_24, 0);
    set_text_color(s_sp_title, t->primary);

    // 标题下分割线
    s_sp_div = make_rect(parent, 20, 54, 200, 1, t->hud_line);

    // 大号时间 HH:MM + 右侧秒
    s_sp_time = lv_label_create(parent);
    lv_obj_set_pos(s_sp_time, 28, 84);
    lv_label_set_text(s_sp_time, "00:00");
    lv_obj_set_style_text_font(s_sp_time, &lv_font_montserrat_48, 0);
    set_text_color(s_sp_time, t->primary);

    s_sp_sec = lv_label_create(parent);
    lv_obj_set_pos(s_sp_sec, 184, 100);
    lv_label_set_text(s_sp_sec, "00");
    lv_obj_set_style_text_font(s_sp_sec, &lv_font_montserrat_24, 0);
    set_text_color(s_sp_sec, t->secondary);

    // 日期+星期
    s_sp_date = lv_label_create(parent);
    lv_obj_align(s_sp_date, LV_ALIGN_TOP_MID, 0, 158);
    lv_label_set_text(s_sp_date, "2024-01-01  MON");
    lv_obj_set_style_text_font(s_sp_date, &lv_font_montserrat_16, 0);
    set_text_color(s_sp_date, t->text_dim);

    // 同步状态
    s_sp_status = lv_label_create(parent);
    lv_obj_align(s_sp_status, LV_ALIGN_TOP_MID, 0, 190);
    lv_label_set_text(s_sp_status, "NOT SYNCED");
    lv_obj_set_style_text_font(s_sp_status, &lv_font_montserrat_16, 0);
    set_text_color(s_sp_status, t->warn);

    // 蓝牙状态
    s_sp_bt = lv_label_create(parent);
    lv_obj_align(s_sp_bt, LV_ALIGN_TOP_MID, 0, 214);
    lv_label_set_text(s_sp_bt, "BT: ADVERTISING");
    lv_obj_set_style_text_font(s_sp_bt, &lv_font_montserrat_16, 0);
    set_text_color(s_sp_bt, t->primary);

    // 两种对时方式提示
    s_sp_hint_usb = lv_label_create(parent);
    lv_obj_align(s_sp_hint_usb, LV_ALIGN_TOP_MID, 0, 250);
    lv_label_set_text(s_sp_hint_usb, "USB: PC WEB TOOL");
    lv_obj_set_style_text_font(s_sp_hint_usb, &lv_font_montserrat_14, 0);
    set_text_color(s_sp_hint_usb, t->text_dim);

    s_sp_hint_ble = lv_label_create(parent);
    lv_obj_align(s_sp_hint_ble, LV_ALIGN_TOP_MID, 0, 270);
    lv_label_set_text(s_sp_hint_ble, "BLE: PHONE WEB TOOL");
    lv_obj_set_style_text_font(s_sp_hint_ble, &lv_font_montserrat_14, 0);
    set_text_color(s_sp_hint_ble, t->text_dim);

    // 底部动作提示（同步成功倒计时期间显示 AUTO EXIT Ns）
    s_sp_action = lv_label_create(parent);
    lv_obj_align(s_sp_action, LV_ALIGN_TOP_MID, 0, 294);
    lv_label_set_text(s_sp_action, "OK: SKIP TO CLOCK");
    lv_obj_set_style_text_font(s_sp_action, &lv_font_montserrat_14, 0);
    set_text_color(s_sp_action, t->text_dim);
}

// 同步页内容刷新（每秒 + 状态变化时调用；只在 LVGL 上下文执行）
static void sync_page_update(void)
{
    time_t now = time_manager_get_unix_utc();
    time_manager_state_t st = time_manager_get_state();
    time_t local = now + st.tz_offset;
    struct tm tm;
    localtime_r(&local, &tm);
    const cyber_theme_t *t = &THEMES[s_theme_idx];
    // 40 字节：容纳 "2024-12-31  WED"（15 字节）与状态文案的最大长度
    char buf[40];

    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_sp_time, buf);
    // 未同步时时间文字闪烁（奇数秒半透明）：显示的是 NVS 恢复的旧值，不可信
    lv_obj_set_style_text_opa(s_sp_time,
        (!st.synced && (tm.tm_sec % 2)) ? LV_OPA_60 : LV_OPA_COVER, 0);

    snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
    lv_label_set_text(s_sp_sec, buf);

    snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, WEEKDAYS[tm.tm_wday]);
    lv_label_set_text(s_sp_date, buf);

    // 同步状态
    int tz_hours = st.tz_offset / 3600;
    if (st.synced) {
        snprintf(buf, sizeof(buf), "SYNCED  UTC%+d", tz_hours);
        set_text_color(s_sp_status, t->battery_ok);
    } else {
        snprintf(buf, sizeof(buf), "NOT SYNCED  UTC%+d", tz_hours);
        set_text_color(s_sp_status, t->warn);
    }
    lv_label_set_text(s_sp_status, buf);

    // 蓝牙状态（蓝牙只属于本页：OFF 只会出现在启动失败时）
    if (!s_ble_started) {
        lv_label_set_text(s_sp_bt, "BT: OFF (USB ONLY)");
        set_text_color(s_sp_bt, t->warn);
    } else {
        ble_ts_state_t bs = ble_time_sync_get_state();
        if (bs == BLE_TS_CONNECTED || bs == BLE_TS_SYNCED) {
            lv_label_set_text(s_sp_bt, "BT: LINKED");
            set_text_color(s_sp_bt, t->battery_ok);
        } else {
            lv_label_set_text(s_sp_bt, "BT: ADVERTISING");
            set_text_color(s_sp_bt, t->primary);
        }
    }

    // 底部动作提示
    if (s_auto_exit_ms > 0) {
        int remain = (int)((s_auto_exit_ms - esp_timer_get_time() / 1000 + 999) / 1000);
        if (remain < 0) remain = 0;
        snprintf(buf, sizeof(buf), "AUTO EXIT %ds", remain);
        set_text_color(s_sp_action, t->battery_ok);
    } else if (st.synced) {
        snprintf(buf, sizeof(buf), "OK: ENTER CLOCK");
        set_text_color(s_sp_action, t->text_dim);
    } else {
        snprintf(buf, sizeof(buf), "OK: SKIP TO CLOCK");
        set_text_color(s_sp_action, t->text_dim);
    }
    lv_label_set_text(s_sp_action, buf);
}

// ============================================================================
// 菜单页 UI
// ============================================================================
static const char *MENU_TITLES[MENU_ITEM_COUNT] = {
    "1 CLOCK", "2 BRIGHTNESS", "3 TIME SYNC",
};

static void menu_refresh(void)
{
    const cyber_theme_t *t = &THEMES[s_theme_idx];
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        bool sel = (i == s_menu_sel);
        lv_obj_set_style_border_width(s_mp_panels[i], sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(s_mp_panels[i],
            lv_color_hex(sel ? t->primary : t->hud_line), 0);
        set_text_color(s_mp_rows[i], sel ? t->primary : t->text_dim);
    }
}

static void build_menu_page(void)
{
    lv_obj_t *parent = s_pg[PAGE_MENU];
    const cyber_theme_t *t = &THEMES[s_theme_idx];

    s_mp_title = lv_label_create(parent);
    lv_obj_align(s_mp_title, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(s_mp_title, "MENU");
    lv_obj_set_style_text_font(s_mp_title, &lv_font_montserrat_24, 0);
    set_text_color(s_mp_title, t->primary);

    s_mp_div = make_rect(parent, 20, 54, 200, 1, t->hud_line);

    static const int row_y[MENU_ITEM_COUNT] = { 76, 136, 196 };
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        s_mp_panels[i] = lv_obj_create(parent);
        lv_obj_set_pos(s_mp_panels[i], 20, row_y[i]);
        lv_obj_set_size(s_mp_panels[i], 200, 44);
        lv_obj_set_style_bg_opa(s_mp_panels[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(s_mp_panels[i], 2, 0);
        lv_obj_set_style_pad_all(s_mp_panels[i], 0, 0);
        lv_obj_clear_flag(s_mp_panels[i], LV_OBJ_FLAG_SCROLLABLE);

        s_mp_rows[i] = lv_label_create(s_mp_panels[i]);
        lv_label_set_text(s_mp_rows[i], MENU_TITLES[i]);
        lv_obj_set_style_text_font(s_mp_rows[i], &lv_font_montserrat_16, 0);
        lv_obj_center(s_mp_rows[i]);
    }

    s_mp_hint1 = lv_label_create(parent);
    lv_obj_align(s_mp_hint1, LV_ALIGN_TOP_MID, 0, 260);
    lv_label_set_text(s_mp_hint1, "UP/DOWN: SELECT");
    lv_obj_set_style_text_font(s_mp_hint1, &lv_font_montserrat_14, 0);
    set_text_color(s_mp_hint1, t->text_dim);

    s_mp_hint2 = lv_label_create(parent);
    lv_obj_align(s_mp_hint2, LV_ALIGN_TOP_MID, 0, 280);
    lv_label_set_text(s_mp_hint2, "OK: ENTER   LONG: CLOCK");
    lv_obj_set_style_text_font(s_mp_hint2, &lv_font_montserrat_14, 0);
    set_text_color(s_mp_hint2, t->text_dim);

    menu_refresh();
}

// ============================================================================
// 亮度页 UI
// ============================================================================
static void bright_page_update(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_brightness);
    lv_label_set_text(s_bp_pct, buf);
    // 进度条内宽 176px（外框 x=30 w=180 border 2）
    lv_obj_set_width(s_bp_fill, s_brightness * 176 / 100);
}

static void build_bright_page(void)
{
    lv_obj_t *parent = s_pg[PAGE_BRIGHT];
    const cyber_theme_t *t = &THEMES[s_theme_idx];

    s_bp_title = lv_label_create(parent);
    lv_obj_align(s_bp_title, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(s_bp_title, "BRIGHTNESS");
    lv_obj_set_style_text_font(s_bp_title, &lv_font_montserrat_24, 0);
    set_text_color(s_bp_title, t->primary);

    s_bp_div = make_rect(parent, 20, 54, 200, 1, t->hud_line);

    s_bp_pct = lv_label_create(parent);
    lv_obj_align(s_bp_pct, LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(s_bp_pct, "100%");
    lv_obj_set_style_text_font(s_bp_pct, &lv_font_montserrat_48, 0);
    set_text_color(s_bp_pct, t->primary);

    // 进度条外框 + 填充
    s_bp_frame = make_rect(parent, 30, 196, 180, 20, t->primary);
    lv_obj_set_style_bg_opa(s_bp_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bp_frame, 2, 0);
    lv_obj_set_style_border_color(s_bp_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_radius(s_bp_frame, 2, 0);
    s_bp_fill = make_rect(parent, 32, 198, 176, 16, t->primary);

    s_bp_hint1 = lv_label_create(parent);
    lv_obj_align(s_bp_hint1, LV_ALIGN_TOP_MID, 0, 248);
    lv_label_set_text(s_bp_hint1, "UP/DOWN: ADJUST 10-100");
    lv_obj_set_style_text_font(s_bp_hint1, &lv_font_montserrat_14, 0);
    set_text_color(s_bp_hint1, t->text_dim);

    s_bp_hint2 = lv_label_create(parent);
    lv_obj_align(s_bp_hint2, LV_ALIGN_TOP_MID, 0, 270);
    lv_label_set_text(s_bp_hint2, "OK: BACK (SAVED)");
    lv_obj_set_style_text_font(s_bp_hint2, &lv_font_montserrat_14, 0);
    set_text_color(s_bp_hint2, t->text_dim);
}

// ============================================================================
// 主题应用（四个页面统一换色）
// ============================================================================
static void update_time_display(void);

static void apply_theme(void)
{
    const cyber_theme_t *t = &THEMES[s_theme_idx];
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(t->bg), 0);

    // ---- 表盘页 ----
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

    // ---- 同步页 ----
    set_text_color(s_sp_title, t->primary);
    set_text_color(s_sp_time, t->primary);
    set_text_color(s_sp_sec, t->secondary);
    set_text_color(s_sp_date, t->text_dim);
    set_text_color(s_sp_hint_usb, t->text_dim);
    set_text_color(s_sp_hint_ble, t->text_dim);
    lv_obj_set_style_bg_color(s_sp_div, lv_color_hex(t->hud_line), 0);

    // ---- 菜单页 ----
    set_text_color(s_mp_title, t->primary);
    set_text_color(s_mp_hint1, t->text_dim);
    set_text_color(s_mp_hint2, t->text_dim);
    lv_obj_set_style_bg_color(s_mp_div, lv_color_hex(t->hud_line), 0);
    menu_refresh();

    // ---- 亮度页 ----
    set_text_color(s_bp_title, t->primary);
    set_text_color(s_bp_pct, t->primary);
    set_text_color(s_bp_hint1, t->text_dim);
    set_text_color(s_bp_hint2, t->text_dim);
    lv_obj_set_style_bg_color(s_bp_div, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_border_color(s_bp_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_bp_fill, lv_color_hex(t->primary), 0);

    // 状态相关颜色由各刷新函数按同步/BT 状态重设
    battery_update();
    update_time_display();
    sync_page_update();
}

// ============================================================================
// 表盘页时间显示更新
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

    // 未同步时时间主文字闪烁（奇数秒半透明）：此刻显示的是 NVS 恢复的
    // 粗略旧值，不可信；闪烁提醒用户需要对时（USB 或同步页蓝牙任一方式）。
    lv_obj_set_style_text_opa(s_time_label,
        (!st.synced && (tm.tm_sec % 2)) ? LV_OPA_60 : LV_OPA_COVER, 0);

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
// 页面切换（只在 LVGL 上下文调用：tick / 按键回调均已持 bsp_lvgl 锁）
// ============================================================================
static void page_switch(app_page_t next)
{
    if (next == s_page) return;

    // ---- 退出当前页 ----
    if (s_page == PAGE_SYNC) {
        // 离开同步页即关蓝牙：表盘/菜单/亮度页都不需要 BLE（v8 动线核心）
        if (s_ble_started) {
            ble_time_sync_stop();   // 同步操作：等 NimBLE host 真正退出
            s_ble_started = false;
        }
        s_auto_exit_ms = 0;
    }

    s_page = next;
    for (int i = 0; i < PAGE_COUNT_; i++) {
        if (i == (int)next) lv_obj_clear_flag(s_pg[i], LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(s_pg[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_last_sec = -1;   // 强制新页面立即刷新

    // ---- 进入新页面 ----
    if (next == PAGE_SYNC) {
        // 同步页专属：开启 BLE 广播等待对时（手动从菜单进入也重新开）
        esp_err_t err = ble_time_sync_start();
        if (err == ESP_OK) {
            s_ble_started = true;
            ESP_LOGI(TAG, "进入同步页，BLE 广播已开启");
        } else {
            s_ble_started = false;
            ESP_LOGE(TAG, "BLE 启动失败（可改用 USB 对时）: %s", esp_err_to_name(err));
        }
        sync_page_update();
    } else if (next == PAGE_CLOCK) {
        update_time_display();
    } else if (next == PAGE_BRIGHT) {
        bright_page_update();
    }
    // 菜单页为静态内容，无需刷新
}

// ============================================================================
// 定时器回调（每 100ms 触发）
// ============================================================================
static void tick(lv_timer_t *timer)
{
    (void)timer;
    // BLE 同步回调运行在 NimBLE host 任务：LVGL 非线程安全且彼时未持
    // bsp_lvgl 锁，直接调 UI 刷新会与 LVGL 任务的渲染并发操作 LVGL 堆，
    // 导致死锁/堆损坏（表现为写入成功后整机卡死）。
    // 因此回调只置标志，在这里（LVGL 定时器上下文，已持锁）完成刷新。
    if (s_sync_pending) {
        s_sync_pending = false;
        s_last_sec = -1;   // 强制立即刷新显示
    }

    time_manager_state_t st = time_manager_get_state();
    int64_t now_ms = esp_timer_get_time() / 1000;

    // ---- 新同步事件边沿检测（BLE / USB 写时间都会让 sync_count 自增）----
    // 只有在同步页才安排"5 秒后自动进表盘"（顺带关 BLE 省电）；
    // 在其他页面收到同步事件（如表盘页 USB 对时）只刷新时间，不跳页面。
    if (st.sync_count != s_last_sync_count) {
        s_last_sync_count = st.sync_count;
        if (s_page == PAGE_SYNC && s_auto_exit_ms == 0) {
            s_auto_exit_ms = now_ms + SYNC_AUTO_EXIT_MS;
            ESP_LOGI(TAG, "时间同步成功，%d 秒后自动进入表盘并关闭 BLE",
                     SYNC_AUTO_EXIT_MS / 1000);
        }
    }

    // ---- 到点自动离开同步页 ----
    if (s_auto_exit_ms > 0 && now_ms >= s_auto_exit_ms) {
        s_auto_exit_ms = 0;
        page_switch(PAGE_CLOCK);   // 内部会停 BLE
        return;
    }

    // ---- 每秒刷新当前页面 ----
    time_t now = time_manager_get_unix_utc();
    time_t local = now + st.tz_offset;
    struct tm tm;
    localtime_r(&local, &tm);

    if (tm.tm_sec != s_last_sec) {
        s_last_sec = tm.tm_sec;
        if (s_page == PAGE_CLOCK) {
            update_time_display();
            // 每 5 秒刷新电池
            if (tm.tm_sec % 5 == 0) {
                battery_refresh();
            }
        } else if (s_page == PAGE_SYNC) {
            sync_page_update();
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
    ESP_LOGI(TAG, "进入赛博朋克时钟（v8 四页面动线）");
    s_enter_time = esp_timer_get_time();
    s_last_sec = -1;
    s_batt_soc = -1;
    s_theme_idx = 0;
    s_mode = MODE_FULL;
    s_menu_sel = 0;
    s_auto_exit_ms = 0;
    brightness_load();

    // 根屏幕 + 四个页面容器（互斥显示，切换即隐藏其余）
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(THEMES[0].bg), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    for (int i = 0; i < PAGE_COUNT_; i++) {
        s_pg[i] = make_page_container();
        lv_obj_add_flag(s_pg[i], LV_OBJ_FLAG_HIDDEN);
    }
    build_sync_page();
    build_clock_page();
    build_menu_page();
    build_bright_page();

    lv_scr_load(s_scr);

    // 应用持久化的亮度（main.c 先设了 100%，这里恢复用户设定）
    brightness_apply();

    // 初始化电池读数
    battery_refresh();

    // BLE 同步回调只注册一次（跨页面共用）
    ble_time_sync_set_sync_callback(on_ble_sync);

    // 开机首屏：时间同步页（内部自动开启 BLE）
    page_switch(PAGE_SYNC);

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
    s_auto_exit_ms = 0;
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
    memset(s_pg, 0, sizeof(s_pg));

    s_time_label = NULL;
    s_sec_label = NULL;
    s_date_label = NULL;
    s_weekday_label = NULL;
    s_status_label = NULL;
    s_uptime_label = NULL;
    s_sync_icon = NULL;
    s_sync_label = NULL;
    s_batt_icon_top = NULL;
    s_batt_frame = NULL;
    s_batt_fill = NULL;
    s_batt_cap = NULL;
    s_batt_pct = NULL;
    s_batt_volt = NULL;
    s_divider1 = NULL;
    s_divider2 = NULL;
    memset(s_corners, 0, sizeof(s_corners));

    s_sp_title = NULL;
    s_sp_time = NULL;
    s_sp_sec = NULL;
    s_sp_date = NULL;
    s_sp_status = NULL;
    s_sp_bt = NULL;
    s_sp_hint_usb = NULL;
    s_sp_hint_ble = NULL;
    s_sp_action = NULL;
    s_sp_div = NULL;

    s_mp_title = NULL;
    memset(s_mp_panels, 0, sizeof(s_mp_panels));
    memset(s_mp_rows, 0, sizeof(s_mp_rows));
    s_mp_hint1 = NULL;
    s_mp_hint2 = NULL;
    s_mp_div = NULL;

    s_bp_title = NULL;
    s_bp_pct = NULL;
    s_bp_frame = NULL;
    s_bp_fill = NULL;
    s_bp_hint1 = NULL;
    s_bp_hint2 = NULL;
    s_bp_div = NULL;
}

// ============================================================================
// 按键处理（按页面分发；已由 main.c 持 bsp_lvgl 锁后转发）
// ============================================================================
void demo_cyber_clock_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // ---- OK 长按：全局"上一层"导航 ----
    //   表盘/同步页 → 菜单页；菜单页 → 表盘；亮度页 → 菜单页
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s_page == PAGE_CLOCK || s_page == PAGE_SYNC) page_switch(PAGE_MENU);
        else if (s_page == PAGE_MENU)                    page_switch(PAGE_CLOCK);
        else                                             page_switch(PAGE_MENU);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    switch (s_page) {

    case PAGE_SYNC:
        // OK：跳过等待，直接进表盘（同步页长按也可去菜单）
        if (btn == BSP_BTN_OK) page_switch(PAGE_CLOCK);
        break;

    case PAGE_CLOCK:
        // 表盘页没有蓝牙，也不提供同步功能（都在同步页）
        if (btn == BSP_BTN_UP) {
            // 切换配色主题
            s_theme_idx = (s_theme_idx + 1) % THEME_COUNT;
            ESP_LOGI(TAG, "切换主题: %d", s_theme_idx);
            apply_theme();
        } else if (btn == BSP_BTN_DOWN) {
            // 切换显示模式
            s_mode = (s_mode + 1) % MODE_COUNT;
            ESP_LOGI(TAG, "切换模式: %d", s_mode);
            if (s_mode == MODE_MINIMAL) {
                lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_batt_volt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_divider2, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_batt_volt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_divider2, LV_OBJ_FLAG_HIDDEN);
            }
        }
        break;

    case PAGE_MENU:
        if (btn == BSP_BTN_UP) {
            s_menu_sel = (s_menu_sel + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
            menu_refresh();
        } else if (btn == BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % MENU_ITEM_COUNT;
            menu_refresh();
        } else if (btn == BSP_BTN_OK) {
            switch (s_menu_sel) {
            case 0: page_switch(PAGE_CLOCK); break;   // 1 进入表盘
            case 1: page_switch(PAGE_BRIGHT); break;  // 2 亮度调整
            case 2: page_switch(PAGE_SYNC); break;    // 3 时间同步
            default: break;
            }
        }
        break;

    case PAGE_BRIGHT:
        if (btn == BSP_BTN_UP && s_brightness < BRIGHT_MAX) {
            s_brightness += BRIGHT_STEP;
            brightness_apply();
            brightness_save();   // 即时持久化，返回时无需确认
            bright_page_update();
        } else if (btn == BSP_BTN_DOWN && s_brightness > BRIGHT_MIN) {
            s_brightness -= BRIGHT_STEP;
            brightness_apply();
            brightness_save();
            bright_page_update();
        } else if (btn == BSP_BTN_OK) {
            page_switch(PAGE_MENU);
        }
        break;

    default:
        break;
    }
}
