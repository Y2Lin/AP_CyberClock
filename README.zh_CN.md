<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AP_CyberClock

基于 [folotoy/ai-passport](https://github.com/folotoy/ai-passport)（MIT License）开发的**赛博朋克风格时钟**固件，运行于 FoloToy AI Passport 硬件（ESP32-C3）。

支持 **BLE 蓝牙对时** 与 **USB 线对时** 双通道，蓝牙只属于时间同步页（表盘零射频功耗），无需配套 App——手机/Mac 浏览器网页或任意 BLE 调试工具即可完成时间同步。

![固件](https://img.shields.io/badge/firmware-v9-00f0ff) ![芯片](https://img.shields.io/badge/chip-ESP32--C3-ff2ec4) ![license](https://img.shields.io/badge/license-MIT-green)

## 页面动线（v9）

```
开机 → [TIME SYNC 时间同步页] --按 OK 进表盘（蓝牙关闭）--> [表盘页]
        （蓝牙仅此页开启，                          OK 长按 ↓
         同步后停留本页）                               [菜单页]
                                          1 表盘 | 2 亮度 | 3 时间同步
```

## 功能特性

### 赛博朋克时钟 UI（240×320）
- HUD 风格布局：四角 L 形角标、侧边刻度线、分割线装饰
- 大字号 `HH:MM` 带错位霓虹残影 + 右侧独立秒数 `SS` + 秒数垂直进度柱（v9）
- 日期 + 星期 + 时区 + 运行时长
- 底部电池条 + 百分比 + 电压，低电量红色告警
- **4 套主题**：霓虹青紫 / 青绿单色 / 橙红暖色 / 矩阵绿
- **2 种显示模式**：完整信息 / 精简（仅时间+日期+电池）

### BLE 蓝牙对时
- 设备作为 BLE Peripheral 广播，名称 `CyberClock`
- 自定义 GATT 服务（FFC0），写入 Unix 时间戳即可对时，支持时区
- 连接后每 5 秒通知当前时间状态（JSON）
- 支持重复配对（自动清除旧绑定），兼容 nRF Connect / LightBlue / Web Bluetooth

### USB 线对时（v5+，免蓝牙）
- 复用刷固件的 USB 数据线，走 ESP32-C3 USB Serial/JTAG
- **任意页面可用**，不依赖所在页面
- 文本命令协议：`PING` / `T <unix> <tz>` / `Q`
- 浏览器直连（Chrome/Edge Web Serial）或 `usb-sync.py` 命令行脚本

### 蓝牙只属于时间同步页（v8/v9）
- 进入 TIME SYNC 同步页（开机首屏）即开始广播，离开该页即关闭蓝牙——表盘页完全不碰射频
- 同步页屏幕显示 `BT: ADVERTISING / BT: LINKED`
- **同步成功后设备停留在同步页**（v9：不自动跳转）——连接保持、网页可持续收到状态通知；按 OK 进表盘时蓝牙随之关闭
- 需要再同步：长按 OK → 菜单 → `3 TIME SYNC`

### 亮度调整（v8）
- 亮度页 UP/DOWN 以 10% 步进调背光（10~100%）
- 持久化到 NVS，重启自动恢复

### 其他
- 时间与时区持久化到 NVS，重启恢复粗略时间
- LVGL 跨线程安全设计（BLE 回调仅置标志，UI 刷新全部在 LVGL 定时器上下文）

## 按键操作（v9 按页面分发）

| 页面 | 按键 | 动作 | 功能 |
|---|---|---|---|
| 时间同步页（开机首屏） | OK | 短按 | 进入表盘（同步后停留本页，不自动跳转） |
| | OK | 长按 | 进入菜单 |
| 表盘页 | UP | 短按 | 切换配色主题（蓝紫 → 青绿 → 橙红 → 矩阵绿） |
| | DOWN | 短按 | 切换显示模式（完整 ⇄ 精简） |
| | OK | 长按 | 进入菜单 |
| 菜单页 | UP / DOWN | 短按 | 移动选中项（1 表盘 / 2 亮度 / 3 时间同步） |
| | OK | 短按 | 进入选中项；长按返回表盘 |
| 亮度页 | UP / DOWN | 短按 | 亮度 +10% / -10%（10~100%，即时保存） |
| | OK | 短按/长按 | 返回菜单 |

## 快速开始

### 1. 刷固件
预编译固件（v9）在 [`tools/cyber-clock-sync/firmware/`](tools/cyber-clock-sync/firmware/)：
- 浏览器刷写（免安装）：Chrome/Edge 打开官方刷机页——FoloToy [官方刷机页](https://ai-passport.folotoy.cn/tools/web-flasher/)（厂商首选）或 [esptool-js](https://espressif.github.io/esptool-js/)（芯片厂商通用版），连接设备后添加合并固件、地址填 `0x0` 即可——固件直链下载：<https://y2lin.github.io/AP_CyberClock/firmware/FoloToy-AI-Passport-full.bin>
- 或 esptool：`esptool --chip esp32c3 write_flash 0x0 FoloToy-AI-Passport-full.bin`

详见 [烧录指南](tools/cyber-clock-sync/烧录指南.md)。

### 2. 对时（三选一）
- **USB 线（最简单）**：Chrome/Edge 打开 `index.html`（本地或在线版，见下）→ "USB 线同步"标签页 → 连接 → 自动写入
- **手机蓝牙**：nRF Connect / LightBlue 连接 `CyberClock`，向 FFC1 写入时间戳
- **Mac 蓝牙**：`index.html` 蓝牙标签页，或 `mac-sync.py` 脚本

同步协议、手动验证方法与故障排除见 [时钟功能与BLE同步说明](tools/cyber-clock-sync/时钟功能与BLE同步说明.md)。

### 3. 在线对时页（GitHub Pages）

对时页在每次 push 到 `main` 后自动部署，就是 Pages 站点首页：

- <https://y2lin.github.io/AP_CyberClock/>

Web Bluetooth / Web Serial 要求 HTTPS 安全上下文，GitHub Pages 天然满足；离线单文件版也继续可用。注意：`github.io` 在中国大陆可能无法直接访问，此时请用本地文件。

## 目录结构

```
main/                          固件源码
├── cyber_clock.c/h            赛博朋克时钟：四页面状态机（同步/表盘/菜单/亮度）
├── ble_time_sync.c/h           BLE GATT 时间同步服务
├── usb_time_sync.c/h           USB Serial/JTAG 文本命令对时
├── time_manager.c/h            时间管理：NVS 持久化、时区、同步状态
└── (demo_* / bsp_* 为上游原有文件)
tools/
├── cyber-clock-sync/           对时工具包（同时是 Pages 站点根目录）
│   ├── index.html              双通道对时页（蓝牙 + Web Serial），即站点首页
│   ├── usb-sync.py             USB 命令行同步脚本（pyserial）
│   ├── mac-sync.py             Mac 系统蓝牙同步脚本（bleak）
│   ├── firmware/               预编译固件（bootloader/分区表/应用/合并）
│   └── *.md                    中文文档：烧录指南、功能与同步说明、构建信息
└── validate.sh 等              上游原有构建校验工具
.github/workflows/pages.yml     Pages 自动部署（发布 tools/cyber-clock-sync）
```

## 从源码构建

```bash
git clone https://github.com/Y2Lin/AP_CyberClock.git
cd AP_CyberClock

# ESP-IDF v5.5.3
source ~/esp/v5.5.3/export.sh
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

- 8MB Flash，分区表 `partitions.csv`；应用分区上限 3MB
- **不要修改** `cardid@0x356000` 与 `recovery@0x700000` 分区（小程序 BLE Recovery 依赖）
- 干净构建校验：`./tools/validate.sh --firmware`（构建环境与 SHA256 见 `tools/cyber-clock-sync/构建信息.md`）

## 对时协议速查

**BLE**（Service `FFC0`，Write `FFC1`，Notify `FFC2`）：向 FFC1 写 6 字节小端 = `uint32 Unix 时间戳 + int16 时区小时`。例如 `2026-09-01 00:00:00 UTC` + 东八区 → 时间戳 `1788220800` → `80 15 96 6A 08 00`。

**USB**（串口文本，换行结束）：

| 发送 | 回复 |
|---|---|
| `PING` | `PONG` |
| `T 1788220800 8` | `OK TS=1788220800 TZ=8` |
| `Q` | `{"ts":1788220800,"tz":28800,"synced":true}` |

## GitHub Pages 自动部署

`.github/workflows/pages.yml` 在 `tools/cyber-clock-sync/` 内容变化（或手动触发）时，把该目录（对时页即站点首页、固件、文档）自动发布到 GitHub Pages。一次性设置：仓库 Settings → Pages，**Source 选择 "GitHub Actions"**。

## 致谢与许可

- 基于 [folotoy/ai-passport](https://github.com/folotoy/ai-passport) 开发，保留其完整提交历史以示致谢
- 上游及本项目均采用 [MIT License](LICENSE)
