
extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h" // 流缓冲区
#include "freertos/event_groups.h"  // 事件组
#include "mbedtls/base64.h"         // Base64编码库
#include "esp_timer.h"              // ESP定时器，用于获取时间戳
#include "esp_wn_iface.h"           // 唤醒词检测接口
#include "esp_wn_models.h"          // 唤醒词模型管理
#include "esp_mn_iface.h"           // 命令词识别接口
#include "esp_mn_models.h"          // 命令词模型管理
#include "esp_mn_speech_commands.h" // 命令词配置
#include "esp_process_sdkconfig.h"  // sdkconfig处理函数
#include "esp_vad.h"                // VAD接口
#include "esp_nsn_iface.h"          // 噪音抑制接口
#include "esp_nsn_models.h"         // 噪音抑制模型
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"                // ESP日志系统
#include "mock_voices/hi.h"         // 欢迎音频数据文件
#include "mock_voices/ok.h"         // 确认音频数据文件
#include "mock_voices/bye.h"     // 再见音频数据文件
#include "mock_voices/custom.h"     // 自定义音频数据文件
#include "mock_voices/end.h"
#include "mock_voices/start.h"
#include "mock_voices/newweekup.h"
#include "mock_voices/newsuccess.h"
#include "mock_voices/newloseinter.h"
#include "mock_voices/connected.h"
#include "driver/gpio.h"            // GPIO驱动
#include "nvs_flash.h"              // NVS存储
#include "nvs.h"                    // NVS操作接口
#include "esp_wifi.h"                // WiFi功能（用于获取MAC地址）
}

#include "audio_manager.h"          // 音频管理器
#include "wifi_manager.h"           // WiFi管理器
#include "websocket_client.h"        // WebSocket客户端
#include "power_control.h"              
#include "keyWeakUp.h"              // 按键唤醒
#include "config_server.h"           // 配网HTTP服务器
#include "device_id_manager.h"
#include "ble_adv.h"      // 设备ID管理器
static const char *TAG = "语音识别"; // 日志标签

// 🔌 硬件引脚定义
#define LED_GPIO GPIO_NUM_21 // LED指示灯连接到GPIO21（记得加限流电阻哦）
#define KEY_GPIO GPIO_NUM_11 // LED指示灯连接到GPIO11（记得加限流电阻哦）
// 📡 网络配置（已改为从NVS读取，首次使用会进入AP配网模式）
// 如果NVS中没有WiFi配置，设备会创建热点"ESP_xxxx"，用户连接后访问192.168.4.1进行配网

// 🌐 WebSocket服务器配置
#define WS_URI "ws://192.168.1.64:8000" // 请改IP地址

// WiFi和WebSocket管理器
static WiFiManager* wifi_manager = nullptr;
static WebSocketClient* websocket_client = nullptr;

// 🎮 系统状态机（程序的不同工作阶段）
typedef enum
{
    STATE_WAITING_WAKEUP = 0,   // 休眠状态：等待用户长按按键唤醒
    STATE_RECORDING = 1,        // 录音状态：正在录制用户说话
    STATE_WAITING_RESPONSE = 2, // 等待状态：等待服务器返回AI响应
} system_state_t;

// 📡 网络状态标志
// 该变量会被 WebSocket 回调任务写入、主任务读取，跨任务/跨核共享，需保持可见性
static bool received_ping = false; // 标记是否收到了ping包（用于停止接收新音频数据）

// 🎤 本地命令词ID（快速响应，无需联网）
// 这些ID来自ESP-SR语音识别框架的预定义命令词表
#define COMMAND_TURN_OFF_LIGHT 308 // "帮我关灯" - 关闭LED
#define COMMAND_TURN_ON_LIGHT 309  // "帮我开灯" - 点亮LED
#define COMMAND_BYE_BYE 314        // "拜拜" - 退出对话
#define COMMAND_CUSTOM 315         // "现在安全屋情况如何" - 演示用

// 📝 命令词配置结构（告诉系统要识别哪些命令）
typedef struct
{
    int command_id;              // 命令的唯一标识符
    const char *pinyin;          // 命令的拼音（用于语音识别匹配）
    const char *description;     // 命令的中文描述（方便理解）
} command_config_t;

// 自定义命令词列表
static const command_config_t custom_commands[] = {
    {COMMAND_TURN_ON_LIGHT, "bang wo kai deng", "帮我开灯"},
    {COMMAND_TURN_OFF_LIGHT, "bang wo guan deng", "帮我关灯"},
    {COMMAND_BYE_BYE, "bai bai", "拜拜"},
    {COMMAND_CUSTOM, "xian zai an quan wu qing kuang ru he", "现在安全屋情况如何"},
};

#define CUSTOM_COMMANDS_COUNT (sizeof(custom_commands) / sizeof(custom_commands[0]))

// 全局变量
static system_state_t current_state = STATE_WAITING_WAKEUP;
static TickType_t last_audio_data_tick = 0;     // 上次收到音频数据的时间
static bool is_processing_audio = false; // 是否正在处理音频数据（防止多线程冲突）
static bool is_force_standby_transition = false; // 长按进入待命过程中的音频/网络屏蔽标志
static bool is_force_recording_transition = false; // 短按强制进入录音过程中的旧音频/旧尾包屏蔽标志
static bool pending_recording_started_after_reconnect = false; // 短按中断后，重连成功时补发 recording_started
static bool is_reprovision_transition = false; // 进入重新配网过程中的播报屏蔽标志

static void configure_runtime_log_levels() {
    // 保留业务关键日志，压低 WiFi/BLE 底层刷屏输出。
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("BLE_INIT", ESP_LOG_WARN);
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    esp_log_level_set("httpd", ESP_LOG_WARN);
    esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
}

/**
 * @brief 处理WiFi扫描请求
 */
static void handle_wifi_scan_request(WebSocketClient* ws_client) {
    if (ws_client == nullptr || !ws_client->isConnected()) {
        ESP_LOGE(TAG, "WebSocket未连接，无法发送扫描结果");
        return;
    }
    
    ESP_LOGI(TAG, "📡 开始扫描WiFi网络...");
    
    // 分配扫描结果缓冲区
    const uint16_t MAX_AP = 20;
    wifi_ap_record_t ap_records[MAX_AP];
    
    // 执行扫描
    int ap_count = WiFiManager::scanNetworks(ap_records, MAX_AP);
    ESP_LOGI(TAG, "📡 WiFi扫描完成，找到 %d 个网络", ap_count);
    
    if (ap_count == 0) {
        // 没有找到网络，发送空结果
        const char* empty_response = "{\"event\":\"wifi_scan_results\",\"networks\":[]}";
        ws_client->sendText(empty_response);
        ESP_LOGI(TAG, "✅ WiFi扫描结果已发送: 0 个网络");
        return;
    }
    
    // 使用动态分配避免栈溢出（每个网络约100字节，最多20个网络）
    // 估算大小：基础JSON + 每个网络约150字节
    size_t response_size = 100 + (ap_count * 200);
    char* response = (char*)malloc(response_size);
    if (response == nullptr) {
        ESP_LOGE(TAG, "❌ 内存分配失败，无法发送扫描结果");
        return;
    }
    
    int pos = snprintf(response, response_size, 
                      "{\"event\":\"wifi_scan_results\",\"networks\":[");
    
    for (int i = 0; i < ap_count && pos < (int)(response_size - 200); i++) {
        if (i > 0) {
            pos += snprintf(response + pos, response_size - pos, ",");
        }
        
        // 获取加密类型
        const char* auth_str = "OPEN";
        if (ap_records[i].authmode == WIFI_AUTH_WEP) auth_str = "WEP";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA_PSK) auth_str = "WPA_PSK";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA2_PSK) auth_str = "WPA2_PSK";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA_WPA2_PSK) auth_str = "WPA_WPA2_PSK";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA3_PSK) auth_str = "WPA3_PSK";
        
        pos += snprintf(response + pos, response_size - pos,
                       "{\"ssid\":\"%s\",\"rssi\":%d,\"authmode\":%d,\"auth_str\":\"%s\"}",
                       ap_records[i].ssid, ap_records[i].rssi, ap_records[i].authmode, auth_str);
        
        ESP_LOGI(TAG, "   网络 %d: %s (RSSI: %d, 加密: %s)", i+1, ap_records[i].ssid, ap_records[i].rssi, auth_str);
    }
    
    pos += snprintf(response + pos, response_size - pos, "]}");
    
    // 发送响应
    bool sent = ws_client->sendText(response);
    if (sent) {
        ESP_LOGI(TAG, "✅ WiFi扫描结果已发送: %d 个网络 (JSON长度: %d 字节)", ap_count, pos);
    } else {
        ESP_LOGE(TAG, "❌ WiFi扫描结果发送失败");
    }
    
    // 释放内存
    free(response);
}

/**
 * @brief 处理清除WiFi配置请求（释放设备）
 */
static void handle_wifi_clear_config_request(WebSocketClient* ws_client) {
    if (ws_client == nullptr || !ws_client->isConnected()) {
        ESP_LOGE(TAG, "WebSocket未连接，无法处理清除配置请求");
        return;
    }
    
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "🔄 开始执行释放设备流程...");
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    
    // ① 先发送确认消息（在断开WiFi之前）
    ESP_LOGI(TAG, "步骤1/6: 发送确认消息（在断开WiFi之前）...");
    const char* confirm_msg = "{\"event\":\"wifi_clear_config_result\",\"success\":true,\"message\":\"WiFi配置已清除，设备将在3秒后重启\"}";
    int sent_bytes = ws_client->sendText(confirm_msg);
    if (sent_bytes > 0) {
        ESP_LOGI(TAG, "✅ 确认消息已发送 (%d 字节)", sent_bytes);
    } else {
        ESP_LOGW(TAG, "⚠️ 确认消息发送失败（可能WebSocket已断开）");
    }
    
    // 等待一下，确保消息发送完成
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // ② 断开当前WiFi连接
    ESP_LOGI(TAG, "步骤2/6: 断开WiFi连接...");
    if (wifi_manager != nullptr) {
        ESP_LOGI(TAG, "📡 正在断开WiFi连接...");
        wifi_manager->disconnect();
        // 等待WiFi完全断开
        vTaskDelay(pdMS_TO_TICKS(2000));  // 增加到2秒，确保完全断开
        ESP_LOGI(TAG, "✅ WiFi已断开");
    } else {
        ESP_LOGW(TAG, "⚠️ WiFi管理器为空，跳过断开步骤");
    }
    
    // ③ 清除NVS中的WiFi配置
    ESP_LOGI(TAG, "步骤3/6: 清除NVS中的WiFi配置...");
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        // 删除 SSID、密码及配网扩展字段，避免残留脏数据
        esp_err_t ret1 = nvs_erase_key(nvs_handle, "ssid");
        esp_err_t ret2 = nvs_erase_key(nvs_handle, "password");
        (void)nvs_erase_key(nvs_handle, "bssid");
        (void)nvs_erase_key(nvs_handle, "security");
        
        ESP_LOGI(TAG, "删除ssid键: %s", esp_err_to_name(ret1));
        ESP_LOGI(TAG, "删除password键: %s", esp_err_to_name(ret2));
        
        ret = nvs_commit(nvs_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ NVS提交失败: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "✅ NVS提交成功");
        }
        nvs_close(nvs_handle);
        
        // ④ 验证清除是否成功
        ESP_LOGI(TAG, "步骤4/6: 验证清除结果...");
        nvs_handle_t verify_handle;
        if (nvs_open("wifi_config", NVS_READONLY, &verify_handle) == ESP_OK) {
            size_t required_size = 32;
            char test_ssid[32] = {0};
            esp_err_t verify_ret = nvs_get_str(verify_handle, "ssid", test_ssid, &required_size);
            nvs_close(verify_handle);
            
            if (verify_ret == ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGI(TAG, "✅ 验证成功：NVS中的WiFi配置已完全清除");
            } else {
                ESP_LOGW(TAG, "⚠️ 验证失败：NVS中仍存在WiFi配置，尝试删除整个命名空间");
                // 尝试删除整个命名空间
                nvs_handle_t erase_handle;
                if (nvs_open("wifi_config", NVS_READWRITE, &erase_handle) == ESP_OK) {
                    nvs_erase_all(erase_handle);
                    nvs_commit(erase_handle);
                    nvs_close(erase_handle);
                    ESP_LOGI(TAG, "✅ 已删除整个wifi_config命名空间");
                }
            }
        }
        
        // ⑤ 等待3秒后重启（给服务器时间接收消息）
        ESP_LOGI(TAG, "步骤5/6: 等待3秒后重启...");
        ESP_LOGI(TAG, "⏳ 倒计时: 3秒...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "⏳ 倒计时: 2秒...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "⏳ 倒计时: 1秒...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        ESP_LOGI(TAG, "═══════════════════════════════════════");
        ESP_LOGI(TAG, "🔄 设备即将重启，将重新进入AP配网模式...");
        ESP_LOGI(TAG, "   重启后，请连接设备热点：ESP_xxxx");
        ESP_LOGI(TAG, "   然后访问：http://192.168.4.1");
        ESP_LOGI(TAG, "═══════════════════════════════════════");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "❌ 打开NVS失败: %s", esp_err_to_name(ret));
        const char* error_msg = "{\"event\":\"wifi_clear_config_result\",\"success\":false,\"message\":\"清除配置失败\"}";
        ws_client->sendText(error_msg);
    }
}

/**
 * @brief 处理WiFi连接请求
 */
static void handle_wifi_connect_request(WebSocketClient* ws_client, const char* json_str) {
    if (ws_client == nullptr || !ws_client->isConnected()) {
        ESP_LOGE(TAG, "WebSocket未连接，无法处理连接请求");
        return;
    }
    
    // 简单的JSON解析（提取ssid和password）
    const char* ssid_start = strstr(json_str, "\"ssid\":\"");
    const char* password_start = strstr(json_str, "\"password\":\"");
    
    if (ssid_start == nullptr) {
        ESP_LOGE(TAG, "JSON中未找到ssid字段");
        ws_client->sendText("{\"event\":\"wifi_connect_result\",\"success\":false,\"message\":\"未找到SSID\"}");
        return;
    }
    
    // 提取SSID
    ssid_start += 8;  // 跳过 "ssid":"
    char ssid[33] = {0};
    int i = 0;
    while (*ssid_start != '"' && *ssid_start != '\0' && i < 32) {
        ssid[i++] = *ssid_start++;
    }
    ssid[i] = '\0';
    
    // 提取密码
    char password[65] = {0};
    if (password_start != nullptr) {
        password_start += 11;  // 跳过 "password":"
        i = 0;
        while (*password_start != '"' && *password_start != '\0' && i < 64) {
            password[i++] = *password_start++;
        }
        password[i] = '\0';
    }
    
    ESP_LOGI(TAG, "尝试连接WiFi: SSID=%s", ssid);
    
    // 重新连接WiFi（支持已配网设备切换WiFi）
    if (wifi_manager != nullptr) {
        esp_err_t ret = wifi_manager->reconnect(ssid, password);
        
        if (ret == ESP_OK) {
            // 保存新的WiFi配置到NVS（支持WiFi切换）
            nvs_handle_t nvs_handle;
            esp_err_t nvs_ret = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
            if (nvs_ret == ESP_OK) {
                (void)nvs_erase_key(nvs_handle, "bssid");
                (void)nvs_erase_key(nvs_handle, "security");
                nvs_set_str(nvs_handle, "ssid", ssid);
                nvs_set_str(nvs_handle, "password", password);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
                ESP_LOGI(TAG, "✅ 新WiFi配置已保存到NVS");
            }
            
            std::string ip = wifi_manager->getIpAddress();
            char response[256];
            snprintf(response, sizeof(response),
                    "{\"event\":\"wifi_connect_result\",\"success\":true,\"message\":\"连接成功，IP地址: %s\"}",
                    ip.c_str());
            ws_client->sendText(response);
            ESP_LOGI(TAG, "✅ WiFi连接成功: %s", ip.c_str());
        } else {
            ws_client->sendText("{\"event\":\"wifi_connect_result\",\"success\":false,\"message\":\"连接失败，请检查密码\"}");
            ESP_LOGE(TAG, "❌ WiFi连接失败");
        }
    } else {
        ws_client->sendText("{\"event\":\"wifi_connect_result\",\"success\":false,\"message\":\"WiFi管理器未初始化\"}");
    }
}
static esp_mn_iface_t *multinet __attribute__((unused)) = NULL;
static model_iface_data_t *mn_model_data __attribute__((unused)) = NULL;
// 命令超时相关变量（预留，未来可能使用）
// static TickType_t command_timeout_start = 0;
// static const TickType_t COMMAND_TIMEOUT_MS = 5000; // 5秒超时

// VAD（语音活动检测）相关变量
static vad_handle_t vad_inst __attribute__((unused)) = NULL;

// NS（噪音抑制）相关变量  
static esp_nsn_iface_t *nsn_handle __attribute__((unused)) = NULL;
static esp_nsn_data_t *nsn_model_data __attribute__((unused)) = NULL;

// 音频参数
#define SAMPLE_RATE 16000 // 采样率 16kHz

// 音频管理器
static AudioManager* audio_manager = nullptr;

// VAD（语音活动检测）相关变量
static bool vad_speech_detected __attribute__((unused)) = false;
static int vad_silence_frames __attribute__((unused)) = 0;
static const int VAD_SILENCE_FRAMES_REQUIRED = 20; // VAD检测到静音的帧数阈值（约600ms）

// 💬 连续对话功能相关变量
// 连续对话模式：第一次对话后，不需要再说唤醒词就能继续对话
static bool is_continuous_conversation __attribute__((unused)) = false;  // 是否在连续对话中
static TickType_t recording_timeout_start __attribute__((unused)) = 0;  // 开始计时的时间点
#define RECORDING_TIMEOUT_MS 10000              // 等待说话超时（10秒没说话就退出）
static bool user_started_speaking __attribute__((unused)) = false;      // 用户是否已经开始说话

static void send_device_register_payload() {
    if (websocket_client == nullptr || !websocket_client->isConnected()) {
        return;
    }
    std::string real_mac_address = DeviceIDManager::get_mac_address();
    char device_info[640];
    snprintf(device_info, sizeof(device_info),
            "{\"event\":\"device_register\",\"real_mac_address\":\"%s\",\"device_model\":\"ESP32-S3\"}",
            real_mac_address.c_str());
    websocket_client->sendText(device_info);
    ESP_LOGI(TAG, "📤 已发送设备注册信息: real_mac=%s", real_mac_address.c_str());
}

/**
 * @brief WebSocket事件处理函数
 * 
 * 当WebSocket连接发生各种事件时（连接成功、收到数据、断开等），
 * 这个函数会被自动调用来处理这些事件。
 * 
 * @param event 事件数据，包含事件类型和相关数据
 */
static void on_websocket_event(const WebSocketClient::EventData& event)
{
    switch (event.type)
    {
        case WebSocketClient::EventType::CONNECTED:
            ESP_LOGI(TAG, "🔗 WebSocket已连接");

            // 短按中断后会强制重建连接，这里在新连接建立后补发 recording_started，
            // 从根上切断上一轮回复与这一轮录音的混流风险。
            if (pending_recording_started_after_reconnect &&
                is_force_recording_transition &&
                current_state == STATE_RECORDING &&
                websocket_client != nullptr &&
                websocket_client->isConnected())
            {
                const char* start_msg = "{\"event\":\"recording_started\"}";
                websocket_client->sendText(start_msg);
                ESP_LOGI(TAG, "重连成功后补发 recording_started");
                pending_recording_started_after_reconnect = false;

                // 补发断网期间录制的音频（避免吞字）
                if (audio_manager != nullptr) {
                    size_t rec_len = 0;
                    const int16_t* rec_buf = audio_manager->getRecordingBuffer(rec_len);
                    if (rec_buf != nullptr && rec_len > 0) {
                        size_t bytes_to_send = rec_len * sizeof(int16_t);
                        websocket_client->sendBinary((const uint8_t*)rec_buf, bytes_to_send);
                        ESP_LOGI(TAG, "补发断网期间录制的音频: %zu 字节", bytes_to_send);
                    }
                }
            }
            
            // 🔄 如果正在流式播放，标记网络重连（需要重新预缓冲）
            if (audio_manager != nullptr && audio_manager->isStreamingActive()) {
                ESP_LOGI(TAG, "检测到网络重连，标记需要重新预缓冲");
                audio_manager->setNetworkReconnected();
            }
            
            // 设置WebSocket客户端指针（用于发送加速请求）
            if (audio_manager != nullptr) {
                audio_manager->setWebSocketClient(websocket_client);
            }
            
            // 每次 WebSocket 连接建立（含短按中断后的重连）统一发送注册信息。
            send_device_register_payload();
            break;

        case WebSocketClient::EventType::DISCONNECTED:
            ESP_LOGI(TAG, "🔌 WebSocket已断开");

            // 长按强制进入待命时，disconnect 是预期动作。
            // 此时不要再额外停止当前提示音，避免"卡一下"或把待命提示音截断。
            if (is_force_standby_transition) {
                break;
            }

            // 短按强制进入录音时，disconnect 是为了切断上一轮对话的尾包。
            // 这里不要把状态重置回待唤醒，也不要再次触发停音。
            if (is_force_recording_transition) {
                break;
            }
            
            // 🛑 紧急停止所有音频播放
            if (audio_manager != nullptr) {
                ESP_LOGI(TAG, "检测到断开，紧急停止所有音频");
                audio_manager->emergencyStopAudio();
            }
            
            // 重置状态
            if (current_state == STATE_WAITING_RESPONSE || current_state == STATE_RECORDING) {
                current_state = STATE_WAITING_WAKEUP;
                ESP_LOGI(TAG, "返回等待唤醒状态");
            }
            break;

        case WebSocketClient::EventType::DATA_BINARY:
        {
            // 强制进入待命过程中，丢弃所有服务器音频，避免待命提示音后又播旧残音
            if (is_force_standby_transition) {
                break;
            }

            // 短按强制进入录音后，丢弃上一轮回复的晚到音频分片，避免污染下一轮播放
            if (is_force_recording_transition) {
                break;
            }

            // 检查是否收到了ping包，如果收到则停止接收新数据
            if (received_ping) {
                break;
            }
            
            ESP_LOGI(TAG, "收到WebSocket二进制数据，长度: %zu 字节", event.data_len);
            
            // 标记开始处理
            is_processing_audio = true;
            last_audio_data_tick = xTaskGetTickCount(); // 更新最后收到数据的时间

            // 使用AudioManager处理WebSocket音频数据
            if (audio_manager != nullptr && event.data_len > 0 && current_state == STATE_WAITING_RESPONSE) {
                // 如果还没开始流式播放，初始化
                if (!audio_manager->isStreamingActive()) {
                    ESP_LOGI(TAG, "🎵 开始流式音频播放");
                    audio_manager->startStreamingPlayback();
                }
                
                // 添加音频数据到流式播放队列
                bool added = audio_manager->addStreamingAudioChunk(event.data, event.data_len);
                
                if (added) {
                    ESP_LOGD(TAG, "添加流式音频块: %zu 字节", event.data_len);
                    
                    // 检查流式播放状态（动态缓冲水位监控、缓冲区空转检测等）
                    if (!audio_manager->checkStreamingStatus()) {
                        ESP_LOGI(TAG, "流式播放状态异常，停止播放");
                        audio_manager->finishStreamingPlayback();
                        audio_manager->setStreamingComplete();
                    }
                } else {
                    ESP_LOGW(TAG, "流式音频缓冲区满");
                }
            }
            
            // 标记处理结束
            last_audio_data_tick = xTaskGetTickCount(); // 再次更新时间，防止处理耗时导致误判
            is_processing_audio = false;
        }
        break;
        
        case WebSocketClient::EventType::PING:
            // 不再使用PING事件判断，改为通过JSON消息中的event字段判断
            break;

        case WebSocketClient::EventType::PONG:
            // 收到pong响应，忽略
            break;

        case WebSocketClient::EventType::DATA_TEXT:
        {
            // 强制进入待命过程中，忽略控制消息（特别是 ping），防止旧会话尾包影响后续状态
            if (is_force_standby_transition) {
                break;
            }

            // 短按强制进入录音后，忽略上一轮回复的控制尾包（尤其 ping）
            if (is_force_recording_transition) {
                break;
            }

            // JSON数据处理（用于WiFi配置等控制命令）
            if (event.data && event.data_len > 0) {
                // 创建临时缓冲区
                char *json_str = (char *)malloc(event.data_len + 1);
                if (json_str) {
                    memcpy(json_str, event.data, event.data_len);
                    json_str[event.data_len] = '\0';
                    ESP_LOGI(TAG, "📨 收到JSON消息: %s", json_str);
                    
                    // 简单的JSON解析（查找event字段）
                    // 使用更灵活的匹配方式，支持不同的JSON格式
                    const char* event_str = strstr(json_str, "event");
                    if (event_str != nullptr) {
                        // 检查是否是wifi_clear_config_request（优先级最高，因为需要立即处理）
                        if (strstr(json_str, "wifi_clear_config_request") != nullptr) {
                            ESP_LOGI(TAG, "🔄 收到清除WiFi配置请求（释放设备）");
                            handle_wifi_clear_config_request(websocket_client);
                        }
                        // 检查是否是wifi_scan_request
                        else if (strstr(json_str, "wifi_scan_request") != nullptr) {
                            ESP_LOGI(TAG, "📡 收到WiFi扫描请求");
                            handle_wifi_scan_request(websocket_client);
                        }
                        // 检查是否是wifi_connect_request
                        else if (strstr(json_str, "wifi_connect_request") != nullptr) {
                            ESP_LOGI(TAG, "📡 收到WiFi连接请求");
                            handle_wifi_connect_request(websocket_client, json_str);
                        }
                        // 检查是否是设备注册确认
                        else if (strstr(json_str, "device_registered") != nullptr) {
                            ESP_LOGI(TAG, "✅ 收到设备注册确认");
                        }
                        // 检查是否是ping事件
                        else if (strstr(json_str, "ping") != nullptr) {
                            // 只在第一次收到ping包时设置标志，避免重复处理
                            if (!received_ping) {
                                ESP_LOGI(TAG, "📨 收到JSON消息: {\"event\": \"ping\"}");
                                received_ping = true;
                            }
                        } else {
                            ESP_LOGD(TAG, "收到其他JSON事件，忽略");
                        }
                    } else {
                        ESP_LOGW(TAG, "JSON消息中没有找到event字段");
                    }
                    
                    free(json_str);
                }
            }
            break;
        }

        case WebSocketClient::EventType::ERROR:
            ESP_LOGI(TAG, "❌ WebSocket错误");
            break;
            
        default:
            break;
    }
}

/**
 * @brief 初始化LED指示灯
 *
 * 💡 这个函数会把GPIO21设置为输出模式，用来控制LED灯的亮灭。
 * 初始状态为关闭（低电平）。
 * 
 * 注意：LED需要串联一个限流电阻（如220Ω）以保护LED和GPIO。
 */
static void init_led(void)
{
    ESP_LOGI(TAG, "正在初始化外接LED (GPIO21)...");

    // 配置GPIO21为输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),    // 设置GPIO21
        .mode = GPIO_MODE_OUTPUT,              // 输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,     // 禁用上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // 禁用下拉
        .intr_type = GPIO_INTR_DISABLE         // 禁用中断
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "外接LED GPIO初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    // 初始状态设置为关闭（低电平）
    gpio_set_level(LED_GPIO, 0);
    ESP_LOGI(TAG, "✓ 外接LED初始化成功，初始状态：关闭");
}

static void led_turn_on(void)
{
    gpio_set_level(LED_GPIO, 1);
    ESP_LOGI(TAG, "外接LED点亮");
}

static void led_turn_off(void)
{
    gpio_set_level(LED_GPIO, 0);
    ESP_LOGI(TAG, "外接LED熄灭");
}

// 实时流式传输标志
static bool is_realtime_streaming = false;

/**
 * @brief 配置本地命令词识别
 *
 * 🎆 这个函数会告诉语音识别系统要识别哪些中文命令。
 * 这些命令在本地运行，不需要联网，响应速度快。
 * 
 * 工作流程：
 * 1. 清空旧的命令词列表
 * 2. 添加我们定义的新命令词（如"帮我开灯"）
 * 3. 更新到识别模型中
 *
 * @param multinet 语音识别的接口对象
 * @param mn_model_data 识别模型的数据
 * @return ESP_OK=成功，ESP_FAIL=失败
 */
static esp_err_t configure_custom_commands(esp_mn_iface_t *multinet, model_iface_data_t *mn_model_data)
{
    ESP_LOGI(TAG, "开始配置自定义命令词...");

    // 首先尝试从sdkconfig加载默认命令词配置
    esp_mn_commands_update_from_sdkconfig(multinet, mn_model_data);

    // 清除现有命令词，重新开始
    esp_mn_commands_clear();

    // 分配命令词管理结构
    esp_err_t ret = esp_mn_commands_alloc(multinet, mn_model_data);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "命令词管理结构分配失败: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    // 添加自定义命令词
    int success_count = 0;
    int fail_count = 0;

    for (int i = 0; i < CUSTOM_COMMANDS_COUNT; i++)
    {
        const command_config_t *cmd = &custom_commands[i];

        ESP_LOGI(TAG, "添加命令词 [%d]: %s (%s)",
                 cmd->command_id, cmd->description, cmd->pinyin);

        // 添加命令词
        esp_err_t ret_cmd = esp_mn_commands_add(cmd->command_id, cmd->pinyin);
        if (ret_cmd == ESP_OK)
        {
            success_count++;
            ESP_LOGI(TAG, "✓ 命令词 [%d] 添加成功", cmd->command_id);
        }
        else
        {
            fail_count++;
            ESP_LOGE(TAG, "✗ 命令词 [%d] 添加失败: %s",
                     cmd->command_id, esp_err_to_name(ret_cmd));
        }
    }

    // 更新命令词到模型
    ESP_LOGI(TAG, "更新命令词到模型...");
    esp_mn_error_t *error_phrases = esp_mn_commands_update();
    if (error_phrases != NULL && error_phrases->num > 0)
    {
        ESP_LOGW(TAG, "有 %d 个命令词更新失败:", error_phrases->num);
        for (int i = 0; i < error_phrases->num; i++)
        {
            ESP_LOGW(TAG, "  失败命令 %d: %s",
                     error_phrases->phrases[i]->command_id,
                     error_phrases->phrases[i]->string);
        }
    }

    // 打印配置结果
    ESP_LOGI(TAG, "命令词配置完成: 成功 %d 个, 失败 %d 个", success_count, fail_count);

    // 打印激活的命令词
    ESP_LOGI(TAG, "当前激活的命令词列表:");
    multinet->print_active_speech_commands(mn_model_data);

    // 打印支持的命令列表
    ESP_LOGI(TAG, "支持的语音命令:");
    for (int i = 0; i < CUSTOM_COMMANDS_COUNT; i++)
    {
        const command_config_t *cmd = &custom_commands[i];
        ESP_LOGI(TAG, "  ID=%d: '%s'", cmd->command_id, cmd->description);
    }

    return (fail_count == 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 根据命令ID获取中文说明
 *
 * 🔍 这是一个工具函数，用来查找命令ID对应的中文说明。
 * 比如：309 -> "帮我开灯"
 * 
 * @param command_id 命令的数字ID
 * @return 命令的中文说明文字
 */
static const char *get_command_description(int command_id)
{
    for (int i = 0; i < CUSTOM_COMMANDS_COUNT; i++)
    {
        if (custom_commands[i].command_id == command_id)
        {
            return custom_commands[i].description;
        }
    }
    return "未知命令";
}

/**
 * @brief 播放音频文件
 *
 * 🔊 这个函数会通过扬声器播放指定的音频数据。
 * 使用AudioManager管理音频播放，确保不会与其他音频冲突。
 * 
 * @param audio_data 要播放的音频数据（PCM格式）
 * @param data_len 音频数据的字节数
 * @param description 音频的描述（如"欢迎音频"）
 * @return ESP_OK=播放成功
 */
static esp_err_t play_audio_with_stop(const uint8_t *audio_data, size_t data_len, const char *description)
{
    if (audio_manager != nullptr) {
        return audio_manager->playAudio(audio_data, data_len, description);
    }
    return ESP_ERR_INVALID_STATE;
}

/**
 * @brief 退出对话模式
 *
 * 👋 当用户说"拜拜"或对话超时后，调用这个函数结束对话。
 * 
 * 执行步骤：
 * 1. 播放"再见"的音频
 * 2. 断开WebSocket连接
 * 3. 清理所有状态
 * 4. 回到等待唤醒词的初始状态
 */
static void execute_exit_logic(void)
{
    // 先进入"强制待命过渡"状态，屏蔽旧会话音频继续进入缓冲区
    is_force_standby_transition = true;
    is_force_recording_transition = false;
    pending_recording_started_after_reconnect = false;
    current_state = STATE_WAITING_WAKEUP;
    received_ping = false;
    is_realtime_streaming = false;

    // 先停止播放/接收并清空缓冲，再播放待命提示音
    if (audio_manager != nullptr) {
        audio_manager->emergencyStopAudio();
        audio_manager->stopRecording();
        audio_manager->clearRecordingBuffer();
    }

    // 先断开WebSocket，防止播放待命提示音期间继续收到服务器旧音频
    if (websocket_client != nullptr && websocket_client->isConnected()) {
        websocket_client->disconnect();
    }

    ESP_LOGI(TAG, "播放再见音频...");
    play_audio_with_stop(end, end_len, "结尾音频");

    // 重置所有状态
    current_state = STATE_WAITING_WAKEUP;
    is_continuous_conversation = false;
    user_started_speaking = false;
    recording_timeout_start = 0;
    vad_speech_detected = false;
    vad_silence_frames = 0;
    is_force_standby_transition = false;
    
    ESP_LOGI(TAG, "返回等待唤醒状态，请长按按键唤醒设备");
}

static void enter_wifi_reprovision_mode(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "📡 检测到连续三击，准备进入AP配网模式");
    ESP_LOGI(TAG, "═══════════════════════════════════════");

    is_reprovision_transition = true;
    current_state = STATE_WAITING_WAKEUP;
    is_continuous_conversation = false;
    user_started_speaking = false;
    recording_timeout_start = 0;
    received_ping = false;
    is_realtime_streaming = false;
    is_force_recording_transition = false;
    is_force_standby_transition = false;
    pending_recording_started_after_reconnect = false;
    vad_speech_detected = false;
    vad_silence_frames = 0;

    if (audio_manager != nullptr) {
        audio_manager->emergencyStopAudio();
        audio_manager->stopRecording();
        audio_manager->clearRecordingBuffer();
    }

    if (websocket_client != nullptr && websocket_client->isConnected()) {
        websocket_client->disconnect();
    }

    ESP_LOGI(TAG, "播放进入配网模式提示音...");
    play_audio_with_stop(newloseinter, newloseinter_len, "进入配网模式音频");

    nvs_handle_t wifi_handle;
    esp_err_t ret = nvs_open("wifi_config", NVS_READWRITE, &wifi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开wifi_config失败: %s", esp_err_to_name(ret));
        is_reprovision_transition = false;
        return;
    }

    esp_err_t erase_ssid_ret = nvs_erase_key(wifi_handle, "ssid");
    esp_err_t erase_pass_ret = nvs_erase_key(wifi_handle, "password");
    (void)nvs_erase_key(wifi_handle, "bssid");
    (void)nvs_erase_key(wifi_handle, "security");
    esp_err_t commit_ret = nvs_commit(wifi_handle);
    nvs_close(wifi_handle);

    ESP_LOGI(TAG, "清除ssid键结果: %s", esp_err_to_name(erase_ssid_ret));
    ESP_LOGI(TAG, "清除password键结果: %s", esp_err_to_name(erase_pass_ret));

    if (commit_ret != ESP_OK) {
        ESP_LOGE(TAG, "提交WiFi配置清除失败: %s", esp_err_to_name(commit_ret));
        is_reprovision_transition = false;
        return;
    }

    key_triple_press = 0;
    vTaskDelay(pdMS_TO_TICKS(800));

    ESP_LOGI(TAG, "🔄 WiFi配置已清除，设备即将重启并进入AP配网模式");
    esp_restart();
}

// 后台播放开机音频：避免阻塞主流程，尽快进入待命待唤醒
static void boot_audio_task(void *param)
{
    (void)param;
    if (is_reprovision_transition) {
        vTaskDelete(nullptr);
        return;
    }
    bsp_play_audio(newweekup, newweekup_len);
    vTaskDelete(nullptr);
}

// 后台播放唤醒提示音：实现边播边录，不阻塞主流程
static void async_start_audio_task(void *param)
{
    (void)param;
    play_audio_with_stop(start, start_len, "开始音频");
    vTaskDelete(nullptr);
}

// 后台播放 WiFi 连接成功提示音，依赖底层 I2S 互斥锁实现串行播放
static void wifi_success_audio_task(void *param)
{
    (void)param;
    if (is_reprovision_transition) {
        vTaskDelete(nullptr);
        return;
    }
    play_audio_with_stop(newsuccess, newsuccess_len, "WiFi连接成功音频");
    vTaskDelete(nullptr);
}

static void provisioning_audio_task(void *param)
{
    (void)param;
    bsp_play_audio(newloseinter, newloseinter_len);
    vTaskDelete(nullptr);
}

static void start_provisioning_audio_async()
{
    xTaskCreatePinnedToCore(provisioning_audio_task, "provision_audio", 4096, nullptr, 3, nullptr, 0);
}

static std::string build_provisioning_ap_ssid()
{
    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (ret != ESP_OK) {
        return "ESP_Config";
    }

    char ssid[20];
    snprintf(ssid, sizeof(ssid), "ESP_%02X%02X", mac[4], mac[5]);
    return std::string(ssid);
}

/**
 * @brief 🉰️ 程序主入口（这里是一切的开始）
 *
 * ESP32启动后会自动调用这个函数。
 * 
 * 主要工作流程：
 * 1. 初始化各种硬件（LED、麦克风、扬声器）
 * 2. 连接WiFi和WebSocket服务器
 * 3. 加载语音识别模型
 * 4. 进入主循环，开始监听用户说话
 */
extern "C" void app_main(void)
{
    // ③ 初始化LED灯（用于状态指示）
    init_led();
    Power_Init();
    
    // ① 初始化NVS（非易失性存储）
    // NVS用于保存WiFi配置等信息，即使断电也不会丢失
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // 如果NVS区域满了或版本不匹配，就清空重来
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    configure_runtime_log_levels();

    // ② 检查是否是重新烧录（通过固件版本号检测）
    // 如果固件版本号不匹配或不存在，说明是新烧录的，清除WiFi配置
    const char* FIRMWARE_VERSION = "1.1.3"; // 固件版本号，每次更新固件时修改此值
    bool is_new_flash = false;
    nvs_handle_t version_handle;
    ret = nvs_open("system", NVS_READWRITE, &version_handle);
    if (ret == ESP_OK) {
        char saved_version[32] = {0};  // 增加缓冲区大小，防止溢出
        size_t required_size = sizeof(saved_version);
        esp_err_t version_ret = nvs_get_str(version_handle, "fw_ver", saved_version, &required_size);
        
        if (version_ret != ESP_OK || strcmp(saved_version, FIRMWARE_VERSION) != 0) {
            // 版本号不存在或不匹配，说明是新烧录的
            is_new_flash = true;
            ESP_LOGI(TAG, "═══════════════════════════════════════");
            ESP_LOGI(TAG, "🔄 检测到新固件版本");
            ESP_LOGI(TAG, "   保存的版本: %s", (version_ret == ESP_OK) ? saved_version : "无");
            ESP_LOGI(TAG, "   当前版本: %s", FIRMWARE_VERSION);
            ESP_LOGI(TAG, "   将清除旧的WiFi配置，进入AP配网模式");
            ESP_LOGI(TAG, "═══════════════════════════════════════");
            
            // 清除WiFi配置
            nvs_handle_t wifi_handle;
            if (nvs_open("wifi_config", NVS_READWRITE, &wifi_handle) == ESP_OK) {
                esp_err_t erase_ssid_ret = nvs_erase_key(wifi_handle, "ssid");
                esp_err_t erase_pass_ret = nvs_erase_key(wifi_handle, "password");
                (void)nvs_erase_key(wifi_handle, "bssid");
                (void)nvs_erase_key(wifi_handle, "security");
                esp_err_t commit_ret = nvs_commit(wifi_handle);
                nvs_close(wifi_handle);
                
                if (erase_ssid_ret == ESP_OK && erase_pass_ret == ESP_OK && commit_ret == ESP_OK) {
                    ESP_LOGI(TAG, "✅ 已清除旧的WiFi配置");
                } else {
                    ESP_LOGW(TAG, "⚠️ 清除WiFi配置部分失败: ssid=%s, password=%s, commit=%s",
                             esp_err_to_name(erase_ssid_ret), esp_err_to_name(erase_pass_ret),
                             esp_err_to_name(commit_ret));
                }
            } else {
                ESP_LOGW(TAG, "⚠️ 无法打开wifi_config命名空间清除配置: %s", esp_err_to_name(ret));
            }
            
            // 保存新的固件版本号（必须成功，否则下次启动会再次清除配置）
            // 注意：NVS键名最大长度为15字符，所以使用"fw_ver"而不是"firmware_version"
            esp_err_t set_ret = nvs_set_str(version_handle, "fw_ver", FIRMWARE_VERSION);
            if (set_ret != ESP_OK) {
                ESP_LOGE(TAG, "❌ 保存版本号失败: %s", esp_err_to_name(set_ret));
                ESP_LOGE(TAG, "   警告：下次启动将再次清除配置");
            } else {
                esp_err_t commit_ret = nvs_commit(version_handle);
                if (commit_ret != ESP_OK) {
                    ESP_LOGE(TAG, "❌ 提交版本号失败: %s", esp_err_to_name(commit_ret));
                    ESP_LOGE(TAG, "   警告：下次启动将再次清除配置");
                } else {
                    ESP_LOGI(TAG, "✅ 已保存新的固件版本号: %s", FIRMWARE_VERSION);
                }
            }
        } else {
            ESP_LOGI(TAG, "ℹ️ 固件版本匹配: %s，使用现有配置", FIRMWARE_VERSION);
        }
        nvs_close(version_handle);
    } else {
        // 无法打开system命名空间，可能是首次启动
        is_new_flash = true;
        ESP_LOGI(TAG, "ℹ️ 首次启动或NVS初始化，将进入AP配网模式");
        
        // 创建system命名空间并保存版本号（必须成功，否则下次启动会再次清除配置）
        nvs_handle_t system_handle;
        if (nvs_open("system", NVS_READWRITE, &system_handle) == ESP_OK) {
            // 注意：NVS键名最大长度为15字符，所以使用"fw_ver"而不是"firmware_version"
            esp_err_t set_ret = nvs_set_str(system_handle, "fw_ver", FIRMWARE_VERSION);
            if (set_ret == ESP_OK) {
                esp_err_t commit_ret = nvs_commit(system_handle);
                if (commit_ret == ESP_OK) {
                    ESP_LOGI(TAG, "✅ 已保存固件版本号: %s", FIRMWARE_VERSION);
                } else {
                    ESP_LOGE(TAG, "❌ 提交版本号失败: %s", esp_err_to_name(commit_ret));
                    ESP_LOGE(TAG, "   警告：下次启动将再次清除配置");
                }
            } else {
                ESP_LOGE(TAG, "❌ 保存版本号失败: %s", esp_err_to_name(set_ret));
                ESP_LOGE(TAG, "   警告：下次启动将再次清除配置");
            }
            nvs_close(system_handle);
        } else {
            ESP_LOGE(TAG, "❌ 无法创建system命名空间: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "   版本号无法保存，下次启动将再次清除配置");
        }
    }


    gpio_set_level(GPIO_NUM_15, 1);
    // ⑤ 尝试从NVS读取WiFi配置
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    if (is_new_flash) {
        ESP_LOGI(TAG, "🔍 检查WiFi配置（新固件，已清除旧配置）...");
    } else {
        ESP_LOGI(TAG, "🔍 正在检查WiFi配置...");
    }
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    nvs_handle_t nvs_handle;
    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};
    size_t required_size = sizeof(saved_ssid);
    
    bool has_wifi_config = false;
    bool wifi_connected = false;
    ret = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        // 检查SSID是否存在且不为空
        esp_err_t ssid_ret = nvs_get_str(nvs_handle, "ssid", saved_ssid, &required_size);
        if (ssid_ret == ESP_OK && strlen(saved_ssid) > 0) {
            required_size = sizeof(saved_pass);
            // 检查密码是否存在（密码可以为空字符串，但键必须存在）
            esp_err_t pass_ret = nvs_get_str(nvs_handle, "password", saved_pass, &required_size);
            if (pass_ret == ESP_OK) {
                has_wifi_config = true;
                ESP_LOGI(TAG, "✅ 找到保存的WiFi配置: %s", saved_ssid);
            } else {
                ESP_LOGI(TAG, "⚠️ 找到SSID但密码读取失败: %s，将进入配网模式", esp_err_to_name(pass_ret));
            }
        } else {
            ESP_LOGI(TAG, "ℹ️ NVS中没有保存的WiFi配置（ssid_ret=%s）", esp_err_to_name(ssid_ret));
            if (ssid_ret == ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGI(TAG, "   这是首次启动或配置已清除，将自动进入AP配网模式");
            }
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGI(TAG, "ℹ️ 无法打开wifi_config命名空间: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "   这是首次启动，将自动进入AP配网模式");
        } else {
            ESP_LOGI(TAG, "   可能是首次启动或已清除配置，将自动进入AP配网模式");
        }
    }
    
    // ⑤ 如果有WiFi配置，尝试连接
    if (has_wifi_config) {
        ESP_LOGI(TAG, "正在连接WiFi: %s", saved_ssid);
        wifi_manager = new WiFiManager(saved_ssid, saved_pass);
        if (wifi_manager->connect() == ESP_OK) {
            ESP_LOGI(TAG, "✅ WiFi连接成功，继续正常流程");
            wifi_connected = true;
            // 继续执行后续代码（连接服务器等）
        } else {
            ESP_LOGW(TAG, "⚠️ WiFi连接失败，进入配网模式");
            // 清理失败的WiFi管理器
            delete wifi_manager;
            wifi_manager = nullptr;
            has_wifi_config = false; // 标记为需要配网
        }
    }
    
    // ⑥ 如果没有WiFi配置或连接失败，进入AP配网模式
    if (!has_wifi_config) {
        const std::string provisioning_ap_ssid = build_provisioning_ap_ssid();
        ESP_LOGI(TAG, "📡 进入AP配网模式，准备快速释放热点...");
        
        // 创建临时WiFiManager用于AP模式
        WiFiManager* ap_wifi = new WiFiManager("", "");
        if (ap_wifi->start_ap_mode(provisioning_ap_ssid, "") != ESP_OK) {
            ESP_LOGE(TAG, "❌ 启动AP模式失败，请检查硬件连接");
            ESP_LOGE(TAG, "   设备将在5秒后重启，尝试恢复...");
            delete ap_wifi;
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();  // 重启设备，尝试恢复
            return;
        }
        
        // 启动配网HTTP服务器
        if (start_config_server() != ESP_OK) {
            ESP_LOGE(TAG, "❌ 启动配网服务器失败");
            ESP_LOGE(TAG, "   设备将在5秒后重启，尝试恢复...");
            ap_wifi->stop_ap_mode();
            delete ap_wifi;
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();  // 重启设备，尝试恢复
            return;
        }
        
        // 启动蓝牙广播
        start_ble_advertising();

        // 热点与页面先就绪，再后台播报提示音，避免启动配网被音频阻塞。
        start_provisioning_audio_async();
        
        // 等待用户配网（轮询检查是否有新配置）
        ESP_LOGI(TAG, "✅ 配网就绪：热点=%s，地址=http://192.168.4.1", provisioning_ap_ssid.c_str());
        std::string new_ssid, new_password;
        int wait_count = 0;
        const int max_wait = 300; // 最多等待5分钟（300秒）
        
        while (wait_count < max_wait) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒
            wait_count++;
            
            // 检查是否有新配置
            if (get_wifi_config(new_ssid, new_password)) {
                ESP_LOGI(TAG, "✅ 收到WiFi配置: %s", new_ssid.c_str());
                ESP_LOGI(TAG, "═══════════════════════════════════════");
                ESP_LOGI(TAG, "📡 配网信息已保存");
                ESP_LOGI(TAG, "   • 将退出当前配网热点");
                ESP_LOGI(TAG, "   • 设备重启后使用新WiFi配置重新联网");
                ESP_LOGI(TAG, "═══════════════════════════════════════");

                stop_config_server();
                ap_wifi->stop_ap_mode();
                delete ap_wifi;

                vTaskDelay(pdMS_TO_TICKS(1500));
                ESP_LOGI(TAG, "🔄 正在重启设备以应用新的WiFi配置...");
                esp_restart();
                return;
            }
            
            // 每30秒打印一次提示，避免串口日志过于嘈杂
            if (wait_count % 30 == 0) {
                ESP_LOGI(TAG, "   等待中... (%d/%d秒)", wait_count, max_wait);
            }
        }
        
        ESP_LOGW(TAG, "═══════════════════════════════════════");
        ESP_LOGW(TAG, "⏰ 配网超时（%d秒），设备将继续运行在AP模式", max_wait);
        ESP_LOGW(TAG, "   您可以随时连接热点进行配网：");
        ESP_LOGW(TAG, "   🔹 热点名称：%s", provisioning_ap_ssid.c_str());
        ESP_LOGW(TAG, "   🔹 访问地址：http://192.168.4.1");
        ESP_LOGW(TAG, "═══════════════════════════════════════");
        // 继续运行在AP模式，不退出
        return;
    }


    
    // ⑧ 连接WebSocket服务器（用于与电脑通信）
    ESP_LOGI(TAG, "正在连接WebSocket服务器...");
    // 待命阶段不强制自动重连，避免日志/进程干扰；
    // 同时：如果当前服务器未启动，仍要继续进入唤醒监听，后续在唤醒/对话触发时再尝试连接。
    websocket_client = new WebSocketClient(WS_URI, false, 5000);
    websocket_client->setEventCallback(on_websocket_event);  // 设置事件处理函数
    if (websocket_client->connect() != ESP_OK) {
        ESP_LOGW(TAG, "WebSocket连接失败（仍将进入待命待唤醒）。");
        ESP_LOGW(TAG, "请检查：1) 电脑上的server.py是否在运行 2) IP地址是否正确");
        // 不 return：即使 WebSocket 暂时不可用，也要先完成麦克风/模型初始化并进入唤醒监听。
        // 后续在唤醒词触发时会根据 websocket_client->isConnected() 再尝试连接。
    }

    // ⑤ 初始化麦克风（INMP441数字麦克风）
    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ESP_LOGI(TAG, "音频参数: 16kHz采样率, 单声道, 16位");

    ret = bsp_board_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查硬件连接: VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5");
        return;
    }
    ESP_LOGI(TAG, "✓ ES8311音频系统初始化成功");
    // 音频系统就绪后播放开机提示音
    xTaskCreatePinnedToCore(boot_audio_task, "boot_audio_task", 4096, nullptr, 3, nullptr, 0);



    // ⑦ 初始化VAD（语音活动检测）
    // VAD用于检测用户什么时候开始说话、什么时候停止
    ESP_LOGI(TAG, "正在初始化语音活动检测（VAD）...");
    
    // 创建VAD实例，使用更精确的参数控制
    // VAD_MODE_1: 中等灵敏度
    // 16000Hz采样率，30ms帧长度，最小语音时长200ms，最小静音时长1000ms
    vad_inst = vad_create_with_param(VAD_MODE_1, SAMPLE_RATE, 30, 200, 1000);
    if (vad_inst == NULL) {
        ESP_LOGE(TAG, "创建VAD实例失败");
        return;
    }
    
    ESP_LOGI(TAG, "✓ VAD初始化成功");
    ESP_LOGI(TAG, "  - VAD模式: 1 (中等灵敏度)");
    ESP_LOGI(TAG, "  - 采样率: %d Hz", SAMPLE_RATE);
    ESP_LOGI(TAG, "  - 帧长度: 30 ms");
    ESP_LOGI(TAG, "  - 最小语音时长: 200 ms");
    ESP_LOGI(TAG, "  - 最小静音时长: 1000 ms");

    // ⑧ 加载唤醒词检测模型（当前已禁用语音唤醒，仅保留模型加载以防后续需要）
    ESP_LOGI(TAG, "正在加载唤醒词检测模型...");

    // 检查内存状态
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "内存状态检查:");
    ESP_LOGI(TAG, "  - 总可用内存: %zu KB", free_heap / 1024);
    ESP_LOGI(TAG, "  - 内部RAM: %zu KB", free_internal / 1024);
    ESP_LOGI(TAG, "  - PSRAM: %zu KB", free_spiram / 1024);

    if (free_heap < 100 * 1024)
    {
        ESP_LOGE(TAG, "可用内存不足，需要至少100KB");
        return;
    }

    // 从模型目录加载所有可用的语音识别模型
    ESP_LOGI(TAG, "开始加载模型文件...");

    // 临时添加错误处理和重试机制
    srmodel_list_t *models = NULL;
    int retry_count = 0;
    const int max_retries = 3;

    while (models == NULL && retry_count < max_retries)
    {
        ESP_LOGI(TAG, "尝试加载模型 (第%d次)...", retry_count + 1);

        // 在每次重试前等待一下
        if (retry_count > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        models = esp_srmodel_init("model");

        if (models == NULL)
        {
            ESP_LOGW(TAG, "模型加载失败，准备重试...");
            retry_count++;
        }
    }
    if (models == NULL)
    {
        ESP_LOGE(TAG, "语音识别模型初始化失败");
        ESP_LOGE(TAG, "请检查模型文件是否正确烧录到Flash分区");
        return;
    }

    // 自动选择sdkconfig中配置的唤醒词模型（如果配置了多个模型则选择第一个）
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (model_name == NULL)
    {
        ESP_LOGE(TAG, "未找到任何唤醒词模型！");
        ESP_LOGE(TAG, "请确保已正确配置并烧录唤醒词模型文件");
        ESP_LOGE(TAG, "可通过 'idf.py menuconfig' 配置唤醒词模型");
        return;
    }

    ESP_LOGI(TAG, "✓ 选择唤醒词模型: %s", model_name);

    // 获取唤醒词检测接口
    esp_wn_iface_t *wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    if (wakenet == NULL)
    {
        ESP_LOGE(TAG, "获取唤醒词接口失败，模型: %s", model_name);
        return;
    }

    // 创建唤醒词模型数据实例
    // DET_MODE_90: 检测模式，90%置信度阈值，平衡准确率和误触发率
    model_iface_data_t *model_data = wakenet->create(model_name, DET_MODE_90);
    if (model_data == NULL)
    {
        ESP_LOGE(TAG, "创建唤醒词模型数据失败");
        return;
    }

    // ⑨ 加载命令词识别模型（识别"开灯"、"关灯"等）
    ESP_LOGI(TAG, "正在加载命令词识别模型...");

    // 获取中文命令词识别模型（MultiNet7）
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL)
    {
        ESP_LOGE(TAG, "未找到中文命令词识别模型！");
        ESP_LOGE(TAG, "请确保已正确配置并烧录MultiNet7中文模型");
        return;
    }

    ESP_LOGI(TAG, "✓ 选择命令词模型: %s", mn_name);

    // 获取命令词识别接口
    multinet = esp_mn_handle_from_name(mn_name);
    if (multinet == NULL)
    {
        ESP_LOGE(TAG, "获取命令词识别接口失败，模型: %s", mn_name);
        return;
    }

    // 创建命令词模型数据实例
    mn_model_data = multinet->create(mn_name, 6000);
    if (mn_model_data == NULL)
    {
        ESP_LOGE(TAG, "创建命令词模型数据失败");
        return;
    }

    // 配置自定义命令词
    ESP_LOGI(TAG, "正在配置命令词...");
    esp_err_t cmd_config_ret = configure_custom_commands(multinet, mn_model_data);
    if (cmd_config_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "命令词配置失败");
        return;
    }
    ESP_LOGI(TAG, "✓ 命令词配置完成");

    // ⑩ 初始化噪音抑制（可选，提高噪音环境下的识别率）
    ESP_LOGI(TAG, "正在初始化噪音抑制模块...");
    
    // 获取噪音抑制模型
    char *nsn_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    if (nsn_model_name == NULL) {
        ESP_LOGW(TAG, "未找到噪音抑制模型，将不使用噪音抑制");
    } else {
        ESP_LOGI(TAG, "✓ 选择噪音抑制模型: %s", nsn_model_name);
        
        // 获取噪音抑制接口
        nsn_handle = (esp_nsn_iface_t *)esp_nsnet_handle_from_name(nsn_model_name);
        if (nsn_handle == NULL) {
            ESP_LOGW(TAG, "获取噪音抑制接口失败");
        } else {
            // 创建噪音抑制实例
            nsn_model_data = nsn_handle->create(nsn_model_name);
            if (nsn_model_data == NULL) {
                ESP_LOGW(TAG, "创建噪音抑制实例失败");
            } else {
                ESP_LOGI(TAG, "✓ 噪音抑制初始化成功");
                ESP_LOGI(TAG, "  - 噪音抑制模型: %s", nsn_model_name);
                ESP_LOGI(TAG, "  - 采样率: %d Hz", SAMPLE_RATE);
            }
        }
    }

    // ⑪ 准备音频缓冲区
    // 获取语音识别模型需要的数据块大小
    int audio_chunksize = wakenet->get_samp_chunksize(model_data) * sizeof(int16_t);

    // 分配音频数据缓冲区内存
    int16_t *buffer = (int16_t *)malloc(audio_chunksize);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "音频缓冲区内存分配失败，需要 %d 字节", audio_chunksize);
        ESP_LOGE(TAG, "请检查系统可用内存");
        return;
    }

    // 初始化音频管理器
    audio_manager = new AudioManager(SAMPLE_RATE, 20, 32);  // 16kHz, 10秒录音, 32秒响应
    ret = audio_manager->init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频管理器初始化失败: %s", esp_err_to_name(ret));
        free(buffer);
        delete audio_manager;
        audio_manager = nullptr;
        return;
    }
    ESP_LOGI(TAG, "✓ 音频管理器初始化成功");

    // 播放WiFi连接状态音频
    if (wifi_connected) {
        ESP_LOGI(TAG, "🔊 播放WiFi连接成功音频（后台串行）...");
        xTaskCreatePinnedToCore(wifi_success_audio_task, "wifi_success_audio_task", 4096, nullptr, 3, nullptr, 0);
    } else if (!has_wifi_config) {
        ESP_LOGI(TAG, "🔊 播放进入配网模式音频...");
        play_audio_with_stop(newloseinter, newloseinter_len, "进入配网模式音频");
    }

    ESP_LOGI(TAG, "✓ 使用WebSocket进行通信");

    // 显示系统配置信息
    ESP_LOGI(TAG, "✓ 智能语音助手系统配置完成:");
    ESP_LOGI(TAG, "  - 唤醒词模型: %s", model_name);
    ESP_LOGI(TAG, "  - 命令词模型: %s", mn_name);
    ESP_LOGI(TAG, "  - 音频块大小: %d 字节", audio_chunksize);
    ESP_LOGI(TAG, "  - 噪音抑制: %s", (nsn_model_data != NULL) ? "已启用" : "未启用");
    ESP_LOGI(TAG, "  - 检测置信度: 90%%");
    ESP_LOGI(TAG, "正在启动智能语音助手...");
    ESP_LOGI(TAG, "请长按按键唤醒设备");

    // 🎮 主循环 - 开始实时语音识别
    ESP_LOGI(TAG, "系统启动完成，请长按按键唤醒设备...");
    keyweakup_init(KEY_GPIO);
    xTaskCreatePinnedToCore(keyweakup_task, "keyweakup_task", 4096, NULL, 5, NULL, 0);

    while (1)
    {
        // 🎧 从麦克风读取一小段音频数据
        // 这里的false表示要处理后的数据（不要原始数据）
        esp_err_t ret = bsp_get_feed_data(false, buffer, audio_chunksize);
        if (ret != ESP_OK)
        {
// 仅在调试模式下输出错误日志
#ifdef DEBUG_MODE
            ESP_LOGE(TAG, "麦克风音频数据获取失败: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "请检查INMP441硬件连接");
#endif
            vTaskDelay(pdMS_TO_TICKS(10)); // 等待10ms后重试
            continue;
        }

        // 如果启用了噪音抑制，先对音频数据进行噪音抑制处理
        int16_t *processed_audio = buffer;
        static int16_t *ns_out_buffer = NULL;  // 噪音抑制输出缓冲区
        if (nsn_handle != NULL && nsn_model_data != NULL) {
            // 如果输出缓冲区未分配，分配它
            if (ns_out_buffer == NULL) {
                int ns_chunksize = nsn_handle->get_samp_chunksize(nsn_model_data);
                ns_out_buffer = (int16_t *)malloc(ns_chunksize * sizeof(int16_t));
                if (ns_out_buffer == NULL) {
                    ESP_LOGW(TAG, "噪音抑制输出缓冲区分配失败");
                    nsn_handle = NULL;  // 禁用噪音抑制
                }
            }
            
            if (ns_out_buffer != NULL) {
                // 执行噪音抑制
                nsn_handle->process(nsn_model_data, buffer, ns_out_buffer);
                processed_audio = ns_out_buffer;  // 使用噪音抑制后的数据
            }
        }

        // 连续三击仅在待机/休眠状态下允许进入配网模式
        key_allow_triple_press = (current_state == STATE_WAITING_WAKEUP) ? 1 : 0;

        if (key_triple_press == 1) {
            if (current_state == STATE_WAITING_WAKEUP) {
                enter_wifi_reprovision_mode();
                continue;
            }
            key_triple_press = 0;
        }
        
        // 检测短按事件（只在非休眠状态下处理）
        if (key_short_press == 1 && current_state != STATE_WAITING_WAKEUP)
        {
            ESP_LOGI(TAG, "检测到短按，中断对话并进入录音状态");
            key_short_press = 0;

            // 先进入"强制录音过渡"状态，屏蔽上一轮回复的音频分片/尾部 ping
            is_force_recording_transition = true;
            is_force_standby_transition = false;
            current_state = STATE_RECORDING;
            received_ping = false;
            is_realtime_streaming = true;
            pending_recording_started_after_reconnect = false;
            
            // 停止音频播放（包括服务器流式音频）
            if (audio_manager != nullptr) {
                audio_manager->emergencyStopAudio();
                audio_manager->resetResponsePlayedFlag();
                audio_manager->stopRecording();
                audio_manager->clearRecordingBuffer();
                bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃旧音频
                audio_manager->startRecording();
            }

            // 强制重建 WebSocket 会话，彻底切断上一轮回复的尾包与下一轮录音/播放的关联
            if (websocket_client != nullptr) {
                pending_recording_started_after_reconnect = true;
                websocket_client->disconnect();
                if (websocket_client->connect() != ESP_OK) {
                    ESP_LOGW(TAG, "短按中断后重连WebSocket失败，将在当前连接不可用状态下继续本地录音");
                    pending_recording_started_after_reconnect = false;
                }
            }
            
            // 初始化各种状态变量
            vad_speech_detected = false;        // 还没检测到说话
            vad_silence_frames = 0;             // 静音帧计数器清零
            is_continuous_conversation = true;  // 进入连续对话模式
            user_started_speaking = false;      // 用户还没开始说话
            recording_timeout_start = xTaskGetTickCount();  // 设置超时
            is_realtime_streaming = true;       // 唤醒后立即开始流式传输，避免吞字
            
            // 重置各种检测器
            vad_reset_trigger(vad_inst);        // 重置VAD
            multinet->clean(mn_model_data);     // 清空命令词缓冲区
            
            ESP_LOGI(TAG, "开始录音，请说话...");
        } else if (key_short_press == 1 && current_state == STATE_WAITING_WAKEUP) {
            // 在休眠状态下，忽略短按
            ESP_LOGI(TAG, "在休眠状态下，忽略短按");
            key_short_press = 0;
        }
        
        // 检测长按事件（只在非休眠状态下处理）
        if (key_flag == 1 && current_state != STATE_WAITING_WAKEUP) {
            ESP_LOGI(TAG, "检测到长按1.5秒，触发休眠模式");
            key_flag = 0;
            execute_exit_logic();
        }
        // 休眠状态下的长按唤醒由下面的唤醒检测逻辑处理
        
        if (current_state == STATE_WAITING_WAKEUP)
        {
            // 连续三击判定窗口内，临时屏蔽待机态唤醒，避免按键声或旧提示音打乱播报顺序
            if (key_triple_press_active == 1 || is_reprovision_transition) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // 🛌 休眠状态：仅支持按键唤醒，禁用语音唤醒
            // wakenet_state_t wn_state = wakenet->detect(model_data, processed_audio);

            if (key_flag == 1)
            {
                key_flag = 0;

                // 检查WebSocket连接状态
                if (websocket_client != nullptr && !websocket_client->isConnected())
                {
                    ESP_LOGI(TAG, "WebSocket未连接，正在重连...");
                    websocket_client->connect();
                    vTaskDelay(pdMS_TO_TICKS(500));  // 等待500ms
                }

                // 通知服务器：唤醒成功
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    // 构造JSON消息
                    char wake_msg[256];
                    snprintf(wake_msg, sizeof(wake_msg),
                             "{\"event\":\"wake_word_detected\",\"model\":\"%s\",\"timestamp\":%lld}",
                             model_name,
                             (long long)esp_timer_get_time() / 1000);
                    websocket_client->sendText(wake_msg);
                }

                // 🎵 同步播放唤醒提示音，彻底播完后再开始录音，避免录入提示音
                ESP_LOGI(TAG, "播放唤醒提示音（同步，等待播完再录音）...");
                play_audio_with_stop(start, start_len, "唤醒提示音");

                // 🎙️ 进入录音状态（提示音播完后清空接收缓冲区，再开始录音）
                current_state = STATE_RECORDING;
                audio_manager->clearRecordingBuffer();
                bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃播放期间录入的音频
                audio_manager->startRecording();

                // 发送开始录音事件（提示音播完、录音真正开始后再通知服务器）
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    const char* start_msg = "{\"event\":\"recording_started\"}";
                    websocket_client->sendText(start_msg);
                    ESP_LOGI(TAG, "发送录音开始事件");
                }
                
                // 初始化各种状态变量
                vad_speech_detected = false;        // 还没检测到说话
                vad_silence_frames = 0;             // 静音帧计数器清零
                is_continuous_conversation = false;  // 第一次对话，不是连续模式
                user_started_speaking = false;      // 用户还没开始说话
                recording_timeout_start = 0;        // 第一次不设超时
                is_realtime_streaming = true;       // 唤醒后立即开始流式传输，避免吞字
                
                // 重置各种检测器
                vad_reset_trigger(vad_inst);        // 重置VAD
                multinet->clean(mn_model_data);     // 清空命令词缓冲区
                
                ESP_LOGI(TAG, "开始录音，请说话...");
            }
        }
        else if (current_state == STATE_RECORDING)
        {
            // 🎙️ 录音状态：记录用户说的话
            if (audio_manager->isRecording() && !audio_manager->isRecordingBufferFull())
            {
                // 将音频数据存入录音缓冲区
                int samples = audio_chunksize / sizeof(int16_t);
                audio_manager->addRecordingData(processed_audio, samples);
                
                // 📤 实时传输音频到服务器（边说边传，降低延迟）
                if (is_realtime_streaming && websocket_client != nullptr && websocket_client->isConnected())
                {
                    // 立即发送当前这段音频
                    size_t bytes_to_send = samples * sizeof(int16_t);
                    websocket_client->sendBinary((const uint8_t*)processed_audio, bytes_to_send);
                    ESP_LOGD(TAG, "实时发送: %zu 字节", bytes_to_send);
                }
                
                // 🎯 连续对话模式下，同时检测本地命令词（如"开灯"、"拜拜"）
                if (is_continuous_conversation)
                {
                    esp_mn_state_t mn_state = multinet->detect(mn_model_data, processed_audio);
                    if (mn_state == ESP_MN_STATE_DETECTED)
                    {
                        // 获取识别结果
                        esp_mn_results_t *mn_result = multinet->get_results(mn_model_data);
                        if (mn_result->num > 0)
                        {
                            int command_id = mn_result->command_id[0];
                            float prob = mn_result->prob[0];
                            const char *cmd_desc = get_command_description(command_id);
                            
                            ESP_LOGI(TAG, "🎯 在录音中检测到命令词: ID=%d, 置信度=%.2f, 内容=%s, 命令='%s'",
                                     command_id, prob, mn_result->string, cmd_desc);
                            
                            // 停止录音
                            audio_manager->stopRecording();
                            
                            // 直接处理命令，不发送到服务器
                            if (command_id == COMMAND_TURN_ON_LIGHT)
                            {
                                ESP_LOGI(TAG, "💡 执行开灯命令");
                                led_turn_on();
                                play_audio_with_stop(ok, ok_len, "开灯确认音频");
                                // 继续保持连续对话模式
                                audio_manager->clearRecordingBuffer();
                                bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃旧音频
                                audio_manager->startRecording();
                                vad_speech_detected = false;
                                vad_silence_frames = 0;
                                user_started_speaking = false;
                                recording_timeout_start = xTaskGetTickCount();
                                is_realtime_streaming = true;  // 命令执行完后立即开启流式传输
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                            else if (command_id == COMMAND_TURN_OFF_LIGHT)
                            {
                                ESP_LOGI(TAG, "💡 执行关灯命令");
                                led_turn_off();
                                play_audio_with_stop(ok, ok_len, "关灯确认音频");
                                // 继续保持连续对话模式
                                audio_manager->clearRecordingBuffer();
                                bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃旧音频
                                audio_manager->startRecording();
                                vad_speech_detected = false;
                                vad_silence_frames = 0;
                                user_started_speaking = false;
                                recording_timeout_start = xTaskGetTickCount();
                                is_realtime_streaming = true;  // 命令执行完后立即开启流式传输
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                            else if (command_id == COMMAND_BYE_BYE)
                            {
                                ESP_LOGI(TAG, "👋 检测到拜拜命令，退出对话");
                                execute_exit_logic();
                                continue;
                            }
                            else if (command_id == COMMAND_CUSTOM)
                            {
                                ESP_LOGI(TAG, "💡 执行自定义命令词");
                                play_audio_with_stop(custom, custom_len, "自定义确认音频");
                                // 继续保持连续对话模式
                                audio_manager->clearRecordingBuffer();
                                bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃旧音频
                                audio_manager->startRecording();
                                vad_speech_detected = false;
                                vad_silence_frames = 0;
                                user_started_speaking = false;
                                recording_timeout_start = xTaskGetTickCount();
                                is_realtime_streaming = true;  // 命令执行完后立即开启流式传输
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                        }
                    }
                }
                
                // 👂 使用VAD检测用户是否在说话
                // VAD会分析音频，判断是语音还是静音
                vad_state_t vad_state = vad_process(vad_inst, processed_audio, SAMPLE_RATE, 30);
                
                // 如果VAD检测到有人说话
                if (vad_state == VAD_SPEECH) {
                    vad_speech_detected = true;
                    vad_silence_frames = 0;
                    user_started_speaking = true;  // 标记用户已经开始说话
                    recording_timeout_start = 0;  // 用户说话后取消连续对话超时
                    
                    // 🚀 用户开始说话了，启动实时传输
                    if (!is_realtime_streaming) {
                        is_realtime_streaming = true;
                        if (is_continuous_conversation) {
                            ESP_LOGI(TAG, "连续对话：检测到说话，开始实时传输...");
                        } else {
                            ESP_LOGI(TAG, "首次对话：检测到说话，开始实时传输...");
                        }
                    }
                    
                    // 显示录音进度（每100ms显示一次）
                    static TickType_t last_log_time = 0;
                    TickType_t current_time = xTaskGetTickCount();
                    if (current_time - last_log_time > pdMS_TO_TICKS(100)) {
                        ESP_LOGD(TAG, "正在录音... 当前长度: %.2f 秒", audio_manager->getRecordingDuration());
                        last_log_time = current_time;
                    }
                } else if (vad_state == VAD_SILENCE && vad_speech_detected) {
                    // 🤐 检测到静音（但之前已经有说话）
                    vad_silence_frames++;
                    
                    // 如果静音超过600ms，认为用户说完了
                    if (vad_silence_frames >= VAD_SILENCE_FRAMES_REQUIRED) {
                        ESP_LOGI(TAG, "VAD检测到用户说话结束，录音长度: %.2f 秒",
                                 audio_manager->getRecordingDuration());
                        audio_manager->stopRecording();
                        is_realtime_streaming = false;  // 停止实时流式传输

                        // 只有在用户确实说话了才发送数据
                        size_t rec_len = 0;
                        audio_manager->getRecordingBuffer(rec_len);
                        if (user_started_speaking && rec_len > SAMPLE_RATE / 4) // 至少0.25秒的音频
                        {
                            // 发送录音结束事件
                            if (websocket_client != nullptr && websocket_client->isConnected())
                            {
                                const char* end_msg = "{\"event\":\"recording_ended\"}";
                                websocket_client->sendText(end_msg);
                                ESP_LOGI(TAG, "发送录音结束事件");
                            }
                            
                            // 切换到等待响应状态
                            is_force_recording_transition = false;
                            pending_recording_started_after_reconnect = false;
                            current_state = STATE_WAITING_RESPONSE;
                            audio_manager->resetResponsePlayedFlag(); // 重置播放标志
                            ESP_LOGI(TAG, "等待服务器响应音频...");
                        }
                        else
                        {
                            ESP_LOGI(TAG, "录音时间过短或用户未说话，重新开始录音");
                            // 发送录音取消事件
                            if (websocket_client != nullptr && websocket_client->isConnected())
                            {
                                const char* cancel_msg = "{\"event\":\"recording_cancelled\"}";
                                websocket_client->sendText(cancel_msg);
                            }
                            // 重新开始录音
                            audio_manager->clearRecordingBuffer();
                            bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃旧音频
                            audio_manager->startRecording();
                            vad_speech_detected = false;
                            vad_silence_frames = 0;
                            user_started_speaking = false;
                            is_realtime_streaming = true;  // 重新录音时也立即开启流式传输
                            if (is_continuous_conversation)
                            {
                                recording_timeout_start = xTaskGetTickCount();
                            }
                            vad_reset_trigger(vad_inst);
                            multinet->clean(mn_model_data);
                        }
                    }
                }
            }
            else if (audio_manager->isRecordingBufferFull())
            {
                // ⚠️ 录音时间太长，缓冲区满了（10秒上限）
                ESP_LOGW(TAG, "录音缓冲区已满，停止录音");
                audio_manager->stopRecording();
                is_realtime_streaming = false;  // 停止实时流式传输

                // 发送录音结束事件
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    const char* end_msg = "{\"event\":\"recording_ended\"}";
                    websocket_client->sendText(end_msg);
                    ESP_LOGI(TAG, "发送录音结束事件（缓冲区满）");
                }

                // 切换到等待响应状态
                is_force_recording_transition = false;
                pending_recording_started_after_reconnect = false;
                current_state = STATE_WAITING_RESPONSE;
                audio_manager->resetResponsePlayedFlag(); // 重置播放标志
                ESP_LOGI(TAG, "等待服务器响应音频...");
            }
            
            // ⏱️ 连续对话模式下，检查是否超时没说话（保持原有10秒逻辑）
            if (is_continuous_conversation && recording_timeout_start > 0 && !user_started_speaking)
            {
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - recording_timeout_start) > pdMS_TO_TICKS(RECORDING_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "⏰ 超过10秒没说话，退出对话");
                    audio_manager->stopRecording();
                    execute_exit_logic();
                }
                // 每秒提示一次剩余时间
                static TickType_t last_timeout_log = 0;
                if (current_time - last_timeout_log > pdMS_TO_TICKS(1000))
                {
                    int remaining_seconds = (RECORDING_TIMEOUT_MS - (current_time - recording_timeout_start) * portTICK_PERIOD_MS) / 1000;
                    if (remaining_seconds > 0)
                    {
                        ESP_LOGI(TAG, "等待用户说话... 剩余 %d 秒", remaining_seconds);
                    }
                    last_timeout_log = current_time;
                }
            }
        }
        else if (current_state == STATE_WAITING_RESPONSE)
        {
            // ⏳ 等待状态：等待服务器的AI回复
            
            // 1. 不再使用数据接收超时逻辑，只通过ping包触发结束

            // 2. 检查是否收到 ping 包
            if (received_ping) {
                // 无论是否正在播放，都处理 ping 包
                if (audio_manager->isStreamingActive()) {
                    // 继续播放缓冲区中的剩余数据
                    audio_manager->processStreamingPlayback();
                    
                    // 检查缓冲区是否为空（没有更多数据需要播放）
                    size_t available_data;
                    if (audio_manager->getStreamingWritePos() >= audio_manager->getStreamingReadPos()) {
                        available_data = audio_manager->getStreamingWritePos() - audio_manager->getStreamingReadPos();
                    } else {
                        available_data = audio_manager->getStreamingBufferSize() - audio_manager->getStreamingReadPos() + audio_manager->getStreamingWritePos();
                    }
                    
                    ESP_LOGI(TAG, "📊 缓冲区剩余数据: %zu 字节，准备播放完毕并进入录音状态", available_data);
                }
                
                // 无论是否还有数据，都播放剩余数据并进入录音状态
                // 因为收到 ping 包意味着服务器已经停止发送数据
                
                // 结束流式播放（会播放所有剩余数据）
                audio_manager->finishStreamingPlayback();
                
                // 标记响应已播放
                audio_manager->setStreamingComplete();
                
                // 重置 ping 标志
                received_ping = false;
            }

            // 3. 音频会通过WebSocket流式接收并播放
            // 这里只需要检查播放是否完成
            if (audio_manager->isResponsePlayed())
            {
                // 🔁 AI回复完毕，进入连续对话模式
                // 通知服务器准备接收下一轮对话
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    const char* start_msg = "{\"event\":\"recording_started\"}";
                    websocket_client->sendText(start_msg);
                }
                
                current_state = STATE_RECORDING;
                audio_manager->clearRecordingBuffer();
                bsp_i2s_flush_rx(); // 清空I2S接收缓冲区，丢弃旧音频
                audio_manager->startRecording();
                vad_speech_detected = false;
                vad_silence_frames = 0;
                is_continuous_conversation = true;  // 标记为连续对话模式
                user_started_speaking = false;
                recording_timeout_start = xTaskGetTickCount();  // 开始超时计时
                is_realtime_streaming = true;  // 在连续对话模式下，也立即开启流式传输，避免吞字
                audio_manager->resetResponsePlayedFlag(); // 重置标志
                // 重置VAD触发器状态
                vad_reset_trigger(vad_inst);
                // 重置命令词识别缓冲区
                multinet->clean(mn_model_data);
                ESP_LOGI(TAG, "进入连续对话模式，请在%d秒内继续说话...", RECORDING_TIMEOUT_MS / 1000);
                ESP_LOGI(TAG, "💡 提示：1) 可以继续提问 2) 说\"帮我开/关灯\" 3) 说\"拜拜\"结束");
            }
        }

        // 短暂延时，避免CPU占用过高，同时保证实时性
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // 🧹 资源清理（正常情况下不会执行到这里）
    // 因为上面是无限循环，只有出错时才会走到这里
    ESP_LOGI(TAG, "正在清理系统资源...");

    // 销毁噪音抑制实例
    if (nsn_model_data != NULL && nsn_handle != NULL)
    {
        nsn_handle->destroy(nsn_model_data);
    }

    // 销毁VAD实例
    if (vad_inst != NULL)
    {
        vad_destroy(vad_inst);
    }

    // 销毁唤醒词模型数据
    if (model_data != NULL)
    {
        wakenet->destroy(model_data);
    }

    // 释放音频缓冲区内存
    if (buffer != NULL)
    {
        free(buffer);
    }

    // 清理WebSocket客户端
    if (websocket_client != nullptr)
    {
        delete websocket_client;
        websocket_client = nullptr;
    }

    // 清理WiFi管理器
    if (wifi_manager != nullptr)
    {
        delete wifi_manager;
        wifi_manager = nullptr;
    }

    // 释放音频管理器
    if (audio_manager != nullptr)
    {
        delete audio_manager;
        audio_manager = nullptr;
    }

    // 删除当前任务
    vTaskDelete(NULL);
}
