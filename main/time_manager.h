#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

// 时间管理器：负责系统时间的设置、获取、持久化和同步状态追踪。
// ESP32-C3 没有电池供电的 RTC，深度睡眠后时间会丢失，因此需要：
//   1. 每次设置时间后写入 NVS
//   2. 启动时从 NVS 恢复时间（仅作为粗略参考，精度取决于睡眠时长）
//   3. 通过 BLE 同步后标记为已校准

typedef struct {
    bool      synced;        // 是否已通过 BLE 校准过
    int32_t   tz_offset;     // 时区偏移（秒），东八区 = 28800
    uint32_t  sync_count;    // 历史同步次数
    int64_t   last_sync_ms;  // 上次同步的系统单调时间（ms）
} time_manager_state_t;

// 初始化时间管理器：从 NVS 恢复持久化状态，不阻塞。
esp_err_t time_manager_init(void);

// 用 Unix 时间戳（秒，UTC）设置系统时间，并持久化到 NVS。
// 会自动加上 tz_offset 转换为本地时间显示。
esp_err_t time_manager_set_unix_utc(time_t unix_seconds);

// 设置时区偏移（秒），例如东八区传 28800。
esp_err_t time_manager_set_timezone(int32_t offset_seconds);

// 获取当前本地时间（已应用时区偏移）。
struct tm time_manager_get_local(void);

// 获取当前 UTC Unix 时间戳（秒）。
time_t time_manager_get_unix_utc(void);

// 运行期间定期把当前时间回写 NVS，缩小断电后恢复值的偏差。
// 可放心每秒调用：内部按 5 分钟节流；未同步时直接返回，不产生 flash 写入。
void time_manager_periodic_save(void);

// 获取当前状态的只读快照。
time_manager_state_t time_manager_get_state(void);

// 判断时间是否可信（已同步过且距上次同步不超过 max_age_seconds）。
bool time_manager_is_reliable(uint32_t max_age_seconds);

// 格式化本地时间为 "HH:MM:SS"，写入 buf（至少 9 字节）。
void time_manager_format_hms(char *buf, size_t buf_size);

// 格式化本地日期为 "YYYY-MM-DD"，写入 buf（至少 11 字节）。
void time_manager_format_ymd(char *buf, size_t buf_size);

// 格式化星期为英文缩写 "MON".."SUN"，写入 buf（至少 4 字节）。
void time_manager_format_weekday(char *buf, size_t buf_size);
