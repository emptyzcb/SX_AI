#include "ble_adv.h"
#include "config_server.h"
#include "esp_log.h"
#include "device_id_manager.h"
#include "esp_chip_info.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <cstdio>
#include <cstring>

static const char *BLE_TAG = "BLE_ADV";

static uint8_t ble_addr_type;
/** 为让路 WiFi 扫描而主动 stop 广播时为 true；避免 ADV_COMPLETE 里立刻把广播拉起来 */
static volatile bool s_ble_radio_held_for_wifi_scan = false;
static uint16_t g_val_handle_ffe2;
static uint16_t g_pending_write_conn_handle = 0xffff;
static std::string g_pending_write_payload;

static const ble_uuid16_t g_prov_svc_uuid = BLE_UUID16_INIT(0xffe0);
static const ble_uuid16_t g_prov_chr_write_uuid = BLE_UUID16_INIT(0xffe1);
static const ble_uuid16_t g_prov_chr_notify_uuid = BLE_UUID16_INIT(0xffe2);

static int prov_gap_event(struct ble_gap_event *event, void *arg);
static int prov_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);

static void reset_pending_write_payload(uint16_t conn_handle)
{
    g_pending_write_conn_handle = conn_handle;
    g_pending_write_payload.clear();
}

static bool is_complete_json_payload(const std::string& payload)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (char ch : payload) {
        if (escaped) {
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            if (in_string) {
                escaped = true;
            }
            continue;
        }

        if (ch == '"') {
            in_string = !in_string;
            continue;
        }

        if (in_string) {
            continue;
        }

        if (ch == '{') {
            depth += 1;
        } else if (ch == '}') {
            depth -= 1;
            if (depth < 0) {
                return false;
            }
        }
    }

    return !payload.empty() && !in_string && depth == 0 && payload.front() == '{';
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_prov_svc_uuid.u,
        .includes = NULL,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &g_prov_chr_write_uuid.u,
                .access_cb = prov_gatt_access,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .min_key_size = 0,
                .val_handle = NULL,
                .cpfd = NULL,
            },
            {
                .uuid = &g_prov_chr_notify_uuid.u,
                .access_cb = prov_gatt_access,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .min_key_size = 0,
                .val_handle = &g_val_handle_ffe2,
                .cpfd = NULL,
            },
            {
                .uuid = NULL,
                .access_cb = NULL,
                .arg = NULL,
                .descriptors = NULL,
                .flags = 0,
                .min_key_size = 0,
                .val_handle = NULL,
                .cpfd = NULL,
            },
        },
    },
    {
        .type = 0,
        .uuid = NULL,
        .includes = NULL,
        .characteristics = NULL,
    },
};

static void send_wifi_config_ack(uint16_t conn_handle, int code)
{
    const char *msg = "ok";
    if (code == -1) {
        msg = "parse_error";
    } else if (code == -2) {
        msg = "invalid_params";
    } else if (code == -3) {
        msg = "save_failed";
    }

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"wifi_config_ack\",\"code\":%d,\"message\":\"%s\"}",
                     code, msg);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)n);
    if (om == NULL) {
        return;
    }
    int rc = ble_gatts_notify_custom(conn_handle, g_val_handle_ffe2, om);
    if (rc != 0) {
        ESP_LOGW(BLE_TAG, "发送 wifi_config_ack 失败: rc=%d", rc);
        os_mbuf_free_chain(om);
    } else {
        ESP_LOGI(BLE_TAG, "已发送 wifi_config_ack: code=%d", code);
    }
}

static void send_ble_ready_notify(uint16_t conn_handle)
{
    char buf[128];

    std::string mac = DeviceIDManager::get_mac_address();

    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"ble_ready\",\"message\":\"notify_subscribed\",\"device_id\":\"%s\",\"mac\":\"%s\"}",
        DeviceIDManager::get_device_id().c_str(),
        mac.c_str()
    );

    if (n <= 0 || n >= (int)sizeof(buf)) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)n);
    if (om == NULL) {
        return;
    }
    int rc = ble_gatts_notify_custom(conn_handle, g_val_handle_ffe2, om);
    if (rc != 0) {
        ESP_LOGW(BLE_TAG, "发送 ble_ready 失败: rc=%d", rc);
        os_mbuf_free_chain(om);
    } else {
        ESP_LOGI(BLE_TAG, "已发送 ble_ready: %s", buf);
    }
}

static void send_device_info_notify(uint16_t conn_handle)
{
    char buf[384];

    std::string mac = DeviceIDManager::get_mac_address();

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const char* chip_model;
    switch (chip_info.model) {
        case CHIP_ESP32:   chip_model = "ESP32"; break;
        case CHIP_ESP32S2: chip_model = "ESP32-S2"; break;
        case CHIP_ESP32S3: chip_model = "ESP32-S3"; break;
        case CHIP_ESP32C3: chip_model = "ESP32-C3"; break;
        case CHIP_ESP32C6: chip_model = "ESP32-C6"; break;
        case CHIP_ESP32H2: chip_model = "ESP32-H2"; break;
        default:            chip_model = "Unknown"; break;
    }

    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"type\":\"device_info\","
        "\"device_name\":\"SX_AIToy\","
        "\"mac\":\"%s\","
        "\"chip_model\":\"%s\","
        "\"firmware_version\":\"1.1.3\","
        "\"device_id\":\"%s\""
        "}",
        mac.c_str(),
        chip_model,
        DeviceIDManager::get_device_id().c_str()
    );

    if (n <= 0 || n >= (int)sizeof(buf)) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)n);
    if (om == NULL) {
        return;
    }

    int rc = ble_gatts_notify_custom(conn_handle, g_val_handle_ffe2, om);
    if (rc != 0) {
        os_mbuf_free_chain(om);
    } else {
        ESP_LOGI(BLE_TAG, "已发送 device_info: %s", buf);
    }
}

static int prov_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (attr_handle == g_val_handle_ffe2) {
            uint8_t z = 0;
            int rc = os_mbuf_append(ctxt->om, &z, 1);
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (ble_uuid_cmp(ctxt->chr->uuid, &g_prov_chr_write_uuid.u) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        {
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            ESP_LOGI(BLE_TAG, "收到 FFE1 写入，长度=%u", (unsigned)om_len);
            if (om_len == 0 || om_len > 512) {
                ESP_LOGW(BLE_TAG, "FFE1 写入长度非法: %u", (unsigned)om_len);
                send_wifi_config_ack(conn_handle, -1);
                return 0;
            }
            uint8_t wbuf[513];
            uint16_t out_len = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, wbuf, sizeof(wbuf) - 1, &out_len);
            if (rc != 0) {
                ESP_LOGW(BLE_TAG, "FFE1 解析 mbuf 失败: rc=%d", rc);
                send_wifi_config_ack(conn_handle, -1);
                return 0;
            }
            wbuf[out_len] = '\0';
            ESP_LOGI(BLE_TAG, "FFE1 载荷: %s", reinterpret_cast<const char*>(wbuf));

            if (g_pending_write_conn_handle != conn_handle) {
                reset_pending_write_payload(conn_handle);
            }
            if (!g_pending_write_payload.empty() && out_len > 0 && wbuf[0] == '{') {
                reset_pending_write_payload(conn_handle);
            }

            g_pending_write_payload.append(reinterpret_cast<const char *>(wbuf), out_len);
            ESP_LOGI(BLE_TAG, "FFE1 累积载荷长度: %u", (unsigned)g_pending_write_payload.size());
            if (g_pending_write_payload.size() > 512) {
                ESP_LOGW(BLE_TAG, "FFE1 累积载荷过长，重置缓冲");
                reset_pending_write_payload(conn_handle);
                send_wifi_config_ack(conn_handle, -1);
                return 0;
            }

            if (!is_complete_json_payload(g_pending_write_payload)) {
                ESP_LOGI(BLE_TAG, "FFE1 载荷未完整，继续等待后续分包");
                return 0;
            }

            ESP_LOGI(BLE_TAG, "FFE1 完整载荷: %s", g_pending_write_payload.c_str());

            std::string type_field;
            if (extract_json_string_field(g_pending_write_payload, "type", type_field)) {
                if (type_field == "get_device_info") {
                    ESP_LOGI(BLE_TAG, "收到获取设备信息请求");
                    send_device_info_notify(conn_handle);
                    reset_pending_write_payload(conn_handle);
                    return 0;
                }
            }

            int prc = provision_apply_wifi_json(g_pending_write_payload.c_str(), g_pending_write_payload.size());
            reset_pending_write_payload(conn_handle);
            send_wifi_config_ack(conn_handle, prc);
        }
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int prov_gatt_register(void)
{
    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(gatt_svr_svcs);
}

static void ble_app_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *device_name = "SX_AIToy";
    int rc;

    ble_svc_gap_device_name_set(device_name);

    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(0xffe0)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "设置广播字段失败; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(ble_addr_type, NULL, BLE_HS_FOREVER, &adv_params, prov_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "启动广播失败; rc=%d", rc);
        return;
    }
    ESP_LOGI(BLE_TAG, "BLE 可连接广播已启动，GATT FFE0/FFE1/FFE2，名称: %s", device_name);
}

static int prov_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ble_app_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        reset_pending_write_payload(0xffff);
        ble_app_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_ble_radio_held_for_wifi_scan) {
            return 0;
        }
        ble_app_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(BLE_TAG, "SUBSCRIBE conn=%d attr=%d notify=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == g_val_handle_ffe2 && event->subscribe.cur_notify) {
            send_ble_ready_notify(event->subscribe.conn_handle);
        }
        return 0;

    default:
        return 0;
    }
}

static void ble_app_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "确保地址失败; rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "推断地址类型失败; rc=%d", rc);
        return;
    }

    ble_app_advertise();
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(BLE_TAG, "蓝牙 Host 任务已启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_pause_for_wifi_scan(void)
{
#if CONFIG_BT_ENABLED
    if (!ble_gap_adv_active()) {
        return;
    }
    s_ble_radio_held_for_wifi_scan = true;
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(BLE_TAG, "扫描前停止 BLE 广播: rc=%d", rc);
    }
    vTaskDelay(pdMS_TO_TICKS(40));
#endif
}

void ble_resume_advertising_after_wifi_scan(void)
{
#if CONFIG_BT_ENABLED
    if (!s_ble_radio_held_for_wifi_scan) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ble_app_advertise();
    s_ble_radio_held_for_wifi_scan = false;
#endif
}

void start_ble_advertising(void)
{
    ESP_LOGI(BLE_TAG, "正在初始化 NimBLE（配网 GATT + 可连接广播）...");
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(BLE_TAG, "NimBLE 初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    // 关键：GATT 服务要在 host run/sync 前完成注册，否则部分手机会出现“已连接但服务表为空”。
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = prov_gatt_register();
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "注册 GATT 服务失败; rc=%d", rc);
        return;
    }

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_svc_gap_device_name_set("SX_AIToy");
    ESP_LOGI(BLE_TAG, "GATT FFE0/FFE1/FFE2 注册完成，等待同步后广播");

    nimble_port_freertos_init(ble_host_task);
}
