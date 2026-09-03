// main/cyber_clock.c —— 赛博朋克时钟：四页面状态机（v10 B1++ 表盘重制）
//
// v8/v9 页面动线（240x320 竖屏，三个实体按键）：
//   开机 → PAGE_SYNC（时间同步页：BLE 只在本页开启；同步成功后停留本页，
//          按 OK 手动进表盘）
//   PAGE_CLOCK（表盘页：纯显示，无蓝牙。UP=主题 DOWN=模式 OK长按=菜单）
//   PAGE_MENU（菜单页：1 表盘 / 2 亮度 / 3 时间同步。UP/DOWN 选择，OK 进入，
//          OK 长按回表盘）
//   PAGE_BRIGHT（亮度页：UP/DOWN 以 10% 步进调整 10..100，NVS 持久化，
//          OK/长按返回菜单）
//
// v10 表盘页按 B1++ 定稿重制（统一 20px 内容边距、右缘对齐、准星几何居中）：
//   - 版式：内嵌外框 + 上下刻度尺（中轴 120 长格）+ 四角双线角标
//           顶栏（型号 / 十六进制流 / 电量%）/ 双色差残影大时间 + 强调线
//           秒数 + 垂直秒进度柱 / 日期星期行 / 12 根频谱条
//           终端行（> UTC+8_ 光标闪烁 | 像素心形 + 运行时长）
//           分段电池（10 段）+ 底部十六进制流
//   - 5 主题：0 霓虹青紫 / 1 传统墨水屏（纸白墨黑，静态：无残影、无故障特效）
//             2 青绿 / 3 橙红 / 4 矩阵绿
//   - 动效（100ms tick 状态机，全部确定性伪随机，零额外资源）：
//     · 故障爆发：约 7s 一次（秒数 %7==3），300ms 内色差残影分离加剧、
//       主字微移轻闪、时间区 1-2 条错位切片条（墨水屏与精简模式禁用）
//     · 故障色条：每 3s 换位（墨水屏与精简模式禁用）
//     · 心跳：像素心形按秒 lub-dub 双跳（放大/复位/弱跳/复位）
//     · 频谱随秒数起伏、光标按秒闪烁、未同步时时间主字呼吸闪烁（v7 保留）
//
// 设计要点：
//   - 表盘页彻底不管蓝牙（进同步页必开广播，离开即停）
//   - USB 串口对时是常驻服务，任何页面都可完成同步
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
#include <math.h>

static const char *TAG = "cyber_clk";

// LVGL 透明度预置档只有 10 的整数倍：残影 2 的 0.15 档自定义（38/255）
#define GHOST2_OPA ((lv_opa_t)38)

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
    { // 1: 传统墨水屏黑白（第二屏：纸白底 + 墨黑字，纯静态——
       //    隐藏色差残影与故障元素，无任何故障特效）
        .bg = 0xEDEBE2,
        .primary = 0x17171C,
        .secondary = 0x4A4A52,
        .text_dim = 0x8A887C,
        .warn = 0x3A3A3A,
        .battery_ok = 0x2E2E2E,
        .hud_line = 0xB9B6AA,
    },
    { // 2: 青绿单色
        .bg = 0x040808,
        .primary = 0x00FFCC,
        .secondary = 0x00AA88,
        .text_dim = 0x448877,
        .warn = 0xFF6644,
        .battery_ok = 0x44FFAA,
        .hud_line = 0x003322,
    },
    { // 3: 橙红暖色
        .bg = 0x120804,
        .primary = 0xFF8800,
        .secondary = 0xFF2244,
        .text_dim = 0xAA6644,
        .warn = 0xFFFF00,
        .battery_ok = 0xFFAA00,
        .hud_line = 0x442200,
    },
    { // 4: 矩阵绿
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
#define THEME_EINK  1   // 墨水屏主题索引：静态专属处理都以此判断

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

// 亮度范围与步进
#define BRIGHT_MIN   10
#define BRIGHT_MAX   100
#define BRIGHT_STEP  10
// 出厂默认亮度：不再拉满 100（满亮度下 OLED 偏刺眼且功耗高）
#define BRIGHT_DEFAULT 80
// v10.2 空闲降亮度：长时间无按键后背光压到 DIM_LEVEL（用户设定 s_brightness
// 不变、不写 NVS，任意按键立即恢复）。背光是整机最大的持续耗电项之一。
#define DIM_AFTER_S 90
#define DIM_LEVEL   20

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

// ---- 表盘页控件（v10 B1++ 版式）----
static lv_obj_t *s_frame;                  // 内嵌外框（3px 内缩 1px 描边）
static lv_obj_t *s_ruler_top;              // 顶部刻度尺横条
static lv_obj_t *s_ruler_bot;              // 底部刻度尺横条
static lv_obj_t *s_ticks_top[13];          // 顶部刻度（中轴 x=120 长格用主色）
static lv_obj_t *s_ticks_bot[13];          // 底部刻度
static lv_obj_t *s_corner[8];              // 四角双线角标
static lv_obj_t *s_hdr_model;              // 顶栏左：CC-320
static lv_obj_t *s_hdr_hex;                // 顶栏中：十六进制流 0x????
static lv_obj_t *s_hdr_batt;               // 顶栏右：电量 %
static lv_obj_t *s_time_ghost1;            // 时间色差残影 1（左偏，暗）
static lv_obj_t *s_time_ghost2;            // 时间色差残影 2（右偏，更暗）
static lv_obj_t *s_time_label;             // HH:MM 主字 48px
static lv_obj_t *s_under1;                 // 强调线主段（124x2）
static lv_obj_t *s_under2;                 // 强调线次段（84x1）
static lv_obj_t *s_sec_label;              // SS 24px
static lv_obj_t *s_secbar_frame;           // 秒进度柱外框
static lv_obj_t *s_secbar_fill;            // 秒进度柱填充
static lv_obj_t *s_div1_l, *s_div1_r;      // 分割线 1 左右段（带准星缺口）
static lv_obj_t *s_div1_n;                 // 分割线 1 准星缺口
static lv_obj_t *s_div2_l, *s_div2_r;      // 分割线 2 左右段
static lv_obj_t *s_div2_n;                 // 分割线 2 准星缺口
static lv_obj_t *s_div3;                   // 分割线 3（通栏）
static lv_obj_t *s_date_label;             // 日期
static lv_obj_t *s_weekday_label;          // 星期（右缘对齐）
static lv_obj_t *s_spec[12];               // 频谱条 12 根
static lv_obj_t *s_term_label;             // 终端行：> UTC+8_
static lv_obj_t *s_heart_sm[7];            // 像素心形（常规 9x7）
static lv_obj_t *s_heart_bg[7];            // 像素心形（放大 12x10，心跳用）
static lv_obj_t *s_uptime_label;           // 运行时长（心形右侧，右缘对齐）
static lv_obj_t *s_gbar[3];                // 故障色条 3 根（每 3s 换位）
static lv_obj_t *s_slice[2];               // 故障切片条（爆发时显示）
static lv_obj_t *s_batt_frame;             // 电池外框（2px 描边）
static lv_obj_t *s_batt_cap;               // 电池正极凸块
static lv_obj_t *s_batt_seg[10];           // 电池 10 分段
static lv_obj_t *s_batt_pct;               // 电池百分比
static lv_obj_t *s_batt_volt;              // 电池电压（右缘对齐）
static lv_obj_t *s_stream2;                // 底部十六进制流 >> 0x??????

// ---- 表盘动效状态（100ms tick 驱动）----
static int s_glitch_ticks = 0;             // 故障爆发剩余 tick 数（0=常态）
static int s_beat_phase = 4;               // 心跳相位（0..3 活跃，>=4 静止）
static uint32_t s_rnd = 1;                 // 确定性伪随机（种子=触发秒）

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
static uint8_t s_brightness = BRIGHT_DEFAULT; // 当前亮度（10..100，出厂默认 80）
static int s_theme_idx = 0;
static display_mode_t s_mode = MODE_FULL;
static int s_last_sec = -1;
static int s_batt_soc = -1;
static int s_batt_mv = 0;
static int64_t s_enter_time;
static bool s_ble_started = false;
static volatile bool s_sync_pending;      // BLE 同步事件标志（跨任务传递）
static int64_t s_last_key_us;             // 最近一次按键时刻（空闲降亮度用）
static bool s_dimmed;                     // 当前处于空闲降亮状态

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

// 文字标签快捷创建（指定字体与颜色）
static lv_obj_t *make_label(lv_obj_t *parent, int x, int y,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// 右缘对齐标签：给定右边界 x2，标签占 [x, x2] 并右对齐文本
//
// 注意：y 必须由调用方显式传入，不能用 lv_obj_get_y(l) 读回来。
// lv_obj_get_y() 读的是 obj->coords，而 coords 要等下一次布局刷新才由 style 计算出来；
// 本函数在 make_label() 之后紧接着调用，此刻 coords 仍是 lv_obj 构造时的初始值
// (coords.y1 = parent->coords.y1 + pad_top)，换算出的 rel_y 恒为 0。
// 早期写法 lv_obj_set_pos(l, x, lv_obj_get_y(l)) 会把 y 覆盖成 0，
// 导致所有右对齐标签（电量%/星期/运行时长/电压）全部堆到页面顶端 y=0 处互相重叠。
static void label_right_align(lv_obj_t *l, int x, int y, int x2)
{
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, x2 - x);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

// 确定性伪随机（LCG）：种子由触发时刻的秒数决定，同秒重现同图案
static uint32_t rnd_next(void)
{
    s_rnd = s_rnd * 1664525u + 1013904223u;
    return s_rnd >> 8;
}

static void rnd_seed(uint32_t seed)
{
    s_rnd = (seed * 2654435761u) | 1u;
}

// 墨水屏主题：纯静态（无残影/无故障特效），灰阶元素透明度降低模拟网点
static inline bool theme_is_eink(void)
{
    return s_theme_idx == THEME_EINK;
}

// 显示模式辅助：精简模式隐藏装饰流（顶栏十六进制/频谱/底流/故障色条）
static inline bool mode_is_full(void)
{
    return s_mode == MODE_FULL;
}

// ============================================================================
// 心跳（像素心形两组互换：常规 9x7 / 放大 12x10）
// ============================================================================
static void heart_set(bool big)
{
    for (int i = 0; i < 7; i++) {
        if (big) {
            lv_obj_add_flag(s_heart_sm[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_heart_bg[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_heart_sm[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_heart_bg[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// 故障爆发（收敛版：±5px 色差分离 + 主字 ±1px 轻闪 + 1-2 条错位切片）
// ============================================================================
static void glitch_apply(int sec)
{
    rnd_seed((uint32_t)sec);
    lv_obj_set_x(s_time_ghost1, 16 - 5);
    lv_obj_set_x(s_time_ghost2, 26 + 5);
    lv_obj_set_style_text_opa(s_time_ghost1, LV_OPA_40, 0);
    lv_obj_set_style_text_opa(s_time_ghost2, LV_OPA_30, 0);
    lv_obj_set_x(s_time_label, 20 + (rnd_next() & 1 ? 1 : -1));
    lv_obj_set_style_text_opa(s_time_label, LV_OPA_70, 0);
    // 切片条：1-2 条（第二条按秒奇偶出现），宽 30-89，落在时间区 y55-124
    for (int k = 0; k < 2; k++) {
        bool show = (k == 0) || (sec & 1);
        if (!show) {
            lv_obj_add_flag(s_slice[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int w = 30 + (int)(rnd_next() % 60);
        int x = (int)(rnd_next() % (uint32_t)(220 - w));
        int y = 55 + (int)(rnd_next() % 70);
        lv_obj_set_pos(s_slice[k], x, y);
        lv_obj_set_size(s_slice[k], w, 2);
        lv_obj_clear_flag(s_slice[k], LV_OBJ_FLAG_HIDDEN);
    }
}

static void glitch_restore(void)
{
    lv_obj_set_x(s_time_ghost1, 16);
    lv_obj_set_x(s_time_ghost2, 26);
    lv_obj_set_style_text_opa(s_time_ghost1, LV_OPA_30, 0);
    lv_obj_set_style_text_opa(s_time_ghost2, GHOST2_OPA, 0);
    lv_obj_set_x(s_time_label, 20);
    lv_obj_set_style_text_opa(s_time_label, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_slice[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_slice[1], LV_OBJ_FLAG_HIDDEN);
}

// 故障色条换位（每 3s）：x 随机 0-169，y 在 {50,128,176} 三档
static void gbar_shuffle(int sec)
{
    rnd_seed((uint32_t)sec);
    static const int ys[3] = { 50, 128, 176 };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_x(s_gbar[i], (int)(rnd_next() % 170));
        lv_obj_set_y(s_gbar[i], ys[rnd_next() % 3]);
    }
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

    // 分段电池：10 段，lit = ceil(soc/10)（87% → 9 段点亮）
    uint32_t batt_color = (soc < 20) ? t->warn : t->battery_ok;
    int lit = (soc + 9) / 10;
    lv_opa_t lit_opa = theme_is_eink() ? LV_OPA_50 : LV_OPA_90;
    for (int i = 0; i < 10; i++) {
        lv_obj_set_style_bg_color(s_batt_seg[i], lv_color_hex(batt_color), 0);
        lv_obj_set_style_bg_opa(s_batt_seg[i],
            (i < lit) ? lit_opa : LV_OPA_20, 0);
    }
    lv_obj_set_style_border_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_batt_cap, lv_color_hex(t->primary), 0);

    // 电池行百分比文字
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", soc);
    lv_label_set_text(s_batt_pct, buf);
    set_text_color(s_batt_pct, batt_color);

    // 顶栏右缘电量%
    lv_label_set_text(s_hdr_batt, buf);

    // 电压文字（右缘对齐）
    snprintf(buf, sizeof(buf), "%d.%02dV", s_batt_mv / 1000, (s_batt_mv % 1000) / 10);
    lv_label_set_text(s_batt_volt, buf);
    set_text_color(s_batt_volt, t->text_dim);
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

    // ---- 内嵌外框（3px 内缩，1px 描边）----
    s_frame = make_rect(parent, 3, 3, 234, 314, t->hud_line);
    lv_obj_set_style_bg_opa(s_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_frame, 1, 0);
    lv_obj_set_style_border_color(s_frame, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_opa(s_frame, LV_OPA_TRANSP, 0);

    // ---- 上下刻度尺横条（240x2）----
    s_ruler_top = make_rect(parent, 0, 0, 240, 2, t->hud_line);
    s_ruler_bot = make_rect(parent, 0, 318, 240, 2, t->hud_line);

    // ---- 刻度 13 格：x = 20 + i*200/12（四舍五入），中轴 i=6 用主色长格 ----
    for (int i = 0; i <= 12; i++) {
        int x = 20 + (i * 200 + 6) / 12;
        bool mid = (i == 6);
        int h = mid ? 9 : ((i % 3 == 0) ? 7 : 4);
        s_ticks_top[i] = make_rect(parent, x, 5, 1, h,
                                   mid ? t->primary : t->hud_line);
        if (mid) lv_obj_set_style_bg_opa(s_ticks_top[i], LV_OPA_90, 0);
        s_ticks_bot[i] = make_rect(parent, x, 317 - h, 1, h,
                                   mid ? t->primary : t->hud_line);
        if (mid) lv_obj_set_style_bg_opa(s_ticks_bot[i], LV_OPA_90, 0);
    }

    // ---- 四角双线角标（8px 边距，14x2 + 2x14）----
    {
        static const int cs[8][4] = {
            { 8, 8, 14, 2 }, { 8, 8, 2, 14 },          // 左上
            { 218, 8, 14, 2 }, { 230, 8, 2, 14 },      // 右上
            { 8, 310, 14, 2 }, { 8, 298, 2, 14 },      // 左下
            { 218, 310, 14, 2 }, { 230, 298, 2, 14 },  // 右下
        };
        for (int i = 0; i < 8; i++) {
            s_corner[i] = make_rect(parent, cs[i][0], cs[i][1],
                                    cs[i][2], cs[i][3], t->primary);
        }
    }

    // ---- 顶栏 y14：型号 / 十六进制流 / 电量%（统一 20px 边距）----
    s_hdr_model = make_label(parent, 20, 14, &lv_font_montserrat_12, t->text_dim);
    lv_label_set_text(s_hdr_model, "CC-320");
    s_hdr_hex = make_label(parent, 0, 14, &lv_font_montserrat_12, t->text_dim);
    lv_obj_set_width(s_hdr_hex, 240);
    lv_obj_set_style_text_align(s_hdr_hex, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_hdr_hex, "0x0000");
    s_hdr_batt = make_label(parent, 160, 14, &lv_font_montserrat_12, t->text_dim);
    lv_label_set_text(s_hdr_batt, "--%");
    label_right_align(s_hdr_batt, 160, 14, 220);

    // ---- 故障切片条（爆发时显示，默认隐藏）----
    s_slice[0] = make_rect(parent, 0, 0, 40, 2, t->secondary);
    s_slice[1] = make_rect(parent, 0, 0, 40, 2, t->primary);
    lv_obj_set_style_bg_opa(s_slice[0], LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(s_slice[1], LV_OPA_50, 0);
    lv_obj_add_flag(s_slice[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_slice[1], LV_OBJ_FLAG_HIDDEN);

    // ---- 故障色条 3 根（每 3s 换位；墨水屏/精简模式隐藏）----
    s_gbar[0] = make_rect(parent, 12, 50, 70, 3, t->secondary);
    s_gbar[1] = make_rect(parent, 150, 128, 56, 2, t->primary);
    s_gbar[2] = make_rect(parent, 36, 176, 32, 2, t->battery_ok);
    lv_obj_set_style_bg_opa(s_gbar[0], LV_OPA_70, 0);
    lv_obj_set_style_bg_opa(s_gbar[1], LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(s_gbar[2], LV_OPA_60, 0);

    // ---- 时间组：双色差残影 + 主字（48px，压 20px 基准线）----
    s_time_ghost1 = make_label(parent, 16, 60, &lv_font_montserrat_48, t->secondary);
    lv_obj_set_style_text_opa(s_time_ghost1, LV_OPA_30, 0);
    lv_label_set_text(s_time_ghost1, "00:00");
    s_time_ghost2 = make_label(parent, 26, 63, &lv_font_montserrat_48, t->secondary);
    lv_obj_set_style_text_opa(s_time_ghost2, GHOST2_OPA, 0);
    lv_label_set_text(s_time_ghost2, "00:00");
    s_time_label = make_label(parent, 20, 60, &lv_font_montserrat_48, t->primary);
    lv_label_set_text(s_time_label, "00:00");

    // ---- 强调线：与时间同起点，124x2 主段 + 84x1 次段 ----
    s_under1 = make_rect(parent, 20, 120, 124, 2, t->primary);
    lv_obj_set_style_bg_opa(s_under1, LV_OPA_80, 0);
    s_under2 = make_rect(parent, 20, 122, 84, 1, t->secondary);
    lv_obj_set_style_bg_opa(s_under2, LV_OPA_50, 0);

    // ---- 秒：SS 24px + 垂直进度柱（外框 210,66 10x52；填充 212,68 6x48）----
    s_sec_label = make_label(parent, 162, 72, &lv_font_montserrat_24, t->secondary);
    lv_label_set_text(s_sec_label, "00");
    s_secbar_frame = make_rect(parent, 210, 66, 10, 52, t->hud_line);
    lv_obj_set_style_bg_opa(s_secbar_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_secbar_frame, 1, 0);
    lv_obj_set_style_border_color(s_secbar_frame, lv_color_hex(t->hud_line), 0);
    s_secbar_fill = make_rect(parent, 212, 68, 6, 48, t->secondary);

    // ---- 分割线 1（y140）：两段 92px，缺口中心 x=120，准星 4x7 ----
    s_div1_l = make_rect(parent, 20, 140, 92, 1, t->hud_line);
    s_div1_r = make_rect(parent, 128, 140, 92, 1, t->hud_line);
    s_div1_n = make_rect(parent, 118, 137, 4, 7, t->secondary);
    lv_obj_set_style_bg_opa(s_div1_n, LV_OPA_80, 0);

    // ---- 日期行 y152：日期左 / 星期右（右缘 220）----
    s_date_label = make_label(parent, 20, 152, &lv_font_montserrat_16, t->text_dim);
    lv_label_set_text(s_date_label, "2026-01-01");
    s_weekday_label = make_label(parent, 160, 152, &lv_font_montserrat_16, t->primary);
    lv_label_set_text(s_weekday_label, "THU");
    label_right_align(s_weekday_label, 160, 152, 220);

    // ---- 频谱条 12 根：x=20+i*17，宽 9，底对齐 y202，高 3-17 随秒起伏 ----
    for (int i = 0; i < 12; i++) {
        s_spec[i] = make_rect(parent, 20 + i * 17, 196, 9, 6, t->secondary);
        // 透明度阶梯 .35/.50/.65/.80（模拟每根亮度差）
        lv_obj_set_style_bg_opa(s_spec[i], (lv_opa_t)(88 + (i % 4) * 38), 0);
    }

    // ---- 分割线 2（y212，同规格；准星用主色）----
    s_div2_l = make_rect(parent, 20, 212, 92, 1, t->hud_line);
    s_div2_r = make_rect(parent, 128, 212, 92, 1, t->hud_line);
    s_div2_n = make_rect(parent, 118, 209, 4, 7, t->primary);
    lv_obj_set_style_bg_opa(s_div2_n, LV_OPA_70, 0);

    // ---- 终端行 y226：> UTC+8_（光标按秒闪烁）----
    s_term_label = make_label(parent, 20, 226, &lv_font_montserrat_14, t->primary);
    lv_label_set_text(s_term_label, "> UTC+8_");

    // ---- 像素心形（两组：常规 9x7 @163,228 / 放大 12x10 @162,226）----
    {
        // 小心形 9x7 @(163,228)：7 块按 h=1/1/2/1/1/1/1 叠成 y228-234，行必须连续。
        // 旧版第 4 块写在 y232（应为 y231），y231 整行缺失，
        // 心形腰部被一条 1px 透明缝横着切断 —— 视觉上就是"多余的横线 / 显示不全"。
        static const int sm[7][4] = {
            { 164, 228, 3, 1 }, { 168, 228, 3, 1 }, { 163, 229, 9, 2 },
            { 164, 231, 7, 1 }, { 165, 232, 5, 1 }, { 166, 233, 3, 1 },
            { 167, 234, 1, 1 },
        };
        // 放大心形 12x10 @(162,226)：7 块按 h=2/2/3/2/1/1/1 叠成 y226-235，行连续无缝。
        static const int bg[7][4] = {
            { 163, 226, 4, 2 }, { 169, 226, 4, 2 }, { 162, 228, 12, 3 },
            { 164, 231, 8, 2 }, { 165, 233, 6, 1 }, { 166, 234, 4, 1 },
            { 167, 235, 2, 1 },
        };
        for (int i = 0; i < 7; i++) {
            s_heart_sm[i] = make_rect(parent, sm[i][0], sm[i][1],
                                      sm[i][2], sm[i][3], t->text_dim);
            s_heart_bg[i] = make_rect(parent, bg[i][0], bg[i][1],
                                      bg[i][2], bg[i][3], t->text_dim);
            lv_obj_add_flag(s_heart_bg[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- 运行时长（心形右侧，右缘对齐 220）----
    s_uptime_label = make_label(parent, 174, 226, &lv_font_montserrat_14, t->text_dim);
    lv_label_set_text(s_uptime_label, "00:00");
    label_right_align(s_uptime_label, 174, 226, 220);

    // ---- 分割线 3（y252，通栏 200px）----
    s_div3 = make_rect(parent, 20, 252, 200, 1, t->hud_line);

    // ---- 分段电池行 y264-284：外框 104x20（2px 描边）+ 正极 + 10 段 ----
    s_batt_frame = make_rect(parent, 20, 264, 104, 20, t->primary);
    lv_obj_set_style_bg_opa(s_batt_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_batt_frame, 2, 0);
    lv_obj_set_style_border_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_radius(s_batt_frame, 2, 0);
    s_batt_cap = make_rect(parent, 124, 270, 4, 8, t->primary);
    for (int i = 0; i < 10; i++) {
        s_batt_seg[i] = make_rect(parent, 22 + i * 10, 266, 8, 16, t->battery_ok);
    }
    s_batt_pct = make_label(parent, 140, 264, &lv_font_montserrat_16, t->battery_ok);
    lv_label_set_text(s_batt_pct, "--%");
    s_batt_volt = make_label(parent, 176, 267, &lv_font_montserrat_12, t->text_dim);
    lv_label_set_text(s_batt_volt, "--.--V");
    label_right_align(s_batt_volt, 176, 267, 220);

    // ---- 底部十六进制流 y296 ----
    s_stream2 = make_label(parent, 20, 296, &lv_font_montserrat_12, t->text_dim);
    lv_label_set_text(s_stream2, ">> 0x000000");
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

    // 底部动作提示（v9：同步后停留本页，不自动跳转）
    if (st.synced) {
        lv_label_set_text(s_sp_action, "OK: ENTER CLOCK");
    } else {
        lv_label_set_text(s_sp_action, "OK: SKIP TO CLOCK");
    }
    set_text_color(s_sp_action, t->text_dim);
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

    // ---- 表盘页（v10 B1++：全元素按主题重着色）----
    bool eink = theme_is_eink();
    // 换主题先结束故障爆发，回到常态坐标（避免残留偏移）
    if (s_glitch_ticks > 0) {
        s_glitch_ticks = 0;
        glitch_restore();
    }
    // 外框 / 刻度尺 / 刻度
    lv_obj_set_style_border_color(s_frame, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_ruler_top, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_ruler_bot, lv_color_hex(t->hud_line), 0);
    for (int i = 0; i <= 12; i++) {
        bool mid = (i == 6);
        lv_obj_set_style_bg_color(s_ticks_top[i],
            lv_color_hex(mid ? t->primary : t->hud_line), 0);
        lv_obj_set_style_bg_color(s_ticks_bot[i],
            lv_color_hex(mid ? t->primary : t->hud_line), 0);
    }
    // 四角角标 / 顶栏
    for (int i = 0; i < 8; i++)
        lv_obj_set_style_bg_color(s_corner[i], lv_color_hex(t->primary), 0);
    set_text_color(s_hdr_model, t->text_dim);
    set_text_color(s_hdr_hex, t->text_dim);
    set_text_color(s_hdr_batt, t->text_dim);
    // 时间组（墨水屏隐藏双残影）
    set_text_color(s_time_ghost1, t->secondary);
    set_text_color(s_time_ghost2, t->secondary);
    set_text_color(s_time_label, t->primary);
    lv_obj_set_style_text_opa(s_time_ghost1, LV_OPA_30, 0);
    lv_obj_set_style_text_opa(s_time_ghost2, GHOST2_OPA, 0);
    (eink ? lv_obj_add_flag : lv_obj_clear_flag)(s_time_ghost1, LV_OBJ_FLAG_HIDDEN);
    (eink ? lv_obj_add_flag : lv_obj_clear_flag)(s_time_ghost2, LV_OBJ_FLAG_HIDDEN);
    // 强调线 / 秒区
    lv_obj_set_style_bg_color(s_under1, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_under2, lv_color_hex(t->secondary), 0);
    set_text_color(s_sec_label, t->secondary);
    lv_obj_set_style_border_color(s_secbar_frame, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_secbar_fill, lv_color_hex(t->secondary), 0);
    // 分割线 1/2/3 + 准星
    lv_obj_set_style_bg_color(s_div1_l, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_div1_r, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_div1_n, lv_color_hex(t->secondary), 0);
    lv_obj_set_style_bg_color(s_div2_l, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_div2_r, lv_color_hex(t->hud_line), 0);
    lv_obj_set_style_bg_color(s_div2_n, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_div3, lv_color_hex(t->hud_line), 0);
    // 日期行 / 频谱 / 终端行
    set_text_color(s_date_label, t->text_dim);
    set_text_color(s_weekday_label, t->primary);
    for (int i = 0; i < 12; i++)
        lv_obj_set_style_bg_color(s_spec[i], lv_color_hex(t->secondary), 0);
    set_text_color(s_term_label, t->primary);
    // 心形（两组同色）/ 运行时长
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_bg_color(s_heart_sm[i], lv_color_hex(t->text_dim), 0);
        lv_obj_set_style_bg_color(s_heart_bg[i], lv_color_hex(t->text_dim), 0);
    }
    set_text_color(s_uptime_label, t->text_dim);
    // 故障色条 / 切片（墨水屏隐藏）
    lv_obj_set_style_bg_color(s_gbar[0], lv_color_hex(t->secondary), 0);
    lv_obj_set_style_bg_color(s_gbar[1], lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_gbar[2], lv_color_hex(t->battery_ok), 0);
    lv_obj_set_style_bg_color(s_slice[0], lv_color_hex(t->secondary), 0);
    lv_obj_set_style_bg_color(s_slice[1], lv_color_hex(t->primary), 0);
    for (int i = 0; i < 3; i++) {
        if (eink || !mode_is_full())
            lv_obj_add_flag(s_gbar[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_gbar[i], LV_OBJ_FLAG_HIDDEN);
    }
    // 电池 / 底流
    lv_obj_set_style_border_color(s_batt_frame, lv_color_hex(t->primary), 0);
    lv_obj_set_style_bg_color(s_batt_cap, lv_color_hex(t->primary), 0);
    set_text_color(s_stream2, t->text_dim);

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

    // HH:MM（主字 + 双色差残影同步刷新）
    // 48 字节：容纳各格式理论最大值（含终端行 "> UTC-12_"、底流 ">> 0xFFFFFF"）
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(s_time_label, buf);
    lv_label_set_text(s_time_ghost1, buf);
    lv_label_set_text(s_time_ghost2, buf);

    // 未同步时时间主文字呼吸闪烁（奇数秒半透明）：此刻显示的是 NVS 恢复的
    // 粗略旧值，不可信；闪烁提醒用户需要对时（v7 保留，墨水屏同样适用）
    lv_obj_set_style_text_opa(s_time_label,
        (!st.synced && (tm.tm_sec % 2)) ? LV_OPA_60 : LV_OPA_COVER, 0);

    // SS + 秒进度柱（内高 48px，本分钟已过秒数自下而上填充）
    snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
    lv_label_set_text(s_sec_label, buf);
    {
        int bar_h = ((tm.tm_sec + 1) * 48) / 60;
        if (bar_h < 0) bar_h = 0;
        if (bar_h > 48) bar_h = 48;
        lv_obj_set_height(s_secbar_fill, bar_h);
        lv_obj_set_y(s_secbar_fill, 68 + (48 - bar_h));
    }

    // 日期 / 星期
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    lv_label_set_text(s_date_label, buf);
    lv_label_set_text(s_weekday_label, WEEKDAYS[tm.tm_wday]);

    // 终端行：> UTC+8 光标按秒闪烁（奇数秒显示 _）
    int tz_hours = st.tz_offset / 3600;
    if (tm.tm_sec % 2) {
        snprintf(buf, sizeof(buf), "> UTC%+d_", tz_hours);
    } else {
        snprintf(buf, sizeof(buf), "> UTC%+d ", tz_hours);
    }
    lv_label_set_text(s_term_label, buf);

    // 心跳运行时长（HH:MM，小时封顶 99 防越界挤占心形）
    int64_t uptime_us = esp_timer_get_time() - s_enter_time;
    int uptime_sec = (int)(uptime_us / 1000000);
    int up_h = uptime_sec / 3600;
    if (up_h > 99) up_h = 99;
    snprintf(buf, sizeof(buf), "%02d:%02d", up_h, (uptime_sec % 3600) / 60);
    lv_label_set_text(s_uptime_label, buf);

    // 十六进制数据流：顶栏 4 位 / 底流 6 位（取 unix 时间戳高位）
    {
        uint32_t ts = (uint32_t)now;
        snprintf(buf, sizeof(buf), "0x%04X", (unsigned)(ts >> 16));
        lv_label_set_text(s_hdr_hex, buf);
        snprintf(buf, sizeof(buf), ">> 0x%06X", (unsigned)(ts >> 8));
        lv_label_set_text(s_stream2, buf);
    }

    // 频谱条：12 根随秒数确定性起伏（3-17px，底对齐 y202）
    for (int i = 0; i < 12; i++) {
        int v = 3 + (int)((sinf(tm.tm_sec * 2.1f + i * 1.7f) + 1.0f) * 7.0f);
        if (v < 3) v = 3;
        if (v > 17) v = 17;
        lv_obj_set_height(s_spec[i], v);
        lv_obj_set_y(s_spec[i], 202 - v);
    }

    // ---- 100ms 级动效的秒级触发 ----
    // 心跳：每秒重开一轮（phase0 = 立即放大）
    s_beat_phase = 0;
    heart_set(true);

    bool eink = theme_is_eink();
    bool full = mode_is_full();
    // 故障色条换位：每 3s（墨水屏/精简模式无故障元素）
    if (!eink && full && tm.tm_sec % 3 == 0) {
        gbar_shuffle(tm.tm_sec);
    }
    // 故障爆发：约 7s 一次（秒数 %7==3），持续 3 个 tick（300ms）
    if (!eink && full && tm.tm_sec % 7 == 3) {
        glitch_apply(tm.tm_sec);
        s_glitch_ticks = 3;
    }
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
// 表盘 100ms 级动效：心跳相位推进 + 故障爆发倒计时
// （先推进动效、后处理秒变化：秒边界当拍由 update_time_display 起搏，
//   随后每个 100ms tick 各推进一相，形成 lub-dub 双跳节奏）
static void clock_fx_tick(void)
{
    // 心跳：0→复位 1→弱跳(dub) 2→复位 3→静止
    switch (s_beat_phase) {
    case 0: heart_set(false); s_beat_phase = 1; break;  // +100ms 复位
    case 1: heart_set(true);  s_beat_phase = 2; break;  // +200ms 第二跳
    case 2: heart_set(false); s_beat_phase = 3; break;  // +300ms 复位
    case 3: s_beat_phase = 4; break;                    // 静止
    default: break;
    }
    // 故障爆发：3 个 tick（300ms）后恢复常态
    if (s_glitch_ticks > 0 && --s_glitch_ticks == 0) {
        glitch_restore();
    }
}

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

    // v9：同步成功后不再自动跳转——同步页就是"对时工作台"，停留本页持续
    // 显示 BT: LINKED 与 SYNCED 状态，由用户按 OK 手动进表盘（蓝牙随离页关闭）。

    // ---- 表盘 100ms 级动效（先于秒变化处理，见函数头注释）----
    if (s_page == PAGE_CLOCK) {
        clock_fx_tick();
    }

    // ---- 每秒刷新当前页面 ----
    time_manager_state_t st = time_manager_get_state();
    time_t now = time_manager_get_unix_utc();
    time_t local = now + st.tz_offset;
    struct tm tm;
    localtime_r(&local, &tm);

    if (tm.tm_sec != s_last_sec) {
        s_last_sec = tm.tm_sec;
        // 把当前时间回写 NVS（内部 5 分钟节流）：下次断电重启时恢复值才不会
        // 慢掉整个开机时长。放在这里是为了任何页面都能生效。
        time_manager_periodic_save();
        // v10.2 空闲降亮度：超过 DIM_AFTER_S 无按键 → 背光压到 DIM_LEVEL。
        // 不碰 s_brightness / NVS，按键路径负责恢复。
        if (!s_dimmed &&
            esp_timer_get_time() - s_last_key_us > (int64_t)DIM_AFTER_S * 1000000) {
            s_dimmed = true;
            bsp_display_backlight(DIM_LEVEL);
        }
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
    ESP_LOGI(TAG, "进入赛博朋克时钟（v10 B1++ 表盘）");
    s_enter_time = esp_timer_get_time();
    s_last_sec = -1;
    s_batt_soc = -1;
    s_theme_idx = 0;
    s_mode = MODE_FULL;
    s_menu_sel = 0;
    brightness_load();
    s_last_key_us = esp_timer_get_time();   // 空闲降亮度计时起点
    s_dimmed = false;

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

    // 启动 LVGL 定时器（先建后切页：page_switch 内会按目标页调整 tick 周期）
    s_timer = lv_timer_create(tick, 100, NULL);

    // 开机首屏：时间同步页（内部自动开启 BLE；tick 周期随即调为 250ms）
    page_switch(PAGE_SYNC);
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
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
    memset(s_pg, 0, sizeof(s_pg));

    s_time_label = NULL;
    s_time_ghost1 = NULL;
    s_time_ghost2 = NULL;
    s_sec_label = NULL;
    s_secbar_frame = NULL;
    s_secbar_fill = NULL;
    s_date_label = NULL;
    s_weekday_label = NULL;
    s_term_label = NULL;
    s_uptime_label = NULL;
    s_stream2 = NULL;
    s_hdr_model = NULL;
    s_hdr_hex = NULL;
    s_hdr_batt = NULL;
    s_under1 = NULL;
    s_under2 = NULL;
    s_div1_l = NULL; s_div1_r = NULL; s_div1_n = NULL;
    s_div2_l = NULL; s_div2_r = NULL; s_div2_n = NULL;
    s_div3 = NULL;
    s_frame = NULL;
    s_ruler_top = NULL;
    s_ruler_bot = NULL;
    memset(s_ticks_top, 0, sizeof(s_ticks_top));
    memset(s_ticks_bot, 0, sizeof(s_ticks_bot));
    memset(s_corner, 0, sizeof(s_corner));
    memset(s_spec, 0, sizeof(s_spec));
    memset(s_heart_sm, 0, sizeof(s_heart_sm));
    memset(s_heart_bg, 0, sizeof(s_heart_bg));
    memset(s_gbar, 0, sizeof(s_gbar));
    memset(s_slice, 0, sizeof(s_slice));
    s_batt_frame = NULL;
    s_batt_cap = NULL;
    memset(s_batt_seg, 0, sizeof(s_batt_seg));
    s_batt_pct = NULL;
    s_batt_volt = NULL;
    s_glitch_ticks = 0;
    s_beat_phase = 4;

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
    // v10.2：任意按键刷新"最近活动"时刻；若正处于空闲降亮，先恢复用户设定
    // 亮度（后续 switch 内的 brightness_apply 用的是 s_brightness，不受影响）。
    s_last_key_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        brightness_apply();
    }

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
            // 切换配色主题（0 霓虹 → 1 墨水屏 → 2 青绿 → 3 橙红 → 4 矩阵绿）
            s_theme_idx = (s_theme_idx + 1) % THEME_COUNT;
            ESP_LOGI(TAG, "切换主题: %d", s_theme_idx);
            apply_theme();
        } else if (btn == BSP_BTN_DOWN) {
            // 切换显示模式：完整 / 精简（隐藏顶栏流、频谱、底流、故障色条）
            s_mode = (s_mode + 1) % MODE_COUNT;
            ESP_LOGI(TAG, "切换模式: %d", s_mode);
            bool hide = (s_mode == MODE_MINIMAL);
            for (int i = 0; i < 12; i++) {
                (hide ? lv_obj_add_flag : lv_obj_clear_flag)(s_spec[i], LV_OBJ_FLAG_HIDDEN);
            }
            for (int i = 0; i < 3; i++) {
                // 精简模式隐藏故障色条；恢复时墨水屏仍保持隐藏
                if (hide || theme_is_eink())
                    lv_obj_add_flag(s_gbar[i], LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_clear_flag(s_gbar[i], LV_OBJ_FLAG_HIDDEN);
            }
            (hide ? lv_obj_add_flag : lv_obj_clear_flag)(s_hdr_hex, LV_OBJ_FLAG_HIDDEN);
            (hide ? lv_obj_add_flag : lv_obj_clear_flag)(s_stream2, LV_OBJ_FLAG_HIDDEN);
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
