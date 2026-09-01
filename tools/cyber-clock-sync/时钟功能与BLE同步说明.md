# 赛博朋克时钟 (Cyberpunk Clock) for FoloToy AI Passport

基于 [folotoy/ai-passport](https://github.com/folotoy/ai-passport) 项目开发的赛博朋克风格时钟固件，支持通过 BLE 与手机 / Mac 进行时间同步。

## 功能特性

### 视觉效果
- **赛博朋克配色**：4 套主题可切换（经典青色 / 品红霓虹 / 琥珀黄 / 矩阵绿）
- **CRT 扫描线**：3 秒循环的垂直扫描线动画，模拟老式显示器
- **辉光文字**：大字号时间数字带暗色辉光层，营造霓虹发光效果
- **故障艺术 (Glitch)**：每秒切换时数字短暂水平偏移，模拟信号干扰
- **HUD 装饰**：四角科技感装饰线 + 背景网格
- **秒进度条**：底部横向进度条实时显示秒数

### 时间显示
- 大字号 `HH:MM:SS`（36px 字体）
- 日期 `YYYY-MM-DD`
- 星期 + 时区 + 历史同步次数
- 3 种显示模式：仅时间 / 时间+日期 / 全信息

### BLE 时间同步
- 设备作为 BLE Peripheral 广播，名称 `CyberClock`
- 自定义 GATT 服务，手机 / Mac 可写入 Unix 时间戳
- 支持时区偏移设置
- 连接后每 5 秒通知当前时间状态
- 同步成功后屏幕闪烁提示

### USB 时间同步（v5 新增，免蓝牙）
- 用**刷固件的同一条 USB 线**直接对时，走 ESP32-C3 USB Serial/JTAG 串口
- **不依赖进入时钟页面**：主菜单、任意演示页都能对时
- 浏览器直连（Chrome/Edge Web Serial）或命令行脚本（`usb-sync.py`）均可
- 与 BLE 方式互不干扰，Mac 蓝牙搜不到设备时的最省事替代

### 其他
- 顶部状态栏显示 BLE 状态 / 同步状态 / 电池电量
- 时间持久化到 NVS，重启后恢复粗略时间
- 退出时钟页面后自动释放 BLE 射频

---

## 按键操作

| 按键 | 动作 | 功能 |
|---|---|---|
| UP | 短按 | 切换配色主题（青紫 → 青绿 → 橙红 → 矩阵绿） |
| DOWN | 短按 | 切换显示模式（完整 ⇄ 精简） |
| OK | 短按 | **手动开关蓝牙**（屏幕右上角 BT ADV / BT OFF 切换） |
| OK | 长按 | 返回主菜单（系统统一拦截） |

---

## BLE 时间同步协议

### GATT 服务定义

| 项目 | UUID |
|---|---|
| Service | `0000FFC0-0000-1000-8000-00805F9B34FB` |
| Char Write (时间写入) | `0000FFC1-0000-1000-8000-00805F9B34FB` |
| Char Notify (状态通知) | `0000FFC2-0000-1000-8000-00805F9B34FB` |

### 写入时间格式

向 `FFC1` 特征值写入**小端 (Little-Endian)** 字节：

```
字节 0-3: uint32 Unix 时间戳（UTC，秒）
字节 4-5: int16 时区偏移（小时，可选。东八区 = 8，西五区 = -5）
```

**示例**：同步 `2026-09-01 00:00:00 UTC` + 东八区

- Unix 时间戳 = `1788220800` = `0x6A961580`
- 小端字节：`80 15 96 6A`
- 时区 +8 小端：`08 00`
- 完整写入：`80 15 96 6A 08 00`

> 注意：手动写入的示例字节是**写死的时间**，仅用于验证通路。真正校时要写入"当前时刻"的时间戳，建议用下方电脑脚本自动生成。

### 通知格式

连接后每 5 秒，`FFC2` 特征值会通知 JSON 格式的状态文本：

```json
{"ts":1788220800,"tz":28800,"synced":true}
```

---

## USB 时间同步（v5 新增，免蓝牙）

> **固件要求 v5 及以上**。用刷固件的同一条 USB 数据线连接设备与电脑即可，
> 不需要进入时钟页面（主菜单也行），不依赖任何蓝牙能力。
> Mac 蓝牙搜不到设备的，直接用这条路径。

### USB 对时协议（文本命令，换行结束）

| 发送 | 设备回复 | 说明 |
|---|---|---|
| `PING` | `PONG` | 探活 / 固件版本探测（v4 及更早无此命令） |
| `T <unix> [tz小时]` | `OK TS=… TZ=…` | 设置时间（Unix 时间戳，可选时区，如 `T 1788220800 8`） |
| `Q` | `{"ts":…,"tz":…,"synced":true}` | 查询设备当前状态 |

设备回复与控制台日志混在同一串口上，按**行首前缀**（`PONG` / `OK` / `ERR` / `{"ts"`）识别即可，其余行是日志可忽略。波特率任意（USB CDC 不校验），习惯用 115200。

### 方法一：网页 USB 对时（推荐，Mac / Windows / Linux 通用）

浏览器原生 **Web Serial** 能力，双击打开包内 `web-sync.html`，切到顶部 **"USB 线同步"** 标签页：

1. USB 数据线连接设备与电脑
2. Mac / Windows / Linux 的 **Chrome 或 Edge** 打开 `web-sync.html`（双击即可；报安全上下文错误时在网页目录运行 `python3 -m http.server 8000` 后访问 `http://localhost:8000/web-sync.html`）
3. 点击 **连接 USB 设备**，弹窗中选择 **USB 串行设备（VID 303A）**——就是刷机时选的那个 `USB JTAG/serial debug unit` 端口
4. 页面自动 PING 探活 → 写入本机当前时间与时区 → 回读设备状态，日志区全程可见
5. 下次对时可直接点 **"重新连接上次设备"**（Chrome 记住了授权，不再弹窗）

**注意**：
- 浏览器占用串口期间**无法烧录固件**（esptool 打不开端口），烧录前先在页面点"断开"
- 打开串口瞬间设备可能复位一次，属正常现象，页面会自动重试 PING
- Safari / Firefox / 手机浏览器不支持 Web Serial，请换 Chrome/Edge 桌面版

### 方法二：usb-sync.py 命令行脚本

```bash
pip3 install pyserial          # 首次需安装
python3 usb-sync.py             # 自动：找设备 -> PING 探活 -> 写入本机时间 -> 回读状态
python3 usb-sync.py --list      # 列出候选串口
python3 usb-sync.py --port /dev/cu.usbmodemXXXX   # 多个串口时手动指定
```

Mac / Windows / Linux 通用；Linux 若提示权限错误：`sudo usermod -aG dialout $USER` 后重新登录。

### 手动验证（任意串口终端）

用 `screen`、`minicom`、Putty 连接设备串口（115200），逐行输入：

```
PING
T 1788220800 8
Q
```

三行分别返回 `PONG`、`OK TS=1788220800 TZ=8`、`{"ts":…}` 即工作正常。

---

## 手机同步步骤（iOS / Android）

> **重要：不要在系统"蓝牙设置"里点击连接 CyberClock。**
> BLE 外设不走系统配对通道，从系统设置连接会提示"需要对应的应用/配套应用"——
> 它说的"应用"不是设备配套 App，而是任意 BLE 调试工具（下方的 nRF Connect / LightBlue）。
> 正确入口：安装 BLE 调试 App，在 **App 内** 扫描、连接、写入。

### 方法一：使用 BLE 调试 App（推荐）

1. 进入时钟页面，设备开始广播 `CyberClock`
2. 手机安装 **nRF Connect**（Android，Google Play / APKPure 可下）或 **LightBlue**（iOS，App Store 可下）
3. 在 App 内扫描，找到 `CyberClock`，点 **CONNECT** 连接（无需配对；若弹出配对框直接确认即可）
4. 展开未知服务，找到 `FFC0` 服务，进入 `FFC1` 特征值
5. 选择 **Write** / **Write Value**，格式选 **Byte array / Hex**，输入十六进制字节（见上方示例）
6. 写入后设备屏幕立即更新时间，并闪烁 "TIME SYNCED"

### 方法二：使用快捷指令（iOS，需配合第三方 BLE 快捷指令）

可通过 iOS 快捷指令 App 的 BLE 操作，自动获取当前时间并转换为小端字节写入。

---

## Mac 同步步骤

> **Mac 用户注意：若 Chrome 始终搜不到蓝牙设备，别再跟蓝牙较劲——
> 直接用上方"USB 时间同步"章节的网页 USB 对时（插线即用），10 秒搞定。**
> 以下蓝牙方法适用于蓝牙工作正常或不想插线的场景。

### 方法一：网页版同步（推荐，无需安装任何东西）

使用浏览器原生 **Web Bluetooth** 能力，双击打开包内 `web-sync.html` 即可（纯本地静态页，无任何数据上传）：

1. 设备进入 **CyberClock 页面**（BLE 仅在该页面广播）
2. Mac 上用 **Chrome 或 Edge** 打开 `web-sync.html`（双击即可；若浏览器报安全上下文错误，在网页所在目录运行 `python3 -m http.server 8000`，访问 `http://localhost:8000/web-sync.html`）
3. 点击 **连接 CyberClock**，在弹出的设备选择框中选中设备
4. 页面默认勾选"连接后自动同步时间"——连上即写入本机当前时间与时区，完成校时
5. 页面下方实时显示 FFC2 通知的设备状态（时间戳、时区、是否已同步），也可写入自定义时间

**Mac 搜不到设备？先做交叉验证（金标准）**：Mac 设备选择框保持打开的同时，用安卓 nRF Connect **只扫描不连接**——手机此刻能看到 CyberClock 而 Mac 看不到，说明设备广播正常、问题在 Mac 侧。按顺序排查：

1. **先断开安卓手机的连接**：BLE 同一时间只允许一个中心设备连接，被占用时设备停止广播，Mac 自然搜不到
2. 保持页面"仅显示名称含 CyberClock 的设备"**不勾选**（默认即如此），从全部设备列表中选——名字不是 CyberClock 的陌生设备也可能是它（macOS 会缓存旧的蓝牙广播名）
3. 系统设置 → 隐私与安全性 → 蓝牙：确认 **Chrome 已被允许**（首次弹窗若点了"不允许"，之后会静默失败）
4. 地址栏打开 `chrome://bluetooth-internals` → Devices → Start Scan：这里能扫到设备说明是 Chrome 选择器问题；扫不到则是系统蓝牙层面问题
5. 系统设置 → 蓝牙关闭再打开；仍不行重启 Mac
6. 以上都失败 → 用方法二脚本（走系统蓝牙，完全绕过 Chrome）

**浏览器要求**：Chrome（macOS）、Edge、Opera 均支持；**Safari、Firefox 不支持**；iPhone / iPad 所有浏览器均不支持（iOS 强制 WebKit 内核），iOS 请用 LightBlue App。

**扩展用法**：把 `web-sync.html` 上传到任意 HTTPS 静态托管（如 GitHub Pages），安卓手机用 Chrome 打开同样可用，免装 nRF Connect。本项目的在线版已部署：<b>https://y2lin.github.io/AP_CyberClock/web-sync.html</b>（大陆访问 github.io 不稳定时用本地文件）。

### 方法二：mac-sync.py 脚本（Chrome 搜不到时的可靠替代）

包内 `mac-sync.py` 走 macOS 系统蓝牙（CoreBluetooth），与浏览器完全无关：

```bash
pip3 install bleak            # 首次需安装
python3 mac-sync.py            # 自动：按服务UUID扫描 → 连接 → 写入当前时间 → 回读状态
python3 mac-sync.py --list     # 列出附近所有 BLE 设备（排查"搜不到设备"）
python3 mac-sync.py --name 名字 # 名称被缓存成旧名时，从 --list 里看到实际名字后手动指定
```

- 扫描优先按**服务 UUID** 匹配（FFC0），不受 macOS 广播名缓存影响
- 终端首次运行若弹出"xxx 想要使用蓝牙"，必须点**允许**
- `--list` 一个设备都扫不到：Mac 蓝牙权限/适配器问题，重启蓝牙或重启 Mac
- 写入后自动监听 6 秒 FFC2 通知，回读设备状态确认同步成功

### 方法三：使用 Mac 上的 BLE 调试工具

- **Bluetooth Explorer**（Apple 额外工具）
- **LightBlue**（Mac App Store 有版本）
- 操作方式与手机类似，找到 `FFC1` 写入时间戳字节

---

## 构建与烧录

### 环境要求
- ESP-IDF **v5.5.3**（项目固定版本）
- Python 3.8+
- 已安装 FoloToy AI Passport 硬件驱动

### 构建步骤

```bash
# 1. 克隆项目（本分支已包含所有修改）
git clone https://github.com/folotoy/ai-passport.git
cd ai-passport
git checkout feature/cyberpunk-clock   # 或使用你本地的分支

# 2. 初始化 ESP-IDF 环境
get_idf553    # 或 source ~/esp/v5.5.3/export.sh

# 3. 设置目标芯片
idf.py set-target esp32c3

# 4. 构建
idf.py build

# 5. 烧录（设备通过 USB 连接）
idf.py flash

# 6. 查看串口日志（可选）
idf.py monitor
```

### 分区说明
- 项目使用 8MB Flash，自定义分区表 `partitions.csv`
- 固件大小不超过 3MB（factory-app 分区）
- **不要修改** `cardid@0x356000` 和 `recovery@0x700000` 分区（小程序 BLE Recovery 依赖）

---

## 新增/修改的文件清单

### 新增文件
| 文件 | 说明 |
|---|---|
| `main/time_manager.h` | 时间管理器头文件 |
| `main/time_manager.c` | 时间管理：NVS 持久化、时区、同步状态 |
| `main/ble_time_sync.h` | BLE 时间同步服务头文件 |
| `main/ble_time_sync.c` | 可连接 BLE GATT 服务实现 |
| `main/usb_time_sync.h` | USB 串口对时头文件（v5） |
| `main/usb_time_sync.c` | USB Serial/JTAG 文本命令对时（v5） |
| `main/cyber_clock.h` | 赛博朋克时钟头文件 |
| `main/cyber_clock.c` | 时钟 UI + 动画 + 按键交互 |

### 修改文件
| 文件 | 修改内容 |
|---|---|
| `main/main.c` | DEMOS 数组新增 CyberClock 入口，s_ok 数组扩展；启动 USB 对时服务（v5） |
| `main/demo.h` | 新增时钟函数声明 |
| `main/CMakeLists.txt` | 新增 4 个源文件与 `esp_driver_usb_serial_jtag` 依赖 |
| `sdkconfig.defaults` | 新增 montserrat_36 字体，LVGL 内存池 24KB→32KB |

---

## 技术细节

### 时间持久化策略
ESP32-C3 没有电池供电的 RTC，深度睡眠后系统时间会重置。本项目的策略：
1. 每次 BLE 同步成功后，将 Unix 时间戳写入 NVS
2. 启动时从 NVS 恢复时间作为粗略起点
3. 状态栏用 `SYNCED` / `NO-SYNC` 标记时间可信度
4. 建议每次使用前通过 BLE 重新同步

### BLE 与 Wi-Fi 共存
当前固件中 BLE 和 Wi-Fi 堆栈**不能同时运行**（进入页面时初始化，退出时释放）。时钟页面只使用 BLE，不影响其他 demo 页面的 Wi-Fi 功能。

### 内存占用
- 新增 36px 字体约占用 Flash 空间（运行时按需渲染，不占 RAM）
- LVGL 内存池从 24KB 增加到 32KB，容纳更多 UI 对象和动画
- BLE NimBLE 堆栈运行时约占用 20-30KB RAM

### 已知限制
1. 不支持 BLE 标准 Current Time Service (CTS)，使用自定义服务（更灵活）
2. 时间同步需要手动触发，没有自动轮询
3. 深度睡眠后时间丢失，需重新同步
4. 同一时间只允许 1 个 BLE 连接（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`）

---

## 配对说明与"配对失败"处理

### 本固件是否需要配对？

**不需要。** 时钟的 GATT 特征（写入时间戳 FFC1 / 通知 FFC2）均未启用加密，用 nRF Connect / LightBlue 直接连接即可写入时间，正常流程中**不应出现系统配对弹窗**。

### 为什么手机会弹"配对请求"？

出现配对弹窗，几乎都是**新旧绑定信息失配**引起的：

- 手机曾与该设备配对过（如之前用 FoloToy 小程序安装过固件、或连接时点过"配对"），手机端保存了旧密钥；
- 而设备端密钥丢失或对不上（重新刷机、换了固件、NVS 被擦除等）。

此时手机先尝试用旧密钥加密 → 失败 → 自动发起重新配对 → 弹出配对框。

### 旧版固件的缺陷（已在当前固件修复）

旧版固件的 GAP 事件处理缺少 `BLE_GAP_EVENT_REPEAT_PAIRING` 分支：手机发起重新配对时，固件未答复"删除旧绑定并接受"，协议栈按拒绝处理，手机端即显示**"配对失败"**。

当前固件已修复：

- 收到重复配对请求时，自动删除设备端旧绑定记录并接受重新配对；
- 绑定存储写满时自动淘汰最旧记录（round-robin），避免后续配对失败。

### 手机端建议操作（遇到配对失败时）

1. **系统设置 → 蓝牙 → 找到 CyberClock（或 FoloToy 设备名）→ 忽略/取消配对**，清除手机端旧绑定；
2. 回到 nRF Connect / LightBlue，重新扫描连接（不要在系统蓝牙设置里点连接，BLE 外设不走系统配对通道）；
3. 若弹出配对框，点"配对"即可——固件现在会自动完成重新配对；
4. 连接成功后进入 FFC1 写时间戳，完成同步。

---

## 故障排除

| 问题 | 解决方案 |
|---|---|
| **USB 对时 PING 无响应** | 固件是 v4 及更早（无 USB 对时命令）。刷入本包 v5 固件（应用段 `0x10000` 或合并固件 `0x0`）后重试；确认选中的是 `USB JTAG/serial debug unit` 串口 |
| **网页 USB 标签页"连接 USB 设备"点不动 / 提示不支持** | Web Serial 需要 Mac / Windows / Linux 的 Chrome 或 Edge 桌面版；Safari、Firefox、手机浏览器都不支持。命令行可用 `usb-sync.py` 代替 |
| **浏览器占着串口烧录失败** | 浏览器占用期间 esptool 打不开端口：先在网页点"断开"（或关掉标签页），再烧录 |
| **写入时间后整机卡死（屏幕冻结、按键无响应）** | 旧固件（v3 及以前）的 BLE 写入回调在协议栈任务里直接刷新 LVGL 界面，与渲染任务并发导致死锁。**v4 已修复**（回调只置标志，由界面定时器刷新）。请刷入 v4 及以上固件后重试 |
| **同步成功后蓝牙自动断开** | v6 省电策略的正常行为：同步（蓝牙或 USB）成功约 5 秒后设备自动断开并关闭蓝牙。需要再同步：设备上短按 OK 键重开蓝牙（屏幕显示 BT ADV） |
| **手机/Mac 扫不到设备，但以前能扫到** | ① 设备屏幕右上角若是 `BT OFF`：时间已同步、蓝牙默认关闭——短按 OK 键打开；② 安卓手机先断开连接（被占用时设备不广播）；③ 重启手机蓝牙清缓存 |
| **Mac 网页搜不到设备** | ① 设备屏幕须显示 `BT ADV`（BT OFF 时按 OK 键）；② 安卓手机先断开连接（被占用时设备不广播）；③ 网页保持"仅显示名称过滤"不勾选，从全部设备列表选；④ macOS 缓存旧广播名时重启 Mac 蓝牙；⑤ Chrome 蓝牙权限必须允许。详见"Mac 同步步骤"章节；或直接改用 USB 线同步 |
| 手机扫描不到设备 | ① **必须先进设备菜单里的 CyberClock 页面**（BLE 仅在该页面运行，退出即关闭）；② 设备须显示 `BT ADV`（未同步自动开、已同步默认关——按 OK 键开）；③ 手机蓝牙开关关闭再打开、清缓存后重新扫描；④ 靠近设备（<5m） |
| **扫描到的名字是 "CyberClock" 还是 FoloToy 名？** | 本固件广播名固定为 `CyberClock`。若看到的是其他名字（如原厂固件的设备名），说明刷的不是本固件或未进入时钟页面 |
| **配对失败 / 配对弹窗** | 见上方"配对说明"章节：手机端忽略旧绑定后重连；当前固件已支持自动重新配对 |
| 写入时间后没反应 | 确认写入的是小端字节；检查时间戳是否合理（>1700000000）；查看串口日志 |
| 时间显示 1970 | 未同步过时间，NVS 中无有效时间戳；通过 BLE 同步一次即可 |
| 编译报错字体找不到 | 确认 `sdkconfig.defaults` 中 `CONFIG_LV_FONT_MONTSERRAT_36=y`；清理构建后重编 |
| BLE 连接后立即断开 | 可能是 MTU 协商问题；尝试用 nRF Connect 连接；查看串口日志中的断开原因 |

---

## 许可证

基于原项目 MIT 许可证。新增代码同样采用 MIT 许可证。
