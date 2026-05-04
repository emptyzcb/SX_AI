#include "device_id_manager.h"
#include "esp_log.h"
#include <cstring>
#include <sstream>
#include <iomanip>

static const char *TAG = "DeviceID";

// 静态成员变量定义
std::string DeviceIDManager::cached_device_id = "";
bool DeviceIDManager::initialized = false;

esp_err_t DeviceIDManager::init() {
    if (initialized) {
        return ESP_OK;
    }
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS分区被占用或版本不匹配，需要擦除
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 尝试从NVS读取设备ID
    std::string device_id;
    ret = read_device_id_from_nvs(device_id);
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // 设备ID不存在，需要生成
        ESP_LOGI(TAG, "设备ID不存在，开始生成...");
        ret = generate_and_save_device_id(device_id);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "生成设备ID失败");
            return ret;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取设备ID失败");
        return ret;
    }
    
    // 缓存设备ID
    cached_device_id = device_id;
    initialized = true;
    
    ESP_LOGI(TAG, "设备ID初始化成功: %s", device_id.c_str());
    return ESP_OK;
}

std::string DeviceIDManager::get_device_id() {
    if (!initialized) {
        init();
    }
    
    if (cached_device_id.empty()) {
        // 如果缓存为空，尝试从NVS读取
        std::string device_id;
        if (read_device_id_from_nvs(device_id) == ESP_OK) {
            cached_device_id = device_id;
        } else {
            ESP_LOGE(TAG, "无法获取设备ID");
            return "UNKNOWN";
        }
    }
    
    return cached_device_id;
}

std::string DeviceIDManager::get_mac_address() {
    uint8_t mac[6];
    // 直接读取芯片硬件基准 MAC，避免拿到接口态（AP/STA）运行时派生值。
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取MAC地址失败");
        return "UNKNOWN";
    }
    
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

bool DeviceIDManager::is_device_id_ready() {
    if (!initialized) {
        init();
    }
    return !cached_device_id.empty();
}

esp_err_t DeviceIDManager::read_device_id_from_nvs(std::string& device_id) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 读取设备ID（最大64字节）
    char device_id_buf[65] = {0};
    size_t required_size = sizeof(device_id_buf);
    ret = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id_buf, &required_size);
    
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        device_id = std::string(device_id_buf);
    }
    
    return ret;
}

esp_err_t DeviceIDManager::generate_and_save_device_id(std::string& device_id) {
    // 1. 获取MAC地址
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取MAC地址失败");
        return ret;
    }
    
    // 2. 准备哈希输入数据（MAC地址 + 盐值）
    size_t salt_len = strlen(SALT);
    size_t input_len = 6 + salt_len;  // MAC地址6字节 + 盐值
    uint8_t* input_data = (uint8_t*)malloc(input_len);
    if (input_data == nullptr) {
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_ERR_NO_MEM;
    }
    
    // 复制MAC地址
    memcpy(input_data, mac, 6);
    // 复制盐值
    memcpy(input_data + 6, SALT, salt_len);
    
    // 3. 计算MD5哈希值
    uint8_t md5_hash[16];
    ret = calculate_md5(input_data, input_len, md5_hash);
    free(input_data);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "计算MD5哈希失败");
        return ret;
    }
    
    // 4. 将MD5哈希值转换为十六进制字符串
    std::stringstream ss;
    for (int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)md5_hash[i];
    }
    device_id = ss.str();
    
    // 5. 保存到NVS
    nvs_handle_t nvs_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败");
        return ret;
    }
    
    // 保存设备ID
    ret = nvs_set_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存设备ID到NVS失败");
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 保存MAC地址（用于调试和验证）
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    nvs_set_str(nvs_handle, NVS_KEY_MAC_ADDR, mac_str);
    
    // 提交更改
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "提交NVS更改失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "设备ID生成并保存成功: %s (MAC: %s)", device_id.c_str(), mac_str);
    return ESP_OK;
}

esp_err_t DeviceIDManager::calculate_md5(const uint8_t* input, size_t input_len, uint8_t* output) {
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, input, input_len);
    mbedtls_md5_finish(&ctx, output);
    mbedtls_md5_free(&ctx);
    return ESP_OK;
}

