<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

## Unreleased

- 固件 v10.5.2（精简模式光晕重做为贴字形文字光晕，真机驱动迭代）：v10.5.1 的盒阴影光晕在真机上仍呈现为贴着时间的椭圆光圈——LVGL 对象阴影永远跟随包围盒，label 没有按字形的文字阴影。光晕改为文字发光在 LVGL 上唯一正确的形态：12 个同文本副本围绕数字微偏移 1-3px、三档亮度环（对角 90 / 十字 70 / 外环 45 透明度），层叠在主字之下，叠出跟随字形的 1-3px 渐变光晕（创建于主字之下、残影之上；x 跟随重新居中的主字；随秒 1.0/0.65 呼吸，未同步闪烁时与主字一起压暗；墨水屏隐藏全部光晕层）。主字自身不再带任何阴影与圆角。应用段 1,050,368 → 1,050,480 字节（+112）。

- 固件 v10.5.1（精简模式修复，真机实测发现）：精简模式 `HH:MM` 主字的柔光晕呼吸在真机上呈现为时间上下各一条 240px 横向贯穿光带。根因：LVGL 的对象阴影画在包围盒边缘、不贴字形，而主字 label 此前占满 240px 全宽——所谓"光晕"就是全宽盒子的阴影。主字改为内容自适应宽度并在每次文本刷新后重新居中（`min_time_recenter()`），阴影包围盒紧贴约 140px 文字，光晕只剩紧贴数字的一小段（radius 24 圆角柔化光带边缘；残影 label 仍全宽居中——它们不带阴影）。应用段 1,050,320 → 1,050,368 字节（+48）。

- 固件 v10.5（精简模式重做，设计定稿方案 C）：DOWN 精简模式按定稿设计图 v4.2 全新实现——完整/精简两组控件各挂独立全屏层容器，DOWN 切换整层互换显隐（旧实现只隐藏部分装饰元素，表盘骨架仍在；新实现精简层不包含任何完整模式元素）。精简层只显示：`HH:MM` 48px 居中大字（保留双色差残影与未同步呼吸闪烁，新增主字柔光晕呼吸，墨水屏主题全部禁用）、紧贴主字底的**秒进度下划线**（v4.4 视觉：112px 居中双层错位线，约 4/5 主字宽——低透明次色残影轨道 + 按本分钟已过秒数填充的 70% 亮度（墨水屏 80%）主线，距主字底 8px 呼吸间距；无光标）、日期行 `YYYY-MM-DD  星期`（16px 全行统一暗色，双空格分隔——montserrat 无中点字形）、低于 20% 才出现的右上角低电量红点（慢闪告警、墨水屏常亮，与 v10.4.3 校准后的 SOC 联动）。五套主题全部适配；故障爆发/色条/心跳/频谱等完整层元素在精简模式一律不出现。下划线 y=149/150 与主字底 8px 间距可在真机按字形度量微调（代码已注明）。应用段 1,048,624 → 1,050,320 字节（+1,696，含 v10.4.3 电量计模块；本地 ESP-IDF v5.5.3 构建同步验证——合并镜像 1,115,856 字节，DRAM 174,754 / 321,296 字节，54.4%）。
- 固件 v10.4.3（电池电量校准）：修复电量百分比系统性偏高——CW2017 电量计未写入实际电芯 profile，芯片跑默认通用 Li-Po 曲线，把 3.92V 标成 73%（典型电芯 OCV 约 60-70%）。新增纯逻辑模块 `main/battery_gauge.c/h`：弃用芯片 SOC 寄存器，改读 VCELL 电压经固件内典型 4.2V Li-Po OCV 表分段插值换算（3.92V→68%），叠加三点电压中值滤波（剔除 I2C 偶发坏值，越界采样直接丢弃）与显示迟滞（变化不足 3% 不上屏；跨 20% 低电阈值立即放行，红警与解除不延迟）；表盘每 5 秒读数路径接入，10 段电池条与低电量红线行为不变。新增 host tests `tests/test_battery_gauge.c` 覆盖查表关键点、全程单调性、迟滞边界、坏值剔除与放电序列。OCV 表为典型曲线，待实际电芯规格书或放电标定后进一步校准（`docs/TODO.md` 电池项）。
- 新增 `docs/TODO.md`（+ 中文配对）：下一阶段任务清单，供会话快速接续；记录 v10.4.2 之后约定跟进的三件事——精简模式重设计（先出 HTML 设计图，定稿前不动固件）、电池电量校准排查、`FAP_SCREENSHOT_V1` 踩坑笔记整理到 `AP_Sound_Test` 分支。已注册进双语文档索引。
- 固件 v10.4.2（截屏 USB 写出修复，真机实测发现）：v10.4.1 的采集路径已正确，但每次截屏仍在协议头之后收到设备回的 `ERR SHOT`——根因在 `usb_serial_jtag` 驱动：发送侧是 FreeRTOS 环形缓冲（条目必须连续放入），默认 `tx_buffer_size` 只有 256 字节，2KB 像素分片根本放不进去、写入当场返回 0（主机看到的"像素数据不完整 10/153600"正是这 10 字节 `ERR SHOT\r\n` 回复）。驱动安装时显式配置 4KB TX 环、像素分片改为 1KB 一段发送；`ERR SHOT` 回复追加原因码（`BUSY` / `MEM` / `HOOK` / `DATA` / `STALL` / `USB` / `SHORT`），网页截屏页可据此诊断而非盲目重试。应用段 1,048,480 → 1,048,624 字节（+144）。
- 固件 v10.4.1（截屏内存修复，真机实测发现）：v10.4 的 `FAP_SCREENSHOT_V1` 实现要在系统堆一次性分配 150KB 整屏帧缓冲，真机最大连续空闲块给不出来，导致每次截屏都回 `ERR SHOT`。采集路径改为流式：flush 拦截把每个 20 行条带拷进 3 格环形槽（每格 11.5KB，共 34.5KB，峰值内存约为旧方案的 1/4.4），USB 任务边收边发；生产侧环满时小睡背压，双侧共享中止标志保证失败路径干净收尾。应用段 1,047,904 → 1,048,480 字节（+576）。
- 固件 v10.4（社区发布支持）：USB 串口服务实现 AI Passport 社区发布助手要求的 `FAP_SCREENSHOT_V1` 观测协议——收到命令后固件临时包装显示刷新回调，借一次整屏重绘把原生 RGB565 分块累积进系统堆缓冲（64KB LVGL 池装不下 150KB 整屏帧），再从 USB 流回，期间临时静默日志防止控制台字节混入像素流。纯观测：不改页面状态、时间与任何设置。USB 任务只置请求标志；钩子安装/卸载与累积拷贝全部发生在 LVGL 上下文，超时路径有关门标志与兜底卸钩双保险。应用段 1,046,448 → 1,047,904 字节（+1.4KB）。
- 固件 v10.3（代码审查收尾，P3×5 + P4×9 全部修复）：`main.c` 移除 v8 起不可达的 demo 菜单与演示页入口表，未被引用的 demo 页连同其拉入的 Wi-Fi 协议栈整链被链接器裁剪——应用段 1,547,216 → 1,046,448 字节（-32.4%，较 v10.1 累计 -39%）；`time_manager` 的 NVS 写入从 BLE/USB 任务挪到 LVGL 定时器上下文（dirty 标志 + `flush_pending()`，协议栈任务不再被 flash 擦除卡住）；时区写入增加 ±14 小时最终防线（BLE 入口此前无校验）；`s_state` 读写包进临界区消除快照撕裂；`ble_time_sync_stop()` 失败路径复位初始化标记（旧版会让 BLE 到重启前永久失效）；另清理死 API、重复宏、误导注释、假默认回显等九项打磨问题。
- 固件 v10.2（功耗与体积优化）：默认主频 160→80MHz 并开启 DFS 动态调频（忙时 80 / 空闲 40MHz，SPI/I2C 驱动自动持 PM 锁）；新增空闲自动降亮（90 秒无按键背光压到 20%，任意按键恢复，设定值与 NVS 不变）；非表盘页刷新节拍 100ms→250ms；编译优化由调试级（-Og）改为体积优先（-Os）并关闭无人使用的 20px 字库——应用段 1,716,832 → 1,547,216 字节（-9.9%）；本地 `sdkconfig` 改为从 `sdkconfig.defaults` 全量重生成，本地与 CI 构建配置自此完全一致。
- 新增首篇软件设计文档：`docs/software-design/cyber-clock-design.zh_CN.md`（含英文配对页），覆盖赛博朋克时钟应用栈——模块划分、并发模型、页面状态机、时间同步数据流、NVS 持久化布局、显示内存预算、失败降级与截至固件 v10.1 的已知限制；已注册进软件设计索引与 `docs/INDEX.md`。
- 固件 v10.1（显示缺陷修复 + 两个低优需求）：修复右栏电量%/星期/运行时长/电压四个标签因 `lv_obj_get_y()` 在首次布局前返回占位坐标而被全部压到 y=0 互相覆盖的问题（改为显式传 y）；修复小心形 y231 整行缺失导致的 1px 横缝（心跳"显示不全 + 多余小横线"）；出厂默认亮度 100→80；重启后无条件回到未对时状态（旧判据在部分唤醒路径失效，导致关机再开机仍显示 SYNCED 而时间停走），并新增运行期每 5 分钟回写 NVS，断电恢复偏差由"整个开机时长"压到 5 分钟内；`sdkconfig.defaults` 的 LVGL 内存池 32→64KB（v10 白屏修复此前只在本地 `sdkconfig`，未进仓库，CI 构建仍会白屏）。预编译固件与补丁同步更新。
- 将小程序 BLE 安装兼容提升为二创模板强制契约：固定保护 `cardid`/Recovery 分区，
  保留上键持续 5 秒进入 Recovery 的 bootloader hook，并在 CI 强制校验合并镜像结构、
  分区表 MD5/范围、3 MB 应用上限和保护分区数据不入包。
- 规定多应用发布的 Release 标题约定：tag 按 `v<版本>-<应用名>`（如 `v0.1.0-voice-keychain`）命名，让 Release 标题同时带版本与应用名；发布成功后核对标题，保证一眼扫 Release 列表就能区分是哪个应用。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `project-completion`、`file-issues` 与经验索引文档。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。
