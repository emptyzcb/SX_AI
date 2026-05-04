#include "mqtt_config_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/md5.h"
#include <cstring>
#include <sstream>
#include <iomanip>

static const char *TAG = "MQTTConfig";

MQTTConfigClient::MQTTConfigClient(const std::string& broker_uri, 
                                   const std::string& device_id,
                                   const std::string& username,
                                   const std::string& password)
    : broker_uri_(broker_uri)
    , device_id_(device_id)
    , username_(username)
    , password_(password)
    , connected_(false)
    , mqtt_client_(nullptr)
{
    // 构建配置Topic：/ai_toy/config/{device_id}
    config_topic_ = "/ai_toy/config/" + device_id_;
}

MQTTConfigClient::~MQTTConfigClient() {
    disconnect();
}

esp_err_t MQTTConfigClient::connect() {
    if (connected_) {
        return ESP_OK;
    }
    
    // 配置MQTT客户端
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.uri = broker_uri_.c_str();
    
    if (!username_.empty()) {
        mqtt_cfg.username = username_.c_str();
        mqtt_cfg.password = password_.c_str();
    }
    
    // 设置客户端ID（使用设备ID）
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "ai_toy_%s", device_id_.c_str());
    mqtt_cfg.client_id = client_id;
    
    // 创建MQTT客户端
    mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client_ == nullptr) {
        ESP_LOGE(TAG, "创建MQTT客户端失败");
        return ESP_FAIL;
    }
    
    // 注册事件处理函数
    esp_mqtt_client_register_event(mqtt_client_, 
                                   (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, this);
    
    // 启动MQTT客户端
    esp_err_t ret = esp_mqtt_client_start(mqtt_client_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动MQTT客户端失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "MQTT客户端启动成功，等待连接...");
    return ESP_OK;
}

void MQTTConfigClient::disconnect() {
    if (mqtt_client_ && connected_) {
        esp_mqtt_client_stop(mqtt_client_);
        esp_mqtt_client_destroy(mqtt_client_);
        mqtt_client_ = nullptr;
        connected_ = false;
        ESP_LOGI(TAG, "MQTT连接已断开");
    }
}

void MQTTConfigClient::setConfigUpdateCallback(ConfigUpdateCallback callback) {
    config_callback_ = callback;
}

void MQTTConfigClient::mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                         int32_t event_id, void* event_data) {
    MQTTConfigClient* client = static_cast<MQTTConfigClient*>(handler_args);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ MQTT连接成功");
            client->connected_ = true;
            
            // 订阅配置Topic
            int msg_id = esp_mqtt_client_subscribe(client->mqtt_client_, 
                                                   client->config_topic_.c_str(), 1);
            ESP_LOGI(TAG, "订阅配置Topic: %s (消息ID: %d)", 
                     client->config_topic_.c_str(), msg_id);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT连接断开");
            client->connected_ = false;
            break;
            
        case MQTT_EVENT_DATA:
            // 收到配置消息
            if (strncmp(event->topic, client->config_topic_.c_str(), 
                       event->topic_len) == 0) {
                char* data = (char*)malloc(event->data_len + 1);
                if (data) {
                    memcpy(data, event->data, event->data_len);
                    data[event->data_len] = '\0';
                    client->handleConfigMessage(event->topic, data, event->data_len);
                    free(data);
                }
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT错误");
            break;
            
        default:
            break;
    }
}

void MQTTConfigClient::handleConfigMessage(const char* topic, const char* data, int data_len) {
    ESP_LOGI(TAG, "收到配置消息: %s", data);
    
    // 解析JSON
    cJSON* json = cJSON_Parse(data);
    if (json == nullptr) {
        ESP_LOGE(TAG, "解析JSON失败");
        return;
    }
    
    // 提取配置字段
    cJSON* device_id_item = cJSON_GetObjectItem(json, "device_id");
    cJSON* system_prompt_item = cJSON_GetObjectItem(json, "system_prompt");
    cJSON* voice_id_item = cJSON_GetObjectItem(json, "voice_id");
    cJSON* config_version_item = cJSON_GetObjectItem(json, "config_version");
    cJSON* config_signature_item = cJSON_GetObjectItem(json, "config_signature");
    cJSON* action_item = cJSON_GetObjectItem(json, "action");
    
    if (!device_id_item || !system_prompt_item || !voice_id_item || 
        !config_version_item || !config_signature_item) {
        ESP_LOGE(TAG, "配置消息字段不完整");
        cJSON_Delete(json);
        return;
    }
    
    // 验证设备ID
    std::string recv_device_id = cJSON_GetStringValue(device_id_item);
    if (recv_device_id != device_id_) {
        ESP_LOGE(TAG, "设备ID不匹配: %s != %s", recv_device_id.c_str(), device_id_.c_str());
        cJSON_Delete(json);
        return;
    }
    
    // 提取配置值
    std::string system_prompt = cJSON_GetStringValue(system_prompt_item);
    std::string voice_id = cJSON_GetStringValue(voice_id_item);
    int config_version = cJSON_GetNumberValue(config_version_item);
    std::string config_signature = cJSON_GetStringValue(config_signature_item);
    
    // 验证配置签名
    if (!verifyConfigSignature(device_id_, system_prompt, voice_id, config_signature)) {
        ESP_LOGE(TAG, "配置签名验证失败，使用本地缓存");
        cJSON_Delete(json);
        return;
    }
    
    // 保存配置到NVS
    if (saveConfigToNVS(system_prompt, voice_id, config_version) == ESP_OK) {
        ESP_LOGI(TAG, "配置已保存到本地Flash");
        
        // 调用回调函数
        if (config_callback_) {
            config_callback_(system_prompt, voice_id, config_version);
        }
    } else {
        ESP_LOGE(TAG, "保存配置到NVS失败");
    }
    
    cJSON_Delete(json);
}

bool MQTTConfigClient::verifyConfigSignature(const std::string& device_id,
                                             const std::string& system_prompt,
                                             const std::string& voice_id,
                                             const std::string& signature) {
    // 计算配置签名
    std::string data = device_id + system_prompt + voice_id + CONFIG_SALT;
    
    uint8_t md5_hash[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const uint8_t*)data.c_str(), data.length());
    mbedtls_md5_finish(&ctx, md5_hash);
    mbedtls_md5_free(&ctx);
    
    // 转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)md5_hash[i];
    }
    std::string calculated_signature = ss.str();
    
    // 比较签名
    bool valid = (calculated_signature == signature);
    if (!valid) {
        ESP_LOGE(TAG, "签名不匹配: 计算=%s, 接收=%s", 
                 calculated_signature.c_str(), signature.c_str());
    }
    
    return valid;
}

esp_err_t MQTTConfigClient::saveConfigToNVS(const std::string& system_prompt,
                                           const std::string& voice_id,
                                           int config_version) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败");
        return ret;
    }
    
    // 保存系统提示词
    ret = nvs_set_str(nvs_handle, NVS_KEY_SYSTEM_PROMPT, system_prompt.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存系统提示词失败");
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 保存音色ID
    ret = nvs_set_str(nvs_handle, NVS_KEY_VOICE_ID, voice_id.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存音色ID失败");
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 保存配置版本
    ret = nvs_set_i32(nvs_handle, NVS_KEY_CONFIG_VERSION, config_version);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存配置版本失败");
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 提交更改
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "配置已保存: 版本=%d, 音色=%s", config_version, voice_id.c_str());
    }
    
    return ret;
}

esp_err_t MQTTConfigClient::loadConfigFromNVS(std::string& system_prompt,
                                             std::string& voice_id,
                                             int& config_version) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 读取系统提示词
    char prompt_buf[2048] = {0};
    size_t required_size = sizeof(prompt_buf);
    ret = nvs_get_str(nvs_handle, NVS_KEY_SYSTEM_PROMPT, prompt_buf, &required_size);
    if (ret == ESP_OK) {
        system_prompt = std::string(prompt_buf);
    }
    
    // 读取音色ID
    char voice_buf[64] = {0};
    required_size = sizeof(voice_buf);
    ret = nvs_get_str(nvs_handle, NVS_KEY_VOICE_ID, voice_buf, &required_size);
    if (ret == ESP_OK) {
        voice_id = std::string(voice_buf);
    }
    
    // 读取配置版本
    int32_t version = 0;
    ret = nvs_get_i32(nvs_handle, NVS_KEY_CONFIG_VERSION, &version);
    if (ret == ESP_OK) {
        config_version = version;
    }
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t MQTTConfigClient::pullConfigFromServer(const std::string& server_url) {
    // TODO: 实现HTTP拉取配置（使用esp_http_client）
    // 这里需要实现HTTP GET请求到 /api/device/{device_id}/config
    ESP_LOGI(TAG, "主动拉取配置（待实现）: %s", server_url.c_str());
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t MQTTConfigClient::reportStatus(const std::string& status) {
    if (!connected_ || !mqtt_client_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    std::string status_topic = "/ai_toy/config/" + device_id_ + "/status";
    int msg_id = esp_mqtt_client_publish(mqtt_client_, status_topic.c_str(), 
                                         status.c_str(), 0, 1, 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

