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
#include "lvgl.h"
// 本项目 LVGL 9.2 无公开的 flush getter，直接读私有结构字段
// （esp_lvgl_port 内部同样通过该字段访问）。
#include "display/lv_display_private.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "usb_ts";
static TaskHandle_t s_task;
static bool s_started;

// ============================================================================
// FAP_SCREENSHOT_V1 运行时截屏（AI Passport 社区发布助手的连接证明协议）
//
// 为什么不用 lv_snapshot：本项目 LVGL 走 64KB 内置内存池，全屏 RGB565
// 快照需要 240*320*2 = 150KB，池内必然分配失败。因此改为拦截显示刷新：
// 截屏时临时包装 flush 回调，令整屏重绘一次，把每个 20 行分块在送显
// 之前拼进系统堆分配的缓冲 —— 只借道路由，不占 LVGL 池。
//
// 线程模型：USB 任务只置请求并等待；安装/卸载拦截与累积拷贝全部发生在
// LVGL 上下文（flush 回调 = 渲染任务；service = LVGL 定时器）。标志均为
// volatile 单写者，C3 单核无缓存一致性问题。
// ============================================================================
#define SHOT_TIMEOUT_MS 3000
#define SHOT_CHUNK      1024

static volatile bool s_shot_req;        // USB 任务 -> LVGL：请求截屏
static volatile int  s_shot_state;      // 0=空闲 1=采集中 2=就绪 3=失败/中止
static volatile bool s_shot_hooked;     // flush 拦截是否仍挂着（卸载确认用）
static uint8_t      *s_shot_buf;        // 输出缓冲（USB 任务分配/释放）
static lv_display_flush_cb_t s_orig_flush;
static int  s_shot_rows;
static int  s_shot_w, s_shot_h;

// 拦截版 flush：先把本块像素（送显前、字节序交换前的原生小端 RGB565）
// 拷进累积缓冲，再交回 esp_lvgl_port 原回调。
static void shot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_shot_state == 1) {
        int h = area->y2 - area->y1 + 1;
        int w = area->x2 - area->x1 + 1;
        if (s_shot_buf && area->x1 == 0 && w == s_shot_w &&
            area->y1 + h <= s_shot_h) {
            memcpy(s_shot_buf + (size_t)area->y1 * s_shot_w * 2, px_map,
                   (size_t)w * h * 2);
            s_shot_rows += h;
        }
        if (s_shot_rows >= s_shot_h) {
            // 整屏拼完：恢复原回调并发布就绪
            lv_display_set_flush_cb(disp, s_orig_flush);
            s_shot_hooked = false;
            s_shot_state = 2;
        }
    }
    s_orig_flush(disp, area, px_map);
}

void usb_time_sync_snapshot_service(void)
{
    // 兜底卸载：采集超时等异常路径下 hook 可能仍挂着（state 已非 1），
    // 若不卸载，下一次截屏会把 shot_flush_cb 自己误存为"原回调"，
    // 完成时将造成无限递归。正常路径 state==1 时不动。
    if (s_shot_hooked && s_shot_state != 1) {
        lv_display_t *d = lv_display_get_default();
        if (d) lv_display_set_flush_cb(d, s_orig_flush);
        s_shot_hooked = false;
    }
    if (!s_shot_req) return;
    s_shot_req = false;

    lv_display_t *disp = lv_display_get_default();
    if (!disp || !s_shot_buf) { s_shot_state = 3; return; }

    s_shot_w = (int)lv_display_get_horizontal_resolution(disp);
    s_shot_h = (int)lv_display_get_vertical_resolution(disp);

    s_orig_flush = disp->flush_cb;
    s_shot_rows = 0;
    s_shot_state = 1;
    s_shot_hooked = true;
    lv_display_set_flush_cb(disp, shot_flush_cb);
    // 整屏失效：下一轮渲染会按 20 行分块完整走一遍 flush，截屏由此完成
    lv_obj_invalidate(lv_screen_active());
}

// USB 任务侧：发起请求并等待结果。成功时 *buf/*len 指向发送方随后要读的
// 缓冲（用完调 shot_release）。缓冲本身由本函数在系统堆上分配。
static bool shot_request(uint8_t **buf, size_t *len, int *w, int *h)
{
    if (s_shot_state == 1) return false;             // 上一单还没结束
    s_shot_buf = malloc((size_t)240 * 320 * 2);
    if (!s_shot_buf) { s_shot_state = 3; return false; }

    s_shot_state = 0;
    s_shot_req = true;                               // service() 在 LVGL 上下文消费
    for (int waited = 0; waited < SHOT_TIMEOUT_MS && s_shot_state == 0; waited += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    for (int waited = 0; waited < SHOT_TIMEOUT_MS && s_shot_state == 1; waited += 10)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (s_shot_state == 2 && s_shot_buf) {
        *buf = s_shot_buf; *len = (size_t)s_shot_w * s_shot_h * 2;
        *w = s_shot_w; *h = s_shot_h;
        return true;
    }
    // 失败：先置 3 关门（flush 回调只认 1 才碰缓冲），避免 free 与
    // s_shot_buf=NULL 之间被渲染任务抢占读到悬垂指针；hook 由 service()
    // 兜底卸载（见函数头），这里只等一小会儿再释放。
    s_shot_state = 3;
    for (int waited = 0; waited < 500 && s_shot_hooked; waited += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    free(s_shot_buf); s_shot_buf = NULL;
    s_shot_state = 0;
    return false;
}

// 发送完成后释放缓冲。若拦截仍挂着（异常路径），由 service 兜底卸载：
// 这里只负责 free —— 为安全起见等 hook 卸下后再释放。
static void shot_release(void)
{
    for (int waited = 0; waited < 500 && s_shot_hooked; waited += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    free(s_shot_buf); s_shot_buf = NULL;
    s_shot_state = 0;
}

static void handle_line(const char *line)
{
    // 去掉行尾可能的 '\r'
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", line);
    char *cr = strchr(buf, '\r');
    if (cr) *cr = '\0';

    // ---- FAP_SCREENSHOT_V1：返回运行中的屏幕画面（纯观测，不改任何状态）----
    if (strncmp(buf, "FAP_SCREENSHOT_V1", 17) == 0 &&
        (buf[17] == '\0' || buf[17] == ' ')) {
        uint8_t *data; size_t len; int w, h;
        if (!shot_request(&data, &len, &w, &h)) {
            printf("ERR SHOT\n");
            return;
        }
        // 先全局静默日志再打印头，消除"头与像素流之间被日志插入"的窗口
        esp_log_level_set("*", ESP_LOG_NONE);
        printf("FAP_SCREENSHOT_V1 %d %d RGB565LE %u\n",
               w, h, (unsigned)len);
        fflush(stdout);
        const uint8_t *p = data; size_t left = len;
        while (left > 0) {
            size_t chunk = left > SHOT_CHUNK ? SHOT_CHUNK : left;
            int n = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(1000));
            if (n <= 0) break;
            p += n; left -= n;
        }
        esp_log_level_set("*", ESP_LOG_INFO);
        shot_release();
        return;
    }

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
