// main/usb_time_sync.c —— USB 串口对时(免蓝牙方案)。
//
// 利用刷固件用的同一条 USB 线(USB Serial/JTAG)接收文本对时命令:
//   PING                 -> PONG                    (连通性/版本探测)
//   T <unix> [tz_hours]  -> OK TS=... TZ=...        (设置时间,可选时区)
//   Q                    -> {"ts":...,"tz":...,"synced":true}
//   FAP_SCREENSHOT_V1    -> FAP_SCREENSHOT_V1 <w> <h> RGB565LE <len>\n<二进制>
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
// v10.4 首版在系统堆一次性分配 240*320*2 = 150KB 整屏缓冲，真机上最大
// 连续空闲块给不出来，shot_request 必然 ERR SHOT。v10.4.1 改为环形分块
// 流式：拦截显示刷新回调，把每个 20 行分块（送显前、字节序交换前的原生
// 小端 RGB565）拷进 3 格环形槽（每格 240*24*2 = 11.5KB，共 34.5KB），
// USB 任务边收边发——峰值内存只有旧方案的 1/4.4。
//
// 线程模型（C3 单核，volatile 单写者标志即可）：
//   USB 任务  = 消费者：请求挂钩 → 打印头 → 按序排空环形槽 → 判完成
//   service() = LVGL 定时器上下文：换 flush 回调 + 整屏失效（cyber_clock
//               的 tick() 每拍调用，无请求时只读一个标志）
//   flush 回调 = 渲染任务（= 持有 LVGL 锁的上下文）：产出一格 → 置满标志；
//               环满时小睡让 USB 任务排空（背压），整屏产完自动摘钩
// 状态机 s_shot_state：0 空闲 / 1 已挂钩待首块 / 2 流式中 / 3 完成 /
// 4 失败中止。任一侧失败置 s_shot_abort，对侧在等待点看到即退出并摘钩。
// ============================================================================
#define SHOT_SLOT_W     240                                // 面板宽（超出即失败）
#define SHOT_SLOT_ROWS  24                                 // 每格容纳行数（绘制缓冲 20 行，留余量）
#define SHOT_SLOTS      3
#define SHOT_SLOT_BYTES ((size_t)SHOT_SLOT_W * SHOT_SLOT_ROWS * 2)   // 11520
#define SHOT_RING_BYTES (SHOT_SLOT_BYTES * SHOT_SLOTS)               // 34560
#define SHOT_USB_PIECE  2048                               // 单次 USB 写入上限

static volatile bool s_shot_req;          // USB 任务 -> service：请求截屏
static volatile int  s_shot_state;        // 见上方状态机
static volatile bool s_shot_hooked;       // flush 拦截是否仍挂着
static volatile bool s_shot_abort;        // 任一侧失败：通知对侧停止
static uint8_t      *s_shot_ring;         // 环形缓冲（USB 任务分配/释放）
static volatile int  s_slot_len[SHOT_SLOTS];
static volatile bool s_slot_filled[SHOT_SLOTS];
static volatile int  s_widx, s_ridx;      // 生产/消费计数（取模得槽号）
static lv_display_flush_cb_t s_orig_flush;
static volatile int  s_shot_rows;
static volatile int  s_shot_w, s_shot_h;

static void shot_unhook(lv_display_t *disp)
{
    lv_display_set_flush_cb(disp, s_orig_flush);
    s_shot_hooked = false;
}

// 拦截版 flush：把本块像素拷进下一格空槽，再交回 esp_lvgl_port 原回调。
static void shot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_shot_state == 1 || s_shot_state == 2) {
        int h = area->y2 - area->y1 + 1;
        int w = area->x2 - area->x1 + 1;
        if (s_shot_ring && area->x1 == 0 && w == s_shot_w &&
            area->y1 >= 0 && area->y1 + h <= s_shot_h &&
            (size_t)w * h * 2 <= SHOT_SLOT_BYTES) {
            int slot = s_widx % SHOT_SLOTS;
            // 背压：环满时小睡等 USB 任务排空（上限 2s，超时/中止即失败）
            int waited = 0;
            while (s_slot_filled[slot] && !s_shot_abort) {
                if (++waited > 200) { s_shot_state = 4; break; }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (s_shot_abort) s_shot_state = 4;
            if (s_shot_state == 1 || s_shot_state == 2) {
                memcpy(s_shot_ring + (size_t)slot * SHOT_SLOT_BYTES, px_map,
                       (size_t)w * h * 2);
                s_slot_len[slot] = w * h * 2;
                s_slot_filled[slot] = true;
                s_widx++;
                s_shot_rows += h;
                if (s_shot_state == 1) s_shot_state = 2;   // 首块就绪，USB 可发头
                if (s_shot_rows >= s_shot_h) {
                    // 整屏产完：摘钩收工（消费者继续排空剩余满槽）
                    shot_unhook(disp);
                    s_shot_state = 3;
                }
            }
        }
        // 非整宽分块（局部重绘）不采集：整屏失效后的重绘按全宽条带来，
        // 这里只可能是采集前后的零星局部刷新，跳过即可。
    }
    s_orig_flush(disp, area, px_map);
}

void usb_time_sync_snapshot_service(void)
{
    // 兜底卸载：失败/中止路径下钩子可能仍挂着，若不摘，下一次截屏会把
    // shot_flush_cb 自己误存为"原回调"造成无限递归。活跃态(1/2)不动。
    if (s_shot_hooked && s_shot_state != 1 && s_shot_state != 2) {
        lv_display_t *d = lv_display_get_default();
        if (d) shot_unhook(d);
    }
    if (!s_shot_req) return;
    s_shot_req = false;

    lv_display_t *disp = lv_display_get_default();
    if (!disp || !s_shot_ring) { s_shot_state = 4; return; }

    s_shot_w = (int)lv_display_get_horizontal_resolution(disp);
    s_shot_h = (int)lv_display_get_vertical_resolution(disp);
    if (s_shot_w > SHOT_SLOT_W || s_shot_w <= 0 || s_shot_h <= 0) {
        s_shot_state = 4;
        return;
    }

    s_orig_flush = disp->flush_cb;
    s_shot_rows = 0;
    s_shot_state = 1;
    s_shot_hooked = true;
    lv_display_set_flush_cb(disp, shot_flush_cb);
    // 整屏失效：下一轮渲染会按全宽条带完整走一遍 flush，截屏由此完成
    lv_obj_invalidate(lv_screen_active());
}

// USB 任务侧：请求 → 打印头 → 排空环形槽。成功返回 true。
static bool shot_stream(void)
{
    if (s_shot_state == 1 || s_shot_state == 2) return false;   // 上一单未结束

    uint8_t *ring = malloc(SHOT_RING_BYTES);                    // 34.5KB，一次成功
    if (!ring) { s_shot_state = 4; return false; }
    s_shot_ring = ring;
    for (int i = 0; i < SHOT_SLOTS; i++) { s_slot_filled[i] = false; s_slot_len[i] = 0; }
    s_widx = s_ridx = 0;
    s_shot_abort = false;
    s_shot_state = 0;
    s_shot_req = true;                                          // service() 消费

    // 等 service 挂钩（LVGL 定时器节拍 100/250ms）
    for (int t = 0; t < 300 && s_shot_state == 0; t += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    // 等首块数据（挂钩后一次刷新周期内即有）
    for (int t = 0; t < 300 && s_shot_state == 1; t += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_shot_state != 2) goto fail;

    {
        int w = s_shot_w, h = s_shot_h;
        size_t expected = (size_t)w * h * 2;
        size_t sent = 0;
        bool ok = true;

        // 先全局静默日志再打印头，保证头与像素流之间不混入控制台字节
        esp_log_level_set("*", ESP_LOG_NONE);
        printf("FAP_SCREENSHOT_V1 %d %d RGB565LE %u\n", w, h, (unsigned)expected);
        fflush(stdout);

        while (sent < expected) {
            int slot = s_ridx % SHOT_SLOTS;
            int t = 0;
            while (!s_slot_filled[slot]) {
                if (s_shot_state == 4 || s_shot_state == 0) { ok = false; break; }
                if (s_shot_state == 3 && s_ridx >= s_widx) { ok = false; break; }
                if (++t > 300) { ok = false; break; }            // 3s 无新块
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (!ok) break;
            const uint8_t *p = ring + (size_t)slot * SHOT_SLOT_BYTES;
            size_t left = (size_t)s_slot_len[slot];
            while (left > 0) {
                size_t piece = left > SHOT_USB_PIECE ? SHOT_USB_PIECE : left;
                int n = usb_serial_jtag_write_bytes(p, piece, pdMS_TO_TICKS(1000));
                if (n <= 0) { ok = false; break; }               // 主机断开等
                p += n; left -= (size_t)n; sent += (size_t)n;
            }
            s_slot_filled[slot] = false;                         // 归还生产者
            s_ridx++;
            if (!ok) break;
        }
        esp_log_level_set("*", ESP_LOG_INFO);
        if (!ok) s_shot_abort = true;
        // 等渲染侧摘钩（正常在最后一格产出时就已摘掉）
        for (int t = 0; t < 150 && s_shot_hooked; t += 10)
            vTaskDelay(pdMS_TO_TICKS(10));
        free(ring);
        s_shot_ring = NULL;
        bool success = ok && sent == expected && s_shot_state == 3;
        s_shot_state = 0;
        s_shot_abort = false;
        return success;
    }

fail:
    s_shot_abort = true;
    for (int t = 0; t < 150 && s_shot_hooked; t += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    free(ring);
    s_shot_ring = NULL;
    s_shot_state = 0;
    s_shot_abort = false;
    return false;
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
        if (!shot_stream()) printf("ERR SHOT\n");
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
