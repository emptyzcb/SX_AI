#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include <functional>

/**
 * @brief 🌐 WebSocket客户端类 - 与服务器实时通信
 * 
 * 这个类封装了WebSocket协议，让ESP32能够和电脑上的服务器进行实时双向通信。
 * 
 * 🎆 主要特点：
 * - 支持文本和二进制数据传输
 * - 自动重连机制（断线后自动重连）
 * - 事件回调机制（连接、断开、收到数据等）
 * 
 * 📡 应用场景：
 * - 发送录音数据给服务器
 * - 接收服务器返回的AI语音
 * - 发送控制消息（如唤醒事件）
 */
class WebSocketClient {
public:
    /**
     * @brief WebSocket事件类型
     * 
     * 这些是WebSocket可能发生的各种事件。
     */
    enum class EventType {
        CONNECTED,      // 🔗 连接成功
        DISCONNECTED,   // 🔌 连接断开
        DATA_TEXT,      // 📝 收到文本数据（如JSON）
        DATA_BINARY,    // 📦 收到二进制数据（如音频）
        PING,           // 🏓 收到ping（心跳检测）
        PONG,           // 🏐 收到pong（心跳回应）
        ERROR           // ❌ 发生错误
    };
    
    /**
     * @brief WebSocket事件数据结构
     * 
     * 当发生事件时，会把相关信息打包在这个结构里。
     */
    struct EventData {
        EventType type;         // 事件类型
        const uint8_t* data;    // 数据指针（可能为空）
        size_t data_len;        // 数据长度
        int op_code;            // WebSocket操作码
    };
    
    /**
     * @brief 事件回调函数类型
     * 
     * 用户可以提供一个函数，当事件发生时会被调用。
     */
    using EventCallback = std::function<void(const EventData&)>;
    
    /**
     * @brief 创建WebSocket客户端
     * 
     * @param uri 服务器地址（如 ws://192.168.1.100:8888）
     * @param auto_reconnect 是否自动重连（默认开启）
     * @param reconnect_interval_ms 重连间隔时间（默认5秒）
     */
    WebSocketClient(const std::string& uri, bool auto_reconnect = true, 
                   int reconnect_interval_ms = 5000);
    
    /**
     * @brief 析构函数
     */
    ~WebSocketClient();
    
    /**
     * @brief 设置事件处理函数
     * 
     * 当WebSocket发生事件时，会调用您设置的这个函数。
     * 
     * @param callback 事件处理函数
     */
    void setEventCallback(EventCallback callback);
    
    /**
     * @brief 连接到服务器
     * 
     * 调用后会尝试连接到构造函数中指定的服务器。
     * 
     * @return ESP_OK=成功，其他=失败
     */
    esp_err_t connect();
    
    /**
     * @brief 断开WebSocket连接
     */
    void disconnect();
    
    /**
     * @brief 发送文本消息
     * 
     * 用于发送JSON等文本格式的数据。
     * 
     * @param text 要发送的文本内容
     * @param timeout_ms 超时时间（默认永不超时）
     * @return 发送的字节数，-1=失败
     */
    int sendText(const std::string& text, int timeout_ms = portMAX_DELAY);
    
    /**
     * @brief 发送二进制数据
     * 
     * 用于发送音频等二进制格式的数据。
     * 
     * @param data 数据指针
     * @param len 数据字节数
     * @param timeout_ms 超时时间（默认永不超时）
     * @return 发送的字节数，-1=失败
     */
    int sendBinary(const uint8_t* data, size_t len, int timeout_ms = portMAX_DELAY);
    
    /**
     * @brief 发送ping包
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t sendPing();
    
    /**
     * @brief 查询连接状态
     * 
     * @return true=已连接，false=未连接
     */
    bool isConnected() const { return connected_; }
    
    /**
     * @brief 设置是否自动重连
     * @param enable true启用自动重连，false禁用
     */
    void setAutoReconnect(bool enable) { auto_reconnect_ = enable; }
    
    /**
     * @brief 设置重连间隔
     * @param interval_ms 重连间隔（毫秒）
     */
    void setReconnectInterval(int interval_ms) { reconnect_interval_ms_ = interval_ms; }

private:
    // WebSocket事件处理器
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                      int32_t event_id, void* event_data);
    
    // 重连任务
    static void reconnect_task(void* arg);
    
    // 配置参数
    std::string uri_;
    bool auto_reconnect_;
    int reconnect_interval_ms_;
    
    // WebSocket客户端句柄
    esp_websocket_client_handle_t client_;
    
    // 状态变量
    bool connected_;
    
    // 重连任务句柄
    TaskHandle_t reconnect_task_handle_;
    
    // 事件回调
    EventCallback event_callback_;
    
    // 📦 内部配置常量
    static constexpr int BUFFER_SIZE = 16384;               // 数据缓冲区大小（16KB，支持大音频块）
    static constexpr int TASK_STACK_SIZE = 8192;            // WebSocket任务栈大小
    static constexpr int RECONNECT_TASK_STACK_SIZE = 4096;  // 重连任务栈大小
};

#endif // WEBSOCKET_CLIENT_H