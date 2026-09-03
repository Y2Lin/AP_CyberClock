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

// 运行期间把待写状态落盘（消费 set_* 置位的 dirty 标志），并按 5 分钟
// 节流回写当前时间，缩小断电后恢复值的偏差。
// 建议每秒调用一次（LVGL 定时器上下文）：NVS 写入全部收敛到这里，
// BLE/USB 任务只更新内存态；未同步且无待写时直接返回，不产生 flash 写入。
void time_manager_flush_pending(void);

// 获取当前状态的只读快照（临界区内拷贝，多字段不会撕裂）。
time_manager_state_t time_manager_get_state(void);

// 判断时间是否可信（已同步过且距上次同步不超过 max_age_seconds）。
bool time_manager_is_reliable(uint32_t max_age_seconds);
