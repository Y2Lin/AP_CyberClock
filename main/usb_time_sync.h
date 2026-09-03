// main/usb_time_sync.h —— USB 串口对时(免蓝牙方案)。
#pragma once
#include "esp_err.h"
#include <stdbool.h>

// 启动 USB 串口对时监听(常驻任务,任意页面均可同步)。
// 协议(文本行,CRLF/LF 结尾均可):
//   PING                     -> PONG
//   T <unix> [tz_hours]      -> OK TS=... TZ=...  (设置时间,可选时区)
//   Q                        -> {"ts":...,"tz":...,"synced":true}
//   FAP_SCREENSHOT_V1        -> FAP_SCREENSHOT_V1 <w> <h> RGB565LE <len>\n<二进制>
//   其他                     -> ERR UNKNOWN
esp_err_t usb_time_sync_start(void);

// 截屏服务钩子：须在 LVGL 定时器上下文（已持 bsp_lvgl 锁）每帧调用，
// 内部无请求时仅读一个标志即返回。当 USB 任务收到 FAP_SCREENSHOT_V1 后，
// 这里负责安装显示刷新拦截并把整屏重绘结果累积到请求方提供的缓冲。
// （发布助手要求的 FAP_SCREENSHOT_V1 观测协议，v10.4）
void usb_time_sync_snapshot_service(void);
