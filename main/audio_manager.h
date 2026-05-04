/**
 * @file audio_manager.h
 * @brief 🎧 音频管理器类 - 统一管理音频的录制和播放
 * 
 * 这个类就像一个“音频指挥家”，负责协调所有音频相关的工作：
 * 
 * 🎙️ 录音功能：
 * - 管理录音缓冲区（最多10秒）
 * - 控制录音的开始/停止
 * - 跟踪录音时长
 * 
 * 🔊 播放功能：
 * - 播放本地音频文件
 * - 流式播放网络音频
 * - 缓冲区管理，避免卡顿
 * 
 * 🌐 网络功能：
 * - 接收WebSocket音频流
 * - 处理不同采样率的音频
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

class AudioManager {
public:
    /**
     * @brief 创建音频管理器
     * 
     * @param sample_rate 采样率（默认16000Hz，人声标准）
     * @param recording_duration_sec 最大录音时长（默认10秒）
     * @param response_duration_sec AI回复最大时长（默认32秒）
     */
    AudioManager(uint32_t sample_rate = 16000, 
                 uint32_t recording_duration_sec = 10,
                 uint32_t response_duration_sec = 32);
    
    /**
     * @brief 析构函数，释放所有分配的内存
     */
    ~AudioManager();

    /**
     * @brief 初始化音频管理器
     * 
     * 这个函数会分配所需的内存缓冲区。
     * 必须在使用其他功能前调用。
     * 
     * @return ESP_OK=成功，ESP_ERR_NO_MEM=内存不足
     */
    esp_err_t init();

    /**
     * @brief 反初始化，释放资源
     */
    void deinit();

    // 🎙️ ========== 录音相关功能 ==========
    
    /**
     * @brief 开始录音
     */
    void startRecording();

    /**
     * @brief 停止录音
     */
    void stopRecording();

    /**
     * @brief 查询录音状态
     * 
     * @return true=正在录音中，false=没在录音
     */
    bool isRecording() const { return is_recording; }

    /**
     * @brief 添加音频数据到录音缓冲区
     * 
     * 每次从麦克风读到数据后，用这个函数保存起来。
     * 
     * @param data 音频数据指针
     * @param samples 样本数量（注意：不是字节数！）
     * @return true=添加成功，false=缓冲区满了
     */
    bool addRecordingData(const int16_t* data, size_t samples);

    /**
     * @brief 获取录音数据
     * 
     * 用于获取已经录制的全部音频数据。
     * 
     * @param[out] length 会被设置为录音的样本数
     * @return 指向录音数据的指针
     */
    const int16_t* getRecordingBuffer(size_t& length) const;

    /**
     * @brief 清空录音缓冲区
     */
    void clearRecordingBuffer();

    /**
     * @brief 获取已录音时间
     * 
     * @return 录音时长（单位：秒）
     */
    float getRecordingDuration() const;

    /**
     * @brief 检查录音缓冲区是否已满
     * 
     * @return true 已满，false 未满
     */
    bool isRecordingBufferFull() const;

    // 🔊 ========== 音频播放相关功能 ==========

    /**
     * @brief 开始接收响应音频数据（用于WebSocket）
     */
    void startReceivingResponse();

    /**
     * @brief 添加响应音频数据块
     * 
     * @param data 音频数据
     * @param size 数据大小（字节）
     * @return true 成功，false 失败（缓冲区溢出等）
     */
    bool addResponseData(const uint8_t* data, size_t size);

    /**
     * @brief 完成响应音频接收并播放
     * 
     * @return esp_err_t 播放结果
     */
    esp_err_t finishResponseAndPlay();
    
    // 🌊 ========== 流式播放功能（边下载边播放） ==========
    
    /**
     * @brief 开始流式播放模式
     * 
     * 调用后可以不断添加音频数据块，实现边下载边播放。
     */
    void startStreamingPlayback();
    
    /**
     * @brief 添加一小段音频到播放队列
     * 
     * 在流式播放模式下，不断调用这个函数添加新的音频段。
     * 实现了以下优化：
     * - 动态缓冲水位监控（低于20%时通知服务器加速）
     * - 数据丢弃策略（溢出时丢弃最旧数据）
     * - 网络断流恢复（重连后重新预缓冲）
     * 
     * @param data 音频数据
     * @param size 数据字节数
     * @return true=添加成功，false=缓冲区满
     */
    bool addStreamingAudioChunk(const uint8_t* data, size_t size);
    
    /**
     * @brief 检查并处理缓冲区状态（需要在主循环中定期调用）
     * 
     * 实现功能：
     * - 缓冲区空转检测（500ms无数据自动停止）
     * - 动态缓冲水位监控
     * 
     * @return true=播放正常，false=需要停止播放
     */
    bool checkStreamingStatus();
    
    /**
     * @brief 处理流式播放（用于收到ping包后继续播放缓冲区数据）
     * 
     * 当停止接收新数据后，调用此函数继续播放缓冲区中的剩余数据。
     * 
     * @return true=还有数据需要播放，false=缓冲区已空
     */
    bool processStreamingPlayback();
    
    /**
     * @brief 结束流式播放
     * 
     * 播放剩余的音频数据并停止流式模式。
     */
    void finishStreamingPlayback();
    
    /**
     * @brief 检查流式播放是否正在进行
     * 
     * @return true 正在播放，false 未在播放
     */
    bool isStreamingActive() const { return is_streaming; }
    
    /**
     * @brief 检查流式播放缓冲区是否为空
     * 
     * @return true 为空，false 不为空
     */
    bool isStreamingBufferEmpty() const { 
        if (!is_streaming) return true;
        return streaming_write_pos == streaming_read_pos; 
    }

    /**
     * @brief 紧急停止所有音频播放
     * 
     * 立即停止流式播放，清空缓冲区，停止I2S输出。
     * 用于异常情况下的强制停止。
     */
    void emergencyStopAudio();
    
    /**
     * @brief 标记流式播放已完成
     */
    void setStreamingComplete() { response_played = true; }
    
    /**
     * @brief 获取流式缓冲区写入位置
     */
    size_t getStreamingWritePos() const { return streaming_write_pos; }
    
    /**
     * @brief 获取流式缓冲区读取位置
     */
    size_t getStreamingReadPos() const { return streaming_read_pos; }
    
    /**
     * @brief 获取流式缓冲区大小
     */
    size_t getStreamingBufferSize() const { return streaming_buffer_size; }
    
    /**
     * @brief 设置WebSocket客户端指针（用于发送加速请求）
     * 
     * @param ws_client WebSocket客户端指针（可以为nullptr）
     */
    void setWebSocketClient(void* ws_client) { websocket_client = ws_client; }
    
    /**
     * @brief 标记网络重连（需要重新预缓冲）
     */
    void setNetworkReconnected() { network_reconnected = true; }

    /**
     * @brief 播放一段完整的音频
     * 
     * 用于播放本地存储的音频文件，一次性播放完毕。
     * 
     * @param audio_data 音频数据（PCM格式）
     * @param data_len 数据字节数
     * @param description 音频描述（如“欢迎音频”）
     * @return ESP_OK=播放成功
     */
    esp_err_t playAudio(const uint8_t* audio_data, size_t data_len, const char* description);

    /**
     * @brief 查询AI回复是否播放完成
     * 
     * @return true=已播放完成，false=还没播完
     */
    bool isResponsePlayed() const { return response_played; }

    /**
     * @brief 重置响应播放标志
     */
    void resetResponsePlayedFlag() { response_played = false; }


    // 🔧 ========== 工具函数 ==========

    /**
     * @brief 获取采样率
     * 
     * @return 采样率（Hz）
     */
    uint32_t getSampleRate() const { return sample_rate; }

    /**
     * @brief 获取录音缓冲区大小（样本数）
     * 
     * @return 缓冲区大小
     */
    size_t getRecordingBufferSize() const { return recording_buffer_size; }

    /**
     * @brief 获取响应缓冲区大小（字节）
     * 
     * @return 缓冲区大小
     */
    size_t getResponseBufferSize() const { return response_buffer_size; }

private:
    // 🎶 音频参数
    uint32_t sample_rate;               // 采样率（Hz）
    uint32_t recording_duration_sec;    // 最大录音时长（秒）
    uint32_t response_duration_sec;     // 最大回复时长（秒）

    // 🎙️ 录音相关变量
    int16_t* recording_buffer;          // 录音数据缓冲区
    size_t recording_buffer_size;       // 缓冲区大小（样本数）
    size_t recording_length;            // 已录制的样本数
    bool is_recording;                  // 是否正在录音

    // 🔊 响应音频相关变量
    int16_t* response_buffer;           // AI回复音频缓冲区
    size_t response_buffer_size;        // 缓冲区大小（字节数）
    size_t response_length;             // 已接收的样本数
    bool response_played;               // 是否已播放完成

    
    // 🌊 流式播放相关变量
    bool is_streaming;                  // 是否在流式播放中
    uint8_t* streaming_buffer;          // 环形缓冲区
    size_t streaming_buffer_size;       // 缓冲区大小
    size_t streaming_write_pos;         // 写入位置
    size_t streaming_read_pos;          // 读取位置
    bool streaming_started;             // 是否已开始播放（用于预缓冲）
    void* websocket_client;             // WebSocket客户端指针（用于发送加速请求）
    
    // 🎯 抗卡顿优化相关变量
    int64_t last_data_time;            // 最后一次收到数据的时间（微秒）
    int i2s_write_timeout_count;       // I2S写入超时计数
    bool network_reconnected;           // 网络重连标志（需要重新预缓冲）
    
    static const size_t STREAMING_BUFFER_SIZE = 65536; // 64KB环形缓冲区
    static const size_t STREAMING_CHUNK_SIZE = 1024;   // 每次播放1024字节（约60ms @ 16kHz）
    static const size_t STREAMING_PREBUFFER_SIZE = 8000; // 预缓冲500ms再开始播放
    static const float BUFFER_LOW_THRESHOLD; // 缓冲区低水位阈值（20%）
    static const int I2S_TIMEOUT_MAX_COUNT;  // I2S超时最大次数（3次）
    static const int BUFFER_IDLE_TIMEOUT_MS; // 缓冲区空转超时（500ms）

    // 🏷️ 日志标签
    static const char* TAG;
};

#endif // AUDIO_MANAGER_H