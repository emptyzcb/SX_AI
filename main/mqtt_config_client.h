#ifndef MQTT_CONFIG_CLIENT_H
#define MQTT_CONFIG_CLIENT_H

#include <string>
#include <functional>
#include "esp_err.h"
#include "mqtt_client.h"

/**
 * @brief MQTT配置客户端
 * 
 * 核心功能：
 * 1. 连接到MQTT Broker，订阅设备配置Topic
 * 2. 接收云端推送的配置更新
 * 3. 验证配置签名，防止篡改
 * 4. 将配置保存到本地Flash（NVS）
 * 5. 定时主动拉取配置（兜底方案）
 * 
 * MQTT Topic设计：
 * - 订阅：/ai_toy/config/{device_id}
 * - 发布：/ai_toy/config/{device_id}/status（设备状态上报）
 */
class MQTTConfigClient {
public:
    /**
     * @brief 配置更新回调函数类型
     * 
     * 当收到新的配置时，会调用此回调函数
     */
    using ConfigUpdateCallback = std::function<void(const std::string& system_prompt, 
                                                    const std::string& voice_id,
                                                    int config_version)>;
    
    /**
     * @brief 创建MQTT配置客户端
     * 
     * @param broker_uri MQTT Broker URI（格式：mqtt://host:port 或 mqtts://host:port）
     * @param device_id 设备ID
     * @param username MQTT用户名（可选）
     * @param password MQTT密码（可选）
     */
    MQTTConfigClient(const std::string& broker_uri, const std::string& device_id,
                    const std::string& username = "", const std::string& password = "");
    
    /**
     * @brief 析构函数
     */
    ~MQTTConfigClient();
    
    /**
     * @brief 初始化并连接MQTT Broker
     * 
     * @return ESP_OK=成功，其他=失败
     */
    esp_err_t connect();
    
    /**
     * @brief 断开MQTT连接
     */
    void disconnect();
    
    /**
     * @brief 设置配置更新回调
     * 
     * @param callback 回调函数
     */
    void setConfigUpdateCallback(ConfigUpdateCallback callback);
    
    /**
     * @brief 查询连接状态
     * 
     * @return true=已连接，false=未连接
     */
    bool isConnected() const { return connected_; }
    
    /**
     * @brief 主动拉取配置（从云端HTTP API）
     * 
     * 兜底方案：如果MQTT推送失败，设备可以定时主动拉取
     * 
     * @param server_url 服务器URL（如：http://115.190.153.173:8080）
     * @return ESP_OK=成功，其他=失败
     */
    esp_err_t pullConfigFromServer(const std::string& server_url);
    
    /**
     * @brief 上报设备状态到云端
     * 
     * @param status 状态信息（JSON格式）
     * @return ESP_OK=成功
     */
    esp_err_t reportStatus(const std::string& status);

private:
    /**
     * @brief MQTT事件处理函数
     */
    static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                  int32_t event_id, void* event_data);
    
    /**
     * @brief 处理收到的配置消息
     */
    void handleConfigMessage(const char* topic, const char* data, int data_len);
    
    /**
     * @brief 验证配置签名
     * 
     * @param device_id 设备ID
     * @param system_prompt 系统提示词
     * @param voice_id 音色ID
     * @param signature 配置签名
     * @return true=签名有效，false=签名无效
     */
    bool verifyConfigSignature(const std::string& device_id,
                               const std::string& system_prompt,
                               const std::string& voice_id,
                               const std::string& signature);
    
    /**
     * @brief 保存配置到NVS
     */
    esp_err_t saveConfigToNVS(const std::string& system_prompt,
                             const std::string& voice_id,
                             int config_version);
    
    /**
     * @brief 从NVS加载配置
     */
    esp_err_t loadConfigFromNVS(std::string& system_prompt,
                                std::string& voice_id,
                                int& config_version);
    
    // MQTT客户端句柄
    esp_mqtt_client_handle_t mqtt_client_;
    
    // 配置参数
    std::string broker_uri_;
    std::string device_id_;
    std::string username_;
    std::string password_;
    std::string config_topic_;  // 订阅的Topic
    
    // 状态变量
    bool connected_;
    
    // 回调函数
    ConfigUpdateCallback config_callback_;
    
    // NVS命名空间
    static constexpr const char* NVS_NAMESPACE = "device_config";
    static constexpr const char* NVS_KEY_SYSTEM_PROMPT = "system_prompt";
    static constexpr const char* NVS_KEY_VOICE_ID = "voice_id";
    static constexpr const char* NVS_KEY_CONFIG_VERSION = "config_version";
    
    // 配置签名盐值（需与服务器端一致）
    static constexpr const char* CONFIG_SALT = "AI_TOY_CONFIG_SALT_2024";
};

#endif // MQTT_CONFIG_CLIENT_H

