/**
 * @file wifi_manager.cc
 * @brief 📶 WiFi管理器实现文件 - 让ESP32轻松连上互联网
 * 
 * 这个文件实现了WiFi连接的全部逻辑，包括：
 * - 🔍 扫描和连接WiFi网络
 * - 🔄 连接失败后自动重试
 * - 🏠 获取DHCP分配的IP地址
 * - 📊 监控信号强度
 * 
 * 开发提示：请确保路由器开启了2.4GHz频段，ESP32不支持5GHz！
 */

#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_wifi_default.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include <cstring>
#include <sstream>

static const char *TAG = "WiFiManager";

// 🎯 静态成员初始化（这些变量在所有WiFiManager实例之间共享）
EventGroupHandle_t WiFiManager::s_wifi_event_group = NULL;  // 事件组句柄
int WiFiManager::s_retry_num = 0;                          // 当前重试次数
esp_ip4_addr_t WiFiManager::s_ip_addr = {0};               // IP地址结构体（STA模式）
esp_ip4_addr_t WiFiManager::s_ap_ip_addr = {0};            // IP地址结构体（AP模式）

static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_ap_netif = nullptr;
static SemaphoreHandle_t s_scan_mutex = NULL;

static esp_err_t ensure_network_stack_ready() {
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ 初始化TCP/IP协议栈失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ 创建事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_scan_mutex == NULL) {
        s_scan_mutex = xSemaphoreCreateMutex();
        if (s_scan_mutex == NULL) {
            ESP_LOGE(TAG, "❌ 创建WiFi扫描互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static esp_err_t ensure_sta_netif_ready() {
    if (s_sta_netif != nullptr) {
        return ESP_OK;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == nullptr) {
        ESP_LOGE(TAG, "❌ 创建STA接口失败");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t ensure_ap_netif_ready() {
    if (s_ap_netif != nullptr) {
        return ESP_OK;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(TAG, "❌ 创建AP接口失败");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void destroy_wifi_netifs() {
    if (s_sta_netif != nullptr) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
    }

    if (s_ap_netif != nullptr) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
    }
}

static void stop_and_deinit_wifi_driver() {
    wifi_mode_t current_mode;
    if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }

    destroy_wifi_netifs();
}

WiFiManager::WiFiManager(const std::string& ssid, const std::string& password, int max_retry)
    : ssid_(ssid), password_(password), max_retry_(max_retry), initialized_(false),
      ap_mode_(false), sta_connect_requested_(false), instance_any_id_(nullptr), instance_got_ip_(nullptr),
      instance_ap_started_(nullptr) {
}

WiFiManager::~WiFiManager() {
    if (initialized_) {
        disconnect();
    }
}

void WiFiManager::event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    WiFiManager* wifi_manager = static_cast<WiFiManager*>(arg);
    
    // 📡 AP模式事件处理
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "✅ WiFi AP已启动");
        return;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "🔌 WiFi AP已停止");
        return;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "📱 设备已连接到热点，MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);
        return;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "📱 设备已断开热点，MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);
        return;
    }
    
    // 🟢 WiFi驱动启动完成，开始连接（STA模式）
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (wifi_manager != nullptr && wifi_manager->sta_connect_requested_) {
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "STA接口已启动，当前仅用于扫描或配网辅助");
        }
    } 
    // 🔴 WiFi连接断开（可能是密码错误、信号太弱等）
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_manager != nullptr && wifi_manager->sta_connect_requested_ &&
            s_retry_num < wifi_manager->max_retry_) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "🔄 重试连接WiFi... (%d/%d)", s_retry_num, wifi_manager->max_retry_);
        } else if (wifi_manager != nullptr && wifi_manager->sta_connect_requested_) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else {
            ESP_LOGI(TAG, "STA已断开，但当前未请求联网，忽略此次事件");
        }
        ESP_LOGI(TAG, "❌ WiFi连接失败");
    } 
    // 🎉 成功获得IP地址，可以上网了！
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_ip_addr = event->ip_info.ip;
        ESP_LOGI(TAG, "🏠 获得IP地址: %d.%d.%d.%d",
                 (int)((s_ip_addr.addr >> 0) & 0xFF),
                 (int)((s_ip_addr.addr >> 8) & 0xFF),
                 (int)((s_ip_addr.addr >> 16) & 0xFF),
                 (int)((s_ip_addr.addr >> 24) & 0xFF));
        s_retry_num = 0;  // 重置重试计数器
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);  // 设置连接成功标志
        }
    }
}

esp_err_t WiFiManager::connect() {
    if (initialized_ && !ap_mode_) {
        ESP_LOGW(TAG, "⚠️ WiFi已经初始化");
        return ESP_OK;
    }
    
    sta_connect_requested_ = true;

    // 🎯 创建事件组（用于等待WiFi连接结果）
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "❌ 创建事件组失败");
        return ESP_FAIL;
    }
    
    esp_err_t ret = ensure_network_stack_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_sta_netif_ready();
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 🔧 初始化WiFi驱动（使用默认配置）
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 初始化WiFi驱动失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 🔔 注册事件处理函数
    // 当WiFi发生任何事件时，都会通知我们
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,    // 监听所有WiFi事件
                                                       &event_handler,
                                                       this,
                                                       &instance_any_id_));
    // 当获得IP地址时，也会通知我们
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP, // 监听获得IP事件
                                                       &event_handler,
                                                       this,
                                                       &instance_got_ip_));
    
    // 🔐 配置WiFi连接参数
    wifi_config_t wifi_config = {};
    // 复制WiFi名称（最多32个字符）
    std::strncpy((char*)wifi_config.sta.ssid, ssid_.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    // 复制WiFi密码（最多64个字符）
    std::strncpy((char*)wifi_config.sta.password, password_.c_str(), sizeof(wifi_config.sta.password) - 1);
    // 允许连接开放网络和常见加密网络，避免开放网络被错误拦截
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 支持WPA3加密（更高级的安全性）
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    // 🚀 设置WiFi工作模式并启动
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));      // 设为客户端模式
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));  // 应用配置
    ESP_ERROR_CHECK(esp_wifi_start());                      // 启动WiFi
    
    ESP_LOGI(TAG, "📶 WiFi初始化完成，正在连接到 %s", ssid_.c_str());
    
    // ⏳ 等待连接结果（会阻塞在这里直到连接成功或失败）
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,  // 等待这两个事件之一
                                          pdFALSE,                             // 不清除事件位
                                          pdFALSE,                             // 不需要两个事件都发生
                                          portMAX_DELAY);                      // 永久等待
    
    // 🎯 检查连接结果
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ WiFi连接成功: %s", ssid_.c_str());
        initialized_ = true;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "❌ WiFi连接失败: %s", ssid_.c_str());
        ESP_LOGI(TAG, "💡 提示：请检查WiFi名称和密码是否正确！");
        
        // 🧹 清理资源（释放内存，恢复状态）
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_any_id_ = nullptr;
        instance_got_ip_ = nullptr;
        stop_and_deinit_wifi_driver();
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        sta_connect_requested_ = false;
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "⚠️ 意外事件");
        return ESP_FAIL;
    }
}

void WiFiManager::disconnect() {
    if (!initialized_ && !ap_mode_) {
        return;
    }
    
    ESP_LOGI(TAG, "🔌 断开WiFi连接...");
    
    // 🔔 注销事件处理器（不再监听WiFi事件）
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
    if (instance_ap_started_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_ap_started_);
        instance_ap_started_ = nullptr;
    }
    
    // 🛑️ 停止WiFi驱动
    stop_and_deinit_wifi_driver();
    
    // 🧹 删除事件组（释放内存）
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    // 🔄 重置所有状态变量
    initialized_ = false;
    ap_mode_ = false;
    sta_connect_requested_ = false;
    s_retry_num = 0;
    s_ip_addr.addr = 0;
    s_ap_ip_addr.addr = 0;
    
    ESP_LOGI(TAG, "✅ WiFi已完全断开");
}

bool WiFiManager::isConnected() const {
    if (!initialized_ || !s_wifi_event_group) {
        return false;
    }
    
    // 🔍 检查事件组中的连接标志位
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

std::string WiFiManager::getIpAddress() const {
    if (!isConnected()) {
        return "";
    }
    
    // 🏠 将IP地址结构体转换为可读字符串
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&s_ip_addr));
    return std::string(ip_str);
}

int8_t WiFiManager::getRssi() const {
    if (!isConnected()) {
        return 0;
    }
    
    // 📊 获取当前连接的AP（接入点）信息
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;  // 返回信号强度（负数，越接近0信号越好）
    }
    return 0;
}

int WiFiManager::scanNetworks(wifi_ap_record_t* networks, uint16_t max_count) {
    if (networks == nullptr || max_count == 0) {
        return 0;
    }

    if (ensure_network_stack_ready() != ESP_OK) {
        return 0;
    }

    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ 获取WiFi扫描锁超时，放弃本次扫描");
        return 0;
    }
    
    ESP_LOGI(TAG, "🔍 开始扫描WiFi网络（2.4GHz）...");
    
    // 确保WiFi已初始化（即使未连接也可以扫描）
    esp_err_t ret;
    wifi_mode_t current_mode;
    ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK) {
        // WiFi未初始化，需要先初始化
        ESP_LOGI(TAG, "WiFi未初始化，正在初始化...");
        if (ensure_sta_netif_ready() != ESP_OK) {
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 初始化WiFi驱动失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 设置STA模式失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 启动WiFi失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }
        current_mode = WIFI_MODE_STA;
    } else if (current_mode == WIFI_MODE_AP) {
        // AP-only 模式下扫描前需要先补齐 STA 接口，否则容易在某些固件/机型上崩溃。
        if (ensure_sta_netif_ready() != ESP_OK) {
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }

        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 切换到APSTA模式失败: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_scan_mutex);
            return 0;
        }
        ESP_LOGI(TAG, "已从AP模式切换到APSTA模式，以便安全扫描附近WiFi");
        current_mode = WIFI_MODE_APSTA;
    }

    ret = esp_wifi_scan_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "停止旧扫描任务返回: %s", esp_err_to_name(ret));
    }
    
    // 配置扫描参数
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = nullptr;           // 扫描所有SSID
    scan_config.bssid = nullptr;          // 扫描所有BSSID
    scan_config.channel = 0;              // 扫描所有信道
    scan_config.show_hidden = false;      // 不显示隐藏网络
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;  // 主动扫描
#if CONFIG_BT_ENABLED
    /* 与 BLE/NimBLE 共存时须使用驱动默认主动扫描时间，自定义时长会触发 coexist 告警并易导致 AP 侧手机掉线 */
    scan_config.scan_time.active.min = 0;
    scan_config.scan_time.active.max = 0;
#else
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;
#endif
    
    // 启动扫描
    ret = esp_wifi_scan_start(&scan_config, true);  // true=阻塞等待完成
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ WiFi扫描启动失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_scan_mutex);
        return 0;
    }
    
    // 获取扫描结果数量
    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 获取扫描结果数量失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_scan_mutex);
        return 0;
    }
    
    // 限制返回数量
    if (ap_count > max_count) {
        ap_count = max_count;
    }
    
    // 获取扫描结果
    ret = esp_wifi_scan_get_ap_records(&ap_count, networks);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 获取扫描结果失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_scan_mutex);
        return 0;
    }
    
    ESP_LOGI(TAG, "✅ WiFi扫描完成，找到 %d 个网络", ap_count);
    xSemaphoreGive(s_scan_mutex);
    
    return ap_count;
}

esp_err_t WiFiManager::reconnect(const std::string& new_ssid, const std::string& new_password) {
    ESP_LOGI(TAG, "🔄 重新连接WiFi: SSID=%s", new_ssid.c_str());
    
    // 如果已经连接，先断开
    if (initialized_) {
        disconnect();
    }
    
    // 如果正在AP模式，先停止
    if (ap_mode_) {
        stop_ap_mode();
    }
    
    // 更新SSID和密码
    ssid_ = new_ssid;
    password_ = new_password;
    
    // 重新连接
    return connect();
}

esp_err_t WiFiManager::start_ap_mode(const std::string& ap_ssid, const std::string& ap_password) {
    ESP_LOGI(TAG, "📡 启动AP模式，热点名称: %s", ap_ssid.c_str());
    
    // 如果已有网络流程在运行，先完整清理，避免接口重复创建或状态残留。
    if (initialized_ || ap_mode_) {
        disconnect();
    }

    stop_and_deinit_wifi_driver();

    esp_err_t ret = ensure_network_stack_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    if (ensure_ap_netif_ready() != ESP_OK || ensure_sta_netif_ready() != ESP_OK) {
        return ESP_FAIL;
    }
    
    // 配置AP的IP地址
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);
    
    s_ap_ip_addr = ip_info.ip;
    
    // 初始化WiFi驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 初始化WiFi驱动失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册AP事件处理
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &event_handler,
                                                       this,
                                                       &instance_ap_started_));
    
    // 配置AP参数
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ap_ssid.c_str(), sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = ap_ssid.length();
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    
    if (ap_password.empty()) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "   热点类型: 开放网络（无需密码）");
    } else {
        strncpy((char*)wifi_config.ap.password, ap_password.c_str(), sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGI(TAG, "   热点类型: 需要密码");
    }
    
    sta_connect_requested_ = false;

    // 使用 APSTA 启动配网热点，这样后续扫描无需临时切模式，稳定性更高。
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ap_mode_ = true;
    initialized_ = true;
    ESP_LOGI(TAG, "✅ AP模式启动成功");
    ESP_LOGI(TAG, "   热点名称: %s", ap_ssid.c_str());
    ESP_LOGI(TAG, "   设备IP: 192.168.4.1");
    ESP_LOGI(TAG, "   请连接此热点，然后访问 http://192.168.4.1 进行配网");
    
    return ESP_OK;
}

esp_err_t WiFiManager::stop_ap_mode() {
    if (!ap_mode_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "🔌 停止AP模式");
    
    // 注销事件处理器
    if (instance_ap_started_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_ap_started_);
        instance_ap_started_ = nullptr;
    }

    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    stop_and_deinit_wifi_driver();
    
    ap_mode_ = false;
    initialized_ = false;
    sta_connect_requested_ = false;
    s_ap_ip_addr.addr = 0;
    ESP_LOGI(TAG, "✅ AP模式已停止");
    
    return ESP_OK;
}

std::string WiFiManager::get_ap_ip_address() const {
    if (!ap_mode_) {
        return "";
    }
    
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
             (int)((s_ap_ip_addr.addr >> 0) & 0xFF),
             (int)((s_ap_ip_addr.addr >> 8) & 0xFF),
             (int)((s_ap_ip_addr.addr >> 16) & 0xFF),
             (int)((s_ap_ip_addr.addr >> 24) & 0xFF));
    return std::string(ip_str);
}

esp_err_t WiFiManager::start_ap_sta_mode(const std::string& ap_ssid, 
                                         const std::string& ap_password,
                                         const std::string& sta_ssid,
                                         const std::string& sta_password) {
    ESP_LOGI(TAG, "📡 启动AP+STA模式");
    ESP_LOGI(TAG, "   AP热点: %s", ap_ssid.c_str());
    ESP_LOGI(TAG, "   STA WiFi: %s", sta_ssid.c_str());
    
    // 如果已有网络流程在运行，先完整清理。
    if (initialized_ || ap_mode_) {
        disconnect();
    }

    stop_and_deinit_wifi_driver();

    esp_err_t ret = ensure_network_stack_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    if (ensure_ap_netif_ready() != ESP_OK || ensure_sta_netif_ready() != ESP_OK) {
        ESP_LOGE(TAG, "❌ 创建网络接口失败");
        return ESP_FAIL;
    }
    
    // 配置AP的IP地址
    esp_netif_ip_info_t ap_ip_info;
    IP4_ADDR(&ap_ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ap_ip_info);
    esp_netif_dhcps_start(s_ap_netif);
    s_ap_ip_addr = ap_ip_info.ip;
    
    sta_connect_requested_ = !sta_ssid.empty();

    // 仅在需要连接上级路由器时才创建事件组。
    if (sta_connect_requested_ && s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            ESP_LOGE(TAG, "❌ 创建事件组失败");
            return ESP_FAIL;
        }
    } else if (sta_connect_requested_) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    
    // 初始化WiFi驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 初始化WiFi驱动失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册事件处理
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &event_handler,
                                                       this,
                                                       &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &event_handler,
                                                       this,
                                                       &instance_got_ip_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &event_handler,
                                                       this,
                                                       &instance_ap_started_));
    
    // 配置AP参数
    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ap_ssid.c_str(), sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = ap_ssid.length();
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    
    if (ap_password.empty()) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "   AP热点类型: 开放网络（无需密码）");
    } else {
        strncpy((char*)ap_config.ap.password, ap_password.c_str(), sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGI(TAG, "   AP热点类型: 需要密码");
    }
    
    // 配置STA参数（如果提供了SSID）
    wifi_config_t sta_config = {};
    if (sta_connect_requested_) {
        strncpy((char*)sta_config.sta.ssid, sta_ssid.c_str(), sizeof(sta_config.sta.ssid) - 1);
        if (!sta_password.empty()) {
            strncpy((char*)sta_config.sta.password, sta_password.c_str(), sizeof(sta_config.sta.password) - 1);
        }
        // 允许连接开放网络和常见加密网络，避免开放网络配网失败
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
    }
    
    // 设置AP+STA模式并启动
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    if (sta_connect_requested_) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ap_mode_ = true;
    initialized_ = true;
    
    // 如果提供了STA配置，开始连接
    if (sta_connect_requested_) {
        ESP_LOGI(TAG, "⏳ 正在连接WiFi: %s", sta_ssid.c_str());
        
        // 等待连接结果
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(30000));  // 最多等待30秒
        
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "✅ WiFi连接成功");
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGW(TAG, "⚠️ WiFi连接失败，但AP模式已启动");
            return ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "⚠️ WiFi连接超时，但AP模式已启动");
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "✅ AP+STA模式启动成功");
    ESP_LOGI(TAG, "   AP热点名称: %s", ap_ssid.c_str());
    ESP_LOGI(TAG, "   AP设备IP: 192.168.4.1");
    ESP_LOGI(TAG, "   请连接此热点，然后访问 http://192.168.4.1 进行配网");
    
    return ESP_OK;
}