#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// BLE 时间同步服务：让手机 / Mac 通过 BLE 连接并写入当前时间。
//
// 协议（极简设计，方便用任意 BLE 调试工具操作）：
//   Service UUID:  0000FFC0-0000-1000-8000-00805F9B34FB  (CyberClock Time Service)
//   Char Write:    0000FFC1-0000-1000-8000-00805F9B34FB
//     写入 4 字节小端 uint32 = Unix 时间戳（UTC，秒）
//     可选追加 2 字节小端 int16 = 时区偏移（小时，如 +8 = 8, -5 = -5）
//     例：同步 2026-09-01 00:00:00 UTC + 东八区 →
//         66 B4 63 67  08 00
//   Char Notify:   0000FFC2-0000-1000-8000-00805F9B34FB
//     连接后每 5 秒通知一次当前状态（JSON 风格纯文本，方便调试）：
//       {"ts":1756684800,"tz":28800,"synced":true}
//
// 使用方式（手机）：
//   1. 安装 nRF Connect / LightBlue 等 BLE 调试 App
//   2. 扫描到 "CyberClock-XXXX" 并连接
//   3. 找到 FFC0 服务，向 FFC1 写入时间戳
//   4. 设备屏幕上时间立即更新

typedef enum {
    BLE_TS_DISCONNECTED = 0,
    BLE_TS_ADVERTISING,
    BLE_TS_CONNECTED,
    BLE_TS_SYNCED,      // 本次连接内已成功收到时间
    BLE_TS_ERROR,
} ble_ts_state_t;

// 初始化并开始广播。可重复调用（内部去重）。
esp_err_t ble_time_sync_start(void);

// 停止广播、断开连接并释放 NimBLE。
void ble_time_sync_stop(void);

// 获取当前连接/同步状态。
ble_ts_state_t ble_time_sync_get_state(void);

// 获取已连接客户端的地址描述（静态缓冲区，仅用于显示）。
const char *ble_time_sync_peer_name(void);

// 注册一个回调，每次成功收到时间同步后调用（用于 UI 刷新提示）。
typedef void (*ble_ts_sync_cb_t)(void);
void ble_time_sync_set_sync_callback(ble_ts_sync_cb_t cb);
