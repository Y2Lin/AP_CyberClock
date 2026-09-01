// main/usb_time_sync.h —— USB 串口对时(免蓝牙方案)。
#pragma once
#include "esp_err.h"

// 启动 USB 串口对时监听(常驻任务,任意页面均可同步)。
// 协议(文本行,CRLF/LF 结尾均可):
//   PING                     -> PONG
//   T <unix> [tz_hours]      -> OK TS=... TZ=...  (设置时间,可选时区)
//   Q                        -> {"ts":...,"tz":...,"synced":true}
//   其他                     -> ERR UNKNOWN
esp_err_t usb_time_sync_start(void);
