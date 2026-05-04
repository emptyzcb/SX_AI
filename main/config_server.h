#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#include "esp_err.h"
#include <string>

/**
 * @brief 配网HTTP服务器
 * 
 * 用于AP模式下的WiFi配网，用户连接设备热点后，
 * 访问 http://192.168.4.1 进行WiFi配置。
 */

/**
 * @brief 启动配网HTTP服务器
 * 
 * @return ESP_OK=启动成功，ESP_FAIL=启动失败
 */
esp_err_t start_config_server();

/**
 * @brief 停止配网HTTP服务器
 * 
 * @return ESP_OK=停止成功，ESP_FAIL=停止失败
 */
esp_err_t stop_config_server();

/**
 * @brief 检查是否有新的WiFi配置
 * 
 * @param ssid 输出参数，WiFi名称
 * @param password 输出参数，WiFi密码
 * @return true=有新配置，false=没有新配置
 */
bool get_wifi_config(std::string& ssid, std::string& password);

/**
 * @brief 与 POST /wifi/config 相同的配网逻辑（JSON 明文或整段 Hex 编码的 JSON）。
 * @return 0 成功；-1 解析/类型错误；-2 SSID 或密码校验失败；-3 NVS 失败
 */
#ifdef __cplusplus
extern "C" {
#endif
int provision_apply_wifi_json(const char* json_body, size_t body_len);
#ifdef __cplusplus
}
#endif

/**
 * @brief 从JSON字符串中提取指定字段的值
 * 
 * @param body JSON字符串
 * @param key 字段名
 * @param value 输出参数，字段值
 * @return true=提取成功，false=提取失败
 */
bool extract_json_string_field(const std::string& body, const char* key, std::string& value);

#endif // CONFIG_SERVER_H

