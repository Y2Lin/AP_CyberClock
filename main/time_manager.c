// main/time_manager.c —— 系统时间管理：NVS 持久化、时区、同步状态。
#include "time_manager.h"
#include "demo_radio.h"
#include "esp_log.h"
#include "esp_system.h"   // esp_reset_reason()
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "time_mgr";

#define NVS_NAMESPACE  "cyber_clk"
#define NVS_KEY_STATE  "state"
#define NVS_KEY_TZ     "tz_offset"
#define NVS_KEY_LAST   "last_unix"

static time_manager_state_t s_state;
static bool s_initialized;

// 把状态写入 NVS。失败只打日志，不阻塞 UI。
static void persist_state(void)
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
        nvs_get_blob(h, NVS_KEY_STATE, &s_state, &len);
        int32_t tz = 0;
        if (nvs_get_i32(h, NVS_KEY_TZ, &tz) == ESP_OK && tz != 0) {
            s_state.tz_offset = tz;
        }
        // 尝试恢复上次保存的 Unix 时间作为粗略起点。
        // 注意：深度睡眠期间 RTC 定时器仍在跑，但系统时间会重置，
        // 所以这里恢复的值仅用于"未同步时显示一个大概时间"。
        int64_t last_unix = 0;
        if (nvs_get_i64(h, NVS_KEY_LAST, &last_unix) == ESP_OK && last_unix > 1700000000) {
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
    // 于是界面继续显示 SYNCED 而时间其实是错的 —— 正是本次要修的现象。
    // 只清 synced，sync_count 是历史统计，保留。
    if (s_state.synced) {
        ESP_LOGI(TAG, "复位源=%d esp_timer=%lldms：恢复的时间不可信，"
                      "synced 已清除（等待重新同步）",
                 (int)esp_reset_reason(),
                 (long long)(esp_timer_get_time() / 1000));
        s_state.synced = false;
        persist_state();
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

    s_state.synced = true;
    s_state.sync_count++;
    s_state.last_sync_ms = esp_timer_get_time() / 1000;

    // 持久化：状态 + 当前时间（供下次启动粗略恢复）。
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_STATE, &s_state, sizeof(s_state));
        nvs_set_i64(h, NVS_KEY_LAST, (int64_t)unix_seconds);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "时间已同步: UTC=%lld, 本地同步次数=%u",
             (long long)unix_seconds, s_state.sync_count);
    return ESP_OK;
}

esp_err_t time_manager_set_timezone(int32_t offset_seconds)
{
    if (!s_initialized) time_manager_init();
    s_state.tz_offset = offset_seconds;
    persist_state();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_TZ, offset_seconds);
        nvs_commit(h);
        nvs_close(h);
    }
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

// 运行期间定期把当前时间回写 NVS（建议每秒调用一次，内部自行节流）。
//
// NVS_KEY_LAST 过去只在 set_unix_utc() 里写一次，后果是：连续开机 N 小时后断电，
// 下次启动恢复出来的时间整整慢 N 小时 —— 这是"关机再打开时间就不对了"的主要放大项。
// 按 5 分钟节奏回写后，断电恢复值的偏差被压到 5 分钟以内。
// 不做每分钟是为了控制 flash 擦写次数：NVS 有磨损均衡，但每天 288 次比 1440 次稳妥得多。
void time_manager_periodic_save(void)
{
    if (!s_state.synced) return;   // 未同步时时间本就不可信，不值得写

    struct tm tm = time_manager_get_local();
    if (tm.tm_sec != 0 || tm.tm_min % 5 != 0) return;

    static int last_min = -1;
    if (tm.tm_min == last_min) return;   // 同一分钟内不重复写
    last_min = tm.tm_min;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, NVS_KEY_LAST, (int64_t)time_manager_get_unix_utc());
        nvs_commit(h);
        nvs_close(h);
    }
}

time_manager_state_t time_manager_get_state(void)
{
    return s_state;
}

bool time_manager_is_reliable(uint32_t max_age_seconds)
{
    if (!s_state.synced) return false;
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed = (now_ms - s_state.last_sync_ms) / 1000;
    return elapsed >= 0 && (uint32_t)elapsed <= max_age_seconds;
}

void time_manager_format_hms(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 9) return;
    struct tm tm = time_manager_get_local();
    snprintf(buf, buf_size, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void time_manager_format_ymd(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 11) return;
    struct tm tm = time_manager_get_local();
    snprintf(buf, buf_size, "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

void time_manager_format_weekday(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 4) return;
    static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    struct tm tm = time_manager_get_local();
    snprintf(buf, buf_size, "%s", names[tm.tm_wday]);
}
