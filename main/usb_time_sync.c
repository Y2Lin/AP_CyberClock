// main/usb_time_sync.c —— USB 串口对时(免蓝牙方案)。
//
// 利用刷固件用的同一条 USB 线(USB Serial/JTAG)接收文本对时命令:
//   PING                 -> PONG                    (连通性/版本探测)
//   T <unix> [tz_hours]  -> OK TS=... TZ=...        (设置时间,可选时区)
//   Q                    -> {"ts":...,"tz":...,"synced":true}
//
// 实现要点:
//   - 安装 usb_serial_jtag 驱动后调用 usb_serial_jtag_vfs_use_driver(),
//     控制台日志输出改走同一驱动的 TX 缓冲,与本模块读写无冲突
//     (与官方 esp_console REPL 的初始化序列一致)。
//   - 任务阻塞在 usb_serial_jtag_read_bytes(),无轮询开销。
//   - 时钟页面每秒自行重读时间,USB 对时后 1 秒内屏幕自动更新,
//     无需跨任务回调(避免 v4 修复过的 LVGL 跨线程问题)。
#include "usb_time_sync.h"
#include "time_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "usb_ts";
static TaskHandle_t s_task;
static bool s_started;

static void handle_line(const char *line)
{
    // 去掉行尾可能的 '\r'
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", line);
    char *cr = strchr(buf, '\r');
    if (cr) *cr = '\0';

    if (strncmp(buf, "PING", 4) == 0) {
        printf("PONG\n");
        return;
    }

    if (buf[0] == 'T' && (buf[1] == ' ' || buf[1] == '\0')) {
        char *end = NULL;
        long long ts = strtoll(buf + 1, &end, 10);
        if (ts <= 0 || ts > 4102444800LL) {  // 上限:2100 年,挡住胡乱输入
            printf("ERR TS\n");
            return;
        }
        // 可选第二参数:时区小时数；省缺时回显设备当前时区（v10.3：旧版
        // 硬编码回显 TZ=8，设备实际时区可能根本不是 +8）
        long tz = 0;
        if (end && *end == ' ') {
            tz = strtol(end, NULL, 10);
            if (tz < -14 || tz > 14) {
                printf("ERR TZ\n");
                return;
            }
            time_manager_set_timezone((int32_t)tz * 3600);
        } else {
            tz = time_manager_get_state().tz_offset / 3600;
        }
        if (time_manager_set_unix_utc((time_t)ts) == ESP_OK) {
            printf("OK TS=%lld TZ=%ld\n", ts, tz);
            ESP_LOGI(TAG, "USB 对时成功: %lld tz=%ld", ts, tz);
        } else {
            printf("ERR SET\n");
        }
        return;
    }

    if (buf[0] == 'Q' && buf[1] == '\0') {
        time_manager_state_t st = time_manager_get_state();
        printf("{\"ts\":%ld,\"tz\":%ld,\"synced\":%s}\n",
               (long)time_manager_get_unix_utc(),
               (long)st.tz_offset,
               st.synced ? "true" : "false");
        return;
    }

    printf("ERR UNKNOWN\n");
}

static void usb_time_task(void *arg)
{
    (void)arg;
    char line[64];
    size_t idx = 0;

    // 就绪提示(手动用终端连接时可见)
    printf("\r\n[usb_ts] ready. cmd: T <unix> [tz] | Q | PING\r\n");

    while (1) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY);
        if (n <= 0) continue;

        if (c == '\n' || c == '\r') {
            if (idx > 0) {
                line[idx] = '\0';
                handle_line(line);
                idx = 0;
            }
        } else if (idx < sizeof(line) - 1) {
            line[idx++] = (char)c;
        } else {
            idx = 0;  // 超长行丢弃,重新同步
            printf("ERR LONG\n");
        }
    }
}

esp_err_t usb_time_sync_start(void)
{
    if (s_started) return ESP_OK;

    // 安装 USB Serial/JTAG 驱动(已装则复用),控制台输出切换到驱动路径
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(usb_time_task, "usb_ts", 4096, NULL, 5, &s_task) != pdTRUE) {
        ESP_LOGE(TAG, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "USB 串口对时已就绪");
    return ESP_OK;
}
