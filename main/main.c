// main/main.c —— FoloToy AI Passport 启动入口 + 按键转发。
//
// v8 起开机直进 CyberClock 应用（同步页→表盘→菜单的页面状态机由
// cyber_clock.c 自管）。v10.3 移除了不再可达的上游 demo 菜单与其余演示页
// 入口表——未被引用的 demo_* 代码由链接器 --gc-sections 自动裁剪，
// 同时消除了 enter_menu 的 -Wunused-function 告警（审查 P3-1）。
//
// 按键语义（全局统一，由应用自行解释）：
//   上/下 短按   页面自定义
//   确定  短按   页面自定义
//   确定  长按   页面自定义（全局"上一层"导航）
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "time_manager.h"
#include "usb_time_sync.h"

static const char *TAG = "main";

// 唯一应用：CyberClock（demo.h 中其余 demo_* 声明保留，但不再有入口）
static const demo_entry_t DEMOS[] = {
    { "CyberClock", demo_cyber_clock_enter, demo_cyber_clock_exit, demo_cyber_clock_key },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))
_Static_assert(DEMO_COUNT == 1, "此入口只服务 CyberClock；新增应用需恢复菜单");

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    DEMOS[0].key(btn, ev);   // v8 起唯一应用：按键直接转发，导航由应用内状态机处理

    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport BSP demo 启动");

    // v10.2 功耗：动态调频（DFS）——忙时 80MHz、空闲降到 40MHz（默认主频已是 80）。
    // SPI/I2C 等驱动在 CONFIG_PM_ENABLE 下自动持有 PM 锁，传输期间不会被降频打断。
    // 不开 light sleep：USB-Serial-JTAG 控制台与 USB 对时在睡眠期间不可用。
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    ESP_LOGI(TAG, "电源管理 DFS 40-80MHz: %s",
             pm_err == ESP_OK ? "已启用" : esp_err_to_name(pm_err));

    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 时间管理 + USB 串口对时(常驻,任意页面可用,不依赖蓝牙)
    time_manager_init();
    usb_time_sync_start();

    // 屏幕是本应用的 UI 载体，失败就没有可用界面 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 其余外设单项失败不阻塞（v10.3 随菜单清理简化了假状态数组）：
    //   按键失败 → 无输入，仅显示；电池失败 → 表盘回落 50%/3700mV 占位值
    bool btn_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    bool bat_ok = (bsp_battery_init() == ESP_OK);

    // v8：开机直进 CyberClock 应用首屏（时间同步页）。设备无 RTC 电池，
    // 开机时间必然不可信，先进同步页对时是自洽的动线（USB/蓝牙任一同步后
    // 按 OK 进表盘）。
    if (bsp_lvgl_lock(1000)) { DEMOS[0].enter(); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "就绪: Clock=1 Button=%d Battery=%d", btn_ok, bat_ok);
}
