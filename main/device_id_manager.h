#ifndef DEVICE_ID_MANAGER_H
#define DEVICE_ID_MANAGER_H

#include <string>
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "mbedtls/md5.h"

/**
 * @brief 设备ID管理器
 * 
 * 核心功能：
 * 1. 基于ESP32硬件唯一ID（MAC地址）生成设备唯一标识
 * 2. 使用MD5哈希+盐值确保设备ID的唯一性和安全性
 * 3. 将设备ID加密存储到Flash（NVS），仅可读不可改
 * 4. 提供设备ID读取接口
 * 
 * 设计原则：
 * - Device ID一旦生成，永久不变
 * - 即使设备重置，Device ID也保持不变（基于硬件MAC地址）
 * - 防止Device ID被伪造或篡改
 */
class DeviceIDManager {
public:
    /**
     * @brief 初始化设备ID管理器
     * 
     * 首次调用时会生成并保存设备ID，后续调用直接读取
     * 
     * @return ESP_OK=成功，其他=失败
     */
    static esp_err_t init();
    
    /**
     * @brief 获取设备唯一ID
     * 
     * @return 设备ID字符串（32位MD5哈希值，十六进制格式）
     */
    static std::string get_device_id();
    
    /**
     * @brief 获取原始MAC地址（用于调试）
     * 
     * @return MAC地址字符串（格式：XX:XX:XX:XX:XX:XX）
     */
    static std::string get_mac_address();
    
    /**
     * @brief 检查设备ID是否已生成
     * 
     * @return true=已生成，false=未生成
     */
    static bool is_device_id_ready();

private:
    /**
     * @brief 从NVS读取设备ID
     * 
     * @param device_id 输出参数，存储读取到的设备ID
     * @return ESP_OK=成功，ESP_ERR_NVS_NOT_FOUND=未找到
     */
    static esp_err_t read_device_id_from_nvs(std::string& device_id);
    
    /**
     * @brief 生成设备ID并保存到NVS
     * 
     * 生成逻辑：
     * 1. 读取ESP32的MAC地址
     * 2. 使用MD5哈希算法（MAC地址 + 盐值）生成32位哈希值
     * 3. 将哈希值转换为十六进制字符串作为Device ID
     * 4. 保存到NVS Flash
     * 
     * @param device_id 输出参数，存储生成的设备ID
     * @return ESP_OK=成功，其他=失败
     */
    static esp_err_t generate_and_save_device_id(std::string& device_id);
    
    /**
     * @brief 计算MD5哈希值
     * 
     * @param input 输入数据
     * @param input_len 输入数据长度
     * @param output 输出缓冲区（至少16字节）
     * @return ESP_OK=成功
     */
    static esp_err_t calculate_md5(const uint8_t* input, size_t input_len, uint8_t* output);
    
    // NVS命名空间
    static constexpr const char* NVS_NAMESPACE = "device_id";
    static constexpr const char* NVS_KEY_DEVICE_ID = "device_id";
    static constexpr const char* NVS_KEY_MAC_ADDR = "mac_addr";
    
    // 盐值（用于增强安全性，防止MAC地址被逆向）
    static constexpr const char* SALT = "AI_TOY_DEVICE_SALT_2024";
    
    // 设备ID缓存（避免频繁读取NVS）
    static std::string cached_device_id;
    static bool initialized;
};

#endif // DEVICE_ID_MANAGER_H

