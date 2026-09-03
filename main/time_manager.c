// main/time_manager.c —— 系统时间管理：NVS 持久化、时区、同步状态。
#include "time_manager.h"
#include "demo_radio.h"
#include "esp_log.h"
#include "esp_system.h"   // esp_reset_reason()
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"   // portMUX（状态快照临界区）
#include <string.h>
#include <sys/time.h>

static const char *TAG = "time_mgr";

#define NVS_NAMESPACE  "cyber_clk"
#define NVS_KEY_STATE  "state"
#define NVS_KEY_TZ     "tz_offset"
#define NVS_KEY_LAST   "last_unix"

// 视为合法的时间戳下限：2023-11-14，防止 NVS 脏数据把时钟拨回 1970 附近
#define TIME_MIN_VALID_UNIX 1700000000LL

// 时区偏移的最终防线：UTC-14 .. UTC+14。
// USB 入口自带 ±14 校验；BLE 入口此前完全没有校验（int16 全范围可写穿，
// 屏幕会显示公元前后），v10.3 起在这里统一拦截。
#define TZ_MIN_SECONDS (-14 * 3600)
#define TZ_MAX_SECONDS ( 14 * 3600)

static time_manager_state_t s_state;
static bool s_initialized;

// ---- 延迟持久化（v10.3，对应审查 P3-2）----
// set_unix_utc / set_timezone 可能运行在 NimBLE host 任务或 USB 任务里，
// 旧版直接在这些回调里 nvs_commit：flash 擦除可达上百毫秒，会把协议栈
// 任务卡住——与 v4 确立的"回调只置标志"原则相悖（当时只对 LVGL 成立）。
// 现在 NVS 写入统一挪到 LVGL 定时器上下文：
//   - set_* 只更新内存态并置 dirty 标志（volatile；调用方即生产者）
//   - flush_pending() 每秒被调用一次（cyber_clock tick），有 dirty 才写
// 掉电窗口：同步后 1 秒内断电会丢这次记录——重启后本就要重新对时，可接受。
static volatile bool s_dirty_state;   // state blob 待写
static volatile bool s_dirty_tz;      // tz_offset 键待写
static volatile bool s_dirty_last;    // last_unix 键待写

// ---- 状态快照的并发保护（v10.3，对应审查 P3-5）----
// s_state 有两个写任务（BLE host / USB）与一个读任务（LVGL 定时器按值
// 拷贝整个结构体）。RV32 上单个对齐字段读写原子，但多字段快照可能撕裂
// 出半新半旧的组合。写路径与 get_state() 都包在极短临界区内，彻底杜绝。
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

// 立即写 state blob。仅限进程级上下文（init 的开机清除路径）使用；
// 运行期一律走 dirty 标志 + flush_pending()。
static void persist_state_now(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_STATE, &s_state, sizeof(s_state));
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t time_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    // NVS 是时间持久化的前提；demo_radio_nvs_prepare 不会擦除用户数据。
    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 不可用，时间无法持久化: %s", esp_err_to_name(err));
        // 即使 NVS 失败也继续，至少内存中的时间可用。
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.tz_offset = 28800;  // 默认东八区

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_state);
        // 返回值检查（v10.3）：首次开机 blob 不存在属正常路径，只打调试日志；
        // 未来若扩展 state 结构导致长度不符，这里同样静默回落默认值。
        esp_err_t bl = nvs_get_blob(h, NVS_KEY_STATE, &s_state, &len);
        if (bl != ESP_OK) {
            ESP_LOGD(TAG, "state blob 未读取（首次开机或结构变更）: %s",
                     esp_err_to_name(bl));
        }
        int32_t tz = 0;
        if (nvs_get_i32(h, NVS_KEY_TZ, &tz) == ESP_OK && tz != 0) {
            s_state.tz_offset = tz;
        }
        // 尝试恢复上次保存的 Unix 时间作为粗略起点。
        // 注意：深度睡眠期间 RTC 定时器仍在跑，但系统时间会重置，
        // 所以这里恢复的值仅用于"未同步时显示一个大概时间"。
        int64_t last_unix = 0;
        if (nvs_get_i64(h, NVS_KEY_LAST, &last_unix) == ESP_OK &&
            last_unix > TIME_MIN_VALID_UNIX) {
            struct timeval tv = { .tv_sec = (time_t)last_unix, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "从 NVS 恢复粗略时间: %lld", (long long)last_unix);
        }
        nvs_close(h);
    }

    // 开机一律回到"未对时"。
    //
    // 本机没有给 RTC 供电的纽扣电池，断电/深度睡眠期间根本无法走时，
    // 上面从 NVS 恢复出来的只是"上次保存那一刻"的旧值，偏差等于关机时长，
    // 本质上不可信。所以无论复位源是上电、深度睡眠唤醒、看门狗还是软件重启，
    // 这里都无条件清除 synced，交给 BLE / USB 重新校准。
    //
    // 旧实现靠 "last_sync_ms > esp_timer_get_time()" 判断单调钟是否回退来识别重启；
    // 这条判据依赖 esp_timer 在复位后归零，在部分唤醒路径下并不成立，
    // 于是界面继续显示 SYNCED 而时间其实是错的 —— v10.1 修掉的现象。
    // 只清 synced，sync_count 是历史统计，保留。
    if (s_state.synced) {
        ESP_LOGI(TAG, "复位源=%d esp_timer=%lldms：恢复的时间不可信，"
                      "synced 已清除（等待重新同步）",
                 (int)esp_reset_reason(),
                 (long long)(esp_timer_get_time() / 1000));
        s_state.synced = false;
        persist_state_now();
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成: synced=%d tz=%ld sync_count=%u",
             s_state.synced, (long)s_state.tz_offset, s_state.sync_count);
    return ESP_OK;
}

esp_err_t time_manager_set_unix_utc(time_t unix_seconds)
{
    if (!s_initialized) time_manager_init();

    struct timeval tv = { .tv_sec = unix_seconds, .tv_usec = 0 };
    esp_err_t err = settimeofday(&tv, NULL);
    if (err != 0) {
        ESP_LOGE(TAG, "settimeofday 失败: %d", err);
        return ESP_FAIL;
    }

    // 只更新内存态（临界区内仅字段赋值），NVS 写入交给 flush_pending()
    int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_state_mux);
    s_state.synced = true;
    s_state.sync_count++;
    s_state.last_sync_ms = now_ms;
    portEXIT_CRITICAL(&s_state_mux);
    s_dirty_state = true;
    s_dirty_last = true;   // last_unix 也要尽快落盘，不能等 5 分钟节流

    ESP_LOGI(TAG, "时间已同步: UTC=%lld, 本地同步次数=%u",
             (long long)unix_seconds, s_state.sync_count);
    return ESP_OK;
}

esp_err_t time_manager_set_timezone(int32_t offset_seconds)
{
    if (!s_initialized) time_manager_init();
    // 范围校验（v10.3，P3-3）：BLE 入口此前无任何校验；这里是最终防线
    if (offset_seconds < TZ_MIN_SECONDS || offset_seconds > TZ_MAX_SECONDS) {
        ESP_LOGW(TAG, "时区偏移越界被拒绝: %ld 秒（允许 %d..%d）",
                 (long)offset_seconds, TZ_MIN_SECONDS, TZ_MAX_SECONDS);
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_mux);
    s_state.tz_offset = offset_seconds;
    portEXIT_CRITICAL(&s_state_mux);
    // 只写 tz 键即可：读路径 blob 先读、tz 键覆盖，blob 里的旧值不会生效
    // （v10.3，P4-7：旧版 blob + 键双写同一信息属冗余）
    s_dirty_tz = true;
    return ESP_OK;
}

time_t time_manager_get_unix_utc(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

struct tm time_manager_get_local(void)
{
    time_t now = time_manager_get_unix_utc() + s_state.tz_offset;
    struct tm tm;
    gmtime_r(&now, &tm);  // 用 gmtime_r 处理已加偏移的时间，避免系统 TZ 干扰
    return tm;
}

// 运行期间定期把待写状态与当前时间落盘（建议每秒调用一次，内部自行节流）。
//
// 职责（v10.3 起合并了旧 periodic_save）：
//   1. 把 set_unix_utc / set_timezone 置位的 dirty 标志消费掉——NVS 写入
//      从 BLE/USB 任务上下文挪到了这里（LVGL 定时器，已持锁）；
//   2. 每 5 分钟回写一次 last_unix：旧版只在同步瞬间写一次，开机 N 小时后
//      断电，恢复值慢 N 小时；回写后偏差被压到 5 分钟以内。
//      不做每分钟是为了控制 flash 擦写：每天 288 次比 1440 次稳妥得多。
void time_manager_flush_pending(void)
{
    if (!s_initialized) return;

    // 摘下 dirty 位（本函数只在 LVGL 定时器上下文被调，单消费者）
    bool d_state = s_dirty_state; s_dirty_state = false;
    bool d_tz    = s_dirty_tz;    s_dirty_tz    = false;
    bool d_last  = s_dirty_last;  s_dirty_last  = false;

    // 5 分钟节流的 last_unix 回写（仅同步状态下才有意义）
    struct tm tm = time_manager_get_local();
    bool periodic = s_state.synced && tm.tm_sec == 0 && tm.tm_min % 5 == 0;
    static int last_min = -1;
    if (periodic && tm.tm_min == last_min) periodic = false;  // 同分钟不重写
    else if (periodic) last_min = tm.tm_min;

    if (!d_state && !d_tz && !d_last && !periodic) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        // 打开失败：dirty 放回去，下一秒重试（时间戳类不重试，等下个节流点）
        if (d_state) s_dirty_state = true;
        if (d_tz)    s_dirty_tz    = true;
        if (d_last)  s_dirty_last  = true;
        return;
    }
    if (d_state)
        nvs_set_blob(h, NVS_KEY_STATE, &s_state, sizeof(s_state));
    if (d_tz)
        nvs_set_i32(h, NVS_KEY_TZ, s_state.tz_offset);
    if (d_last || periodic)
        nvs_set_i64(h, NVS_KEY_LAST, (int64_t)time_manager_get_unix_utc());
    nvs_commit(h);
    nvs_close(h);
}

time_manager_state_t time_manager_get_state(void)
{
    time_manager_state_t snap;
    portENTER_CRITICAL(&s_state_mux);
    snap = s_state;
    portEXIT_CRITICAL(&s_state_mux);
    return snap;
}

bool time_manager_is_reliable(uint32_t max_age_seconds)
{
    time_manager_state_t st = time_manager_get_state();
    if (!st.synced) return false;
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed = (now_ms - st.last_sync_ms) / 1000;
    return elapsed >= 0 && (uint32_t)elapsed <= max_age_seconds;
}
