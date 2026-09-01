// main/ble_time_sync.c —— 可连接 BLE GATT 时间同步服务。
//
// 与 demo_ble.c 的区别：demo_ble 只做不可连接广播（仅证明射频可用），
// 本模块实现完整的 Peripheral + GATT，让手机/Mac 写入时间戳。

#include "ble_time_sync.h"
#include "time_manager.h"
#include "demo_radio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_ts";

// ---- UUID 定义（128 位，基础 UUID + 16 位短 ID）----
#define GATT_TS_SERVICE_UUID      0xFFC0
#define GATT_TS_CHAR_WRITE_UUID   0xFFC1
#define GATT_TS_CHAR_NOTIFY_UUID  0xFFC2

static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xC0, 0xFF, 0x00, 0x00);
static const ble_uuid128_t s_char_write_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xC1, 0xFF, 0x00, 0x00);
static const ble_uuid128_t s_char_notify_uuid =
    BLE_UUID128_INIT(0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xC2, 0xFF, 0x00, 0x00);

// ---- 全局状态 ----
static ble_ts_state_t s_state = BLE_TS_DISCONNECTED;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_notify_handle;       // CCCD 句柄，用于判断客户端是否订阅
static bool s_notify_enabled;
static bool s_initialized;
static bool s_start_requested;
static uint8_t s_addr_type;
static SemaphoreHandle_t s_host_stopped;
static esp_timer_handle_t s_notify_timer;
static ble_ts_sync_cb_t s_sync_cb;
static char s_peer_name[32];

#define DEVICE_NAME "CyberClock"

// ---- 前向声明 ----
static int gap_event(struct ble_gap_event *event, void *arg);
static int gatt_svr_chr_write(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gatt_svr_chr_read(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

// GATT 服务定义表
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // 写入特征：接收时间戳
                .uuid = &s_char_write_uuid.u,
                .access_cb = gatt_svr_chr_write,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 通知特征：定期上报当前时间状态
                .uuid = &s_char_notify_uuid.u,
                .access_cb = gatt_svr_chr_read,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_notify_handle,
            },
            { 0 },  // 结束标记
        },
    },
    { 0 },  // 服务结束
};

// ---- 工具：// 固定设备名，避免在 host 同步前读取 MAC 地址的时序问题。
#define DEVICE_NAME "CyberClock"

static void build_device_name(char *buf, size_t len)
{
    snprintf(buf, len, "%s", DEVICE_NAME);
}

// ---- 广播 ----
static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    char name[32];
    build_device_name(name, sizeof(name));

    // 广播包上限 31 字节：flags(3) + "CyberClock"(12) + 128位UUID(18) = 33 字节超限，
    // ble_gap_adv_set_fields() 会返回 BLE_HS_EMSGSIZE，导致广播启动失败、手机扫描不到设备。
    // 因此设备名放广播包，128 位服务 UUID 放扫描响应包（nRF Connect 主动扫描时仍可见）。
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败: %d", rc);
        return rc;
    }

    // 扫描响应包：附 128 位服务 UUID，方便客户端按 UUID 筛选
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应数据失败: %d", rc);
        return rc;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // 可连接（与 demo_ble 的 NON 不同）
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER,
                            &params, gap_event, NULL);
    if (rc == 0) {
        s_state = BLE_TS_ADVERTISING;
        ESP_LOGI(TAG, "开始可连接广播: %s", name);
    }
    return rc;
}

// ---- GATT 写入回调：解析时间戳 ----
static int gatt_svr_chr_write(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    struct os_mbuf *om = ctxt->om;
    if (!om) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    size_t len = OS_MBUF_PKTLEN(om);
    if (len < 4) {
        ESP_LOGW(TAG, "写入数据太短: %u 字节（需要至少 4 字节时间戳）", (unsigned)len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t buf[16];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (os_mbuf_copydata(om, 0, len, buf) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 小端解析 32 位 Unix 时间戳
    uint32_t unix_utc = (uint32_t)buf[0]
                       | ((uint32_t)buf[1] << 8)
                       | ((uint32_t)buf[2] << 16)
                       | ((uint32_t)buf[3] << 24);

    ESP_LOGI(TAG, "收到时间戳: %u (0x%08X)", unix_utc, unix_utc);

    if (time_manager_set_unix_utc((time_t)unix_utc) == ESP_OK) {
        s_state = BLE_TS_SYNCED;
        if (s_sync_cb) s_sync_cb();
    }

    // 可选：第 5-6 字节为时区偏移（int16 小端，单位：小时）
    if (len >= 6) {
        int16_t tz_hours = (int16_t)(buf[4] | ((uint16_t)buf[5] << 8));
        time_manager_set_timezone((int32_t)tz_hours * 3600);
        ESP_LOGI(TAG, "收到时区偏移: %d 小时", tz_hours);
    }

    return 0;
}

// ---- GATT 读取回调：返回当前状态 JSON ----
static int gatt_svr_chr_read(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    time_manager_state_t st = time_manager_get_state();
    char json[96];
    int n = snprintf(json, sizeof(json),
                      "{\"ts\":%ld,\"tz\":%ld,\"synced\":%s}",
                      (long)time_manager_get_unix_utc(),
                      (long)st.tz_offset,
                      st.synced ? "true" : "false");
    return os_mbuf_append(ctxt->om, json, n);
}

// ---- 通知定时器：每 5 秒发送一次状态 ----
static void notify_task(void *arg)
{
    (void)arg;
    if (s_state != BLE_TS_CONNECTED && s_state != BLE_TS_SYNCED) return;
    if (!s_notify_enabled || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    time_manager_state_t st = time_manager_get_state();
    char json[96];
    int n = snprintf(json, sizeof(json),
                      "{\"ts\":%ld,\"tz\":%ld,\"synced\":%s}",
                      (long)time_manager_get_unix_utc(),
                      (long)st.tz_offset,
                      st.synced ? "true" : "false");

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, n);
    if (om) {
        ble_gattc_notify_custom(s_conn_handle, s_notify_handle, om);
    }
}

// ---- GAP 事件回调 ----
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_state = BLE_TS_CONNECTED;
            // 记录对端地址
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                ble_addr_t *addr = &desc.peer_id_addr;
                snprintf(s_peer_name, sizeof(s_peer_name), "%02X:%02X:%02X:%02X:%02X:%02X",
                         addr->val[5], addr->val[4], addr->val[3],
                         addr->val[2], addr->val[1], addr->val[0]);
            } else {
                snprintf(s_peer_name, sizeof(s_peer_name), "unknown");
            }
            ESP_LOGI(TAG, "已连接: %s", s_peer_name);
            // 启动通知定时器
            esp_timer_start_periodic(s_notify_timer, 5000000);  // 5 秒
        } else {
            ESP_LOGW(TAG, "连接失败: %d", event->connect.status);
            s_state = BLE_TS_ADVERTISING;
            start_advertising();  // 重新广播
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开连接: reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        s_peer_name[0] = '\0';
        esp_timer_stop(s_notify_timer);
        // 重新开始广播，等待下一次连接
        s_state = BLE_TS_ADVERTISING;
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // 客户端订阅/取消通知特征
        s_notify_enabled = (event->subscribe.cur_notify == 1);
        ESP_LOGI(TAG, "客户端订阅通知: %s", s_notify_enabled ? "开启" : "关闭");
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 更新: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // 广播停止（通常因为被连接），不需要重新启动
        ESP_LOGI(TAG, "广播结束");
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // Just Works 配对：直接回复确认（SM 事件同样派发到 GAP 回调）
        if (event->passkey.params.action == BLE_SM_IOACT_NONE) {
            struct ble_sm_io pkey = {0};
            pkey.action = event->passkey.params.action;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "Just Works 配对确认");
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "连接已加密");
        } else {
            // 手机端持有旧绑定密钥与本设备失配：手机随后会重新发起配对
            ESP_LOGW(TAG, "加密失败(旧密钥失配?): %d", event->enc_change.status);
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // 手机端已丢绑定而本设备仍存有旧记录（或反之）时，手机发起的
        // 新配对会触发此事件。必须删除旧绑定并返回 RETRY，否则协议栈
        // 视为拒绝，手机端表现为"配对失败"。
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            ESP_LOGW(TAG, "检测到重复配对，已删除旧绑定记录并接受重新配对");
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

// ---- NimBLE host 回调 ----
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset: %d", reason);
    s_state = BLE_TS_ERROR;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr 失败: %d", rc);
        s_state = BLE_TS_ERROR;
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_addr 失败: %d", rc);
        s_state = BLE_TS_ERROR;
        return;
    }
    if (s_start_requested) {
        rc = start_advertising();
        if (rc != 0) {
            ESP_LOGE(TAG, "启动广播失败: %d", rc);
            s_state = BLE_TS_ERROR;
        }
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

// ---- 公共 API ----

esp_err_t ble_time_sync_start(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已在运行中");
        return ESP_ERR_INVALID_STATE;
    }

    s_state = BLE_TS_DISCONNECTED;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_notify_enabled = false;
    s_peer_name[0] = '\0';

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;

    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        nimble_port_deinit();
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 注册自定义 GATT 服务（必须在标准服务初始化之后）
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败: %d", rc);
        goto fail;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败: %d", rc);
        goto fail;
    }

    char name[32];
    // 注意：设备名在 on_sync 时才知道 MAC，这里先用基础名，广播时会用完整名
    snprintf(name, sizeof(name), "%s", DEVICE_NAME);
    ble_svc_gap_device_name_set(name);

    // BLE 安全配置：Just Works 配对，不绑定，不需要 MITM
    // 避免 iOS/Mac 连接时卡在配对流程
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_oob_data_flag = 0;
    // SM 事件（PASSKEY_ACTION / ENC_CHANGE）经 GAP 回调 gap_event() 派发，
    // 本版本 NimBLE 的 ble_hs_cfg 无 sm_event_cb 成员。

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    // 绑定存储写满时按 round-robin 淘汰最旧记录，避免后续配对失败
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 创建通知定时器
    esp_timer_create_args_t timer_args = {
        .callback = notify_task,
        .name = "ble_ts_notify",
    };
    if (esp_timer_create(&timer_args, &s_notify_timer) != ESP_OK) {
        ESP_LOGE(TAG, "创建通知定时器失败");
        goto fail;
    }

    s_start_requested = true;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE 时间同步服务已启动");
    return ESP_OK;

fail:
    if (s_host_stopped) { vSemaphoreDelete(s_host_stopped); s_host_stopped = NULL; }
    nimble_port_deinit();
    s_initialized = false;
    return ESP_FAIL;
}

void ble_time_sync_stop(void)
{
    if (!s_initialized) return;
    s_start_requested = false;

    if (s_notify_timer) {
        esp_timer_stop(s_notify_timer);
        esp_timer_delete(s_notify_timer);
        s_notify_timer = NULL;
    }

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();

    int rc = nimble_port_stop();
    if (rc == 0 && s_host_stopped) {
        xSemaphoreTake(s_host_stopped, portMAX_DELAY);
    }
    if (rc == 0) {
        nimble_port_deinit();
        s_initialized = false;
    } else {
        ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
    }

    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }

    s_state = BLE_TS_DISCONNECTED;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "BLE 时间同步服务已停止");
}

ble_ts_state_t ble_time_sync_get_state(void)
{
    return s_state;
}

const char *ble_time_sync_peer_name(void)
{
    return s_peer_name[0] ? s_peer_name : "---";
}

void ble_time_sync_set_sync_callback(ble_ts_sync_cb_t cb)
{
    s_sync_cb = cb;
}
