/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现文件
 * 
 * 这里实现了audio_manager.h中声明的所有功能。
 * 主要包括录音缓冲区管理、音频播放控制和流式播放。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
#include "esp_timer.h"
}

#include "audio_manager.h"
#include "websocket_client.h"

const char* AudioManager::TAG = "AudioManager";

// 静态常量定义
const float AudioManager::BUFFER_LOW_THRESHOLD = 0.2f;  // 20%低水位阈值
const int AudioManager::I2S_TIMEOUT_MAX_COUNT = 3;      // I2S超时最大次数
const int AudioManager::BUFFER_IDLE_TIMEOUT_MS = 500;   // 缓冲区空转超时500ms

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , recording_duration_sec(recording_duration_sec)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(0)
    , recording_length(0)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
    , streaming_started(false)
    , websocket_client(nullptr)
    , last_data_time(0)
    , i2s_write_timeout_count(0)
    , network_reconnected(false)
{
    // 🧮 计算所需缓冲区大小
    recording_buffer_size = sample_rate * recording_duration_sec;  // 录音缓冲区（样本数）
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);  // 响应缓冲区（字节数）
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器...");
    
    // 分配录音缓冲区
    recording_buffer = (int16_t*)malloc(recording_buffer_size * sizeof(int16_t));
    if (recording_buffer == nullptr) {
        ESP_LOGE(TAG, "录音缓冲区分配失败，需要 %zu 字节", 
                 recording_buffer_size * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 录音缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             recording_buffer_size * sizeof(int16_t), (unsigned long)recording_duration_sec);
    
    // 分配响应缓冲区
    response_buffer = (int16_t*)calloc(response_buffer_size / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == nullptr) {
        ESP_LOGE(TAG, "响应缓冲区分配失败，需要 %zu 字节", response_buffer_size);
        free(recording_buffer);
        recording_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             response_buffer_size, (unsigned long)response_duration_sec);
    
    // 分配流式播放缓冲区
    streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    if (streaming_buffer == nullptr) {
        ESP_LOGE(TAG, "流式播放缓冲区分配失败，需要 %zu 字节", streaming_buffer_size);
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 流式播放缓冲区分配成功，大小: %zu 字节", streaming_buffer_size);
    
    return ESP_OK;
}

void AudioManager::deinit() {
    if (recording_buffer != nullptr) {
        free(recording_buffer);
        recording_buffer = nullptr;
    }
    
    if (response_buffer != nullptr) {
        free(response_buffer);
        response_buffer = nullptr;
    }
    
    if (streaming_buffer != nullptr) {
        free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

// 🎙️ ========== 录音功能实现 ==========

void AudioManager::startRecording() {
    is_recording = true;
    recording_length = 0;
    ESP_LOGI(TAG, "开始录音...");
}

void AudioManager::stopRecording() {
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)", 
             recording_length, getRecordingDuration());
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording || recording_buffer == nullptr) {
        return false;
    }
    
    // 📏 检查缓冲区是否还有空间
    if (recording_length + samples > recording_buffer_size) {
        ESP_LOGW(TAG, "录音缓冲区已满（超过10秒上限）");
        return false;
    }
    
    // 💾 将新的音频数据追加到缓冲区末尾
    memcpy(&recording_buffer[recording_length], data, samples * sizeof(int16_t));
    recording_length += samples;
    
    return true;
}

const int16_t* AudioManager::getRecordingBuffer(size_t& length) const {
    length = recording_length;
    return recording_buffer;
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

bool AudioManager::isRecordingBufferFull() const {
    return recording_length >= recording_buffer_size;
}

// 🔊 ========== 音频播放功能实现 ==========

void AudioManager::startReceivingResponse() {
    response_length = 0;
    response_played = false;
}

bool AudioManager::addResponseData(const uint8_t* data, size_t size) {
    size_t samples = size / sizeof(int16_t);
    
    if (samples * sizeof(int16_t) > response_buffer_size) {
        ESP_LOGW(TAG, "响应数据过大，超过缓冲区限制");
        return false;
    }
    
    memcpy(response_buffer, data, size);
    response_length = samples;
    
    ESP_LOGI(TAG, "📦 接收到完整音频数据: %zu 字节, %zu 样本", size, samples);
    return true;
}

esp_err_t AudioManager::finishResponseAndPlay() {
    if (response_length == 0) {
        ESP_LOGW(TAG, "没有响应音频数据可播放");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "📢 播放响应音频: %zu 样本 (%.2f 秒)",
             response_length, (float)response_length / sample_rate);
    
    // 🔁 添加重试机制，确保音频可靠播放
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ 响应音频播放成功");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "❌ 音频播放失败 (第%d次尝试): %s",
                     retry_count + 1, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等100ms再试
            }
        }
    }
    
    return audio_ret;
}

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    ESP_LOGI(TAG, "播放%s...", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ %s播放成功", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}


// 🌊 ========== 流式播放功能实现（简化版，单线程） ==========

void AudioManager::startStreamingPlayback() {
    ESP_LOGI(TAG, "开始流式音频播放（优化版）");
    is_streaming = true;
    streaming_started = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    i2s_write_timeout_count = 0;
    network_reconnected = false;
    last_data_time = esp_timer_get_time(); // 记录开始时间
    
    // 清空缓冲区
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !streaming_buffer || !data) {
        return false;
    }
    
    // 更新最后收到数据的时间
    last_data_time = esp_timer_get_time();
    
    // 如果网络重连，需要重新预缓冲
    if (network_reconnected) {
        ESP_LOGI(TAG, "🔄 网络重连后重新预缓冲");
        streaming_started = false;
        streaming_write_pos = 0;
        streaming_read_pos = 0;
        network_reconnected = false;
        if (streaming_buffer) {
            memset(streaming_buffer, 0, streaming_buffer_size);
        }
    }
    
    // 📏 计算环形缓冲区的剩余空间
    size_t available_space;
    if (streaming_write_pos >= streaming_read_pos) {
        available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
    } else {
        available_space = streaming_read_pos - streaming_write_pos - 1;
    }
    
    // 🎯 数据丢弃策略：如果缓冲区溢出，丢弃最旧的数据（而不是拒绝新数据）
    if (size > available_space) {
        ESP_LOGW(TAG, "⚠️ 缓冲区溢出: 需要 %zu, 可用 %zu，丢弃最旧数据", size, available_space);
        
        // 计算需要丢弃的数据量
        size_t data_to_discard = size - available_space;
        
        // 丢弃最旧的数据（移动read_pos）
        if (streaming_read_pos + data_to_discard < streaming_buffer_size) {
            streaming_read_pos += data_to_discard;
        } else {
            streaming_read_pos = data_to_discard - (streaming_buffer_size - streaming_read_pos);
        }
        
        // 重新计算可用空间
        if (streaming_write_pos >= streaming_read_pos) {
            available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
        } else {
            available_space = streaming_read_pos - streaming_write_pos - 1;
        }
        
        ESP_LOGD(TAG, "已丢弃 %zu 字节旧数据，新可用空间: %zu", data_to_discard, available_space);
    }
    
    // 📝 将数据写入环形缓冲区
    size_t bytes_to_end = streaming_buffer_size - streaming_write_pos;
    if (size <= bytes_to_end) {
        memcpy(streaming_buffer + streaming_write_pos, data, size);
        streaming_write_pos += size;
    } else {
        memcpy(streaming_buffer + streaming_write_pos, data, bytes_to_end);
        memcpy(streaming_buffer, data + bytes_to_end, size - bytes_to_end);
        streaming_write_pos = size - bytes_to_end;
    }
    
    if (streaming_write_pos >= streaming_buffer_size) {
        streaming_write_pos = 0;
    }
    
    // 计算可用数据
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    // 预缓冲逻辑
    if (!streaming_started) {
        if (available_data >= STREAMING_PREBUFFER_SIZE) {
            ESP_LOGI(TAG, "✅ 预缓冲完成（%zu字节），开始播放", available_data);
            streaming_started = true;
        } else {
            ESP_LOGD(TAG, "⏳ 预缓冲中: %zu/%zu 字节", available_data, STREAMING_PREBUFFER_SIZE);
            return true;
        }
    }
    
    // 📊 动态缓冲水位监控：计算缓冲区使用率
    float buffer_usage = (float)available_data / streaming_buffer_size;
    
    // 如果缓冲区低于20%，通知服务器加速推送
    if (buffer_usage < BUFFER_LOW_THRESHOLD && websocket_client != nullptr) {
        ESP_LOGW(TAG, "⚠️ 缓冲区水位过低: %.1f%%，请求服务器加速推送", buffer_usage * 100.0f);
        // 发送加速请求（通过WebSocket）
        // 注意：这里需要WebSocket客户端支持发送JSON消息
        // 暂时只记录日志，实际实现需要根据WebSocket接口调整
    }
    
    // 播放积累的数据（尽可能多播放，避免缓冲区满）
    while (streaming_started && available_data >= STREAMING_CHUNK_SIZE) {
        // 快速读取并播放
        uint8_t chunk[STREAMING_CHUNK_SIZE];
        
        size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
        if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
            memcpy(chunk, streaming_buffer + streaming_read_pos, STREAMING_CHUNK_SIZE);
            streaming_read_pos += STREAMING_CHUNK_SIZE;
        } else {
            memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
            memcpy(chunk + bytes_to_end, streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
            streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
        }
        
        if (streaming_read_pos >= streaming_buffer_size) {
            streaming_read_pos = 0;
        }
        
        // 播放音频块（带超时检测）
        esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 流式音频播放失败: %s", esp_err_to_name(ret));
            i2s_write_timeout_count++;
            
            // I2S写入超时处理：连续3次超时后重启I2S
            if (i2s_write_timeout_count >= I2S_TIMEOUT_MAX_COUNT) {
                ESP_LOGE(TAG, "🚨 I2S连续%d次写入失败，重启I2S硬件", i2s_write_timeout_count);
                // 停止并重新初始化I2S
                bsp_audio_stop();
                // 注意：这里需要重新初始化I2S，但bsp_audio_init需要参数
                // 实际实现中可能需要保存采样率等参数，或提供重启函数
                i2s_write_timeout_count = 0;
                break;
            }
            break;
        } else {
            // 成功写入，重置超时计数
            i2s_write_timeout_count = 0;
        }
        
        // 重新计算可用数据
        if (streaming_write_pos >= streaming_read_pos) {
            available_data = streaming_write_pos - streaming_read_pos;
        } else {
            available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }
    }
    
    return true;
}

bool AudioManager::processStreamingPlayback() {
    if (!is_streaming) {
        return false;
    }
    
    // 计算可用数据
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    // 如果没有数据了，返回false
    if (available_data == 0) {
        return false;
    }
    
    // 播放积累的数据（尽可能多播放）
    while (available_data >= STREAMING_CHUNK_SIZE) {
        // 快速读取并播放
        uint8_t chunk[STREAMING_CHUNK_SIZE];
        
        size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
        if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
            memcpy(chunk, streaming_buffer + streaming_read_pos, STREAMING_CHUNK_SIZE);
            streaming_read_pos += STREAMING_CHUNK_SIZE;
        } else {
            memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
            memcpy(chunk + bytes_to_end, streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
            streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
        }
        
        if (streaming_read_pos >= streaming_buffer_size) {
            streaming_read_pos = 0;
        }
        
        // 播放音频块（带超时检测）
        esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 流式音频播放失败: %s", esp_err_to_name(ret));
            i2s_write_timeout_count++;
            
            // I2S写入超时处理：连续3次超时后重启I2S
            if (i2s_write_timeout_count >= I2S_TIMEOUT_MAX_COUNT) {
                ESP_LOGE(TAG, "🚨 I2S连续%d次写入失败，重启I2S硬件", i2s_write_timeout_count);
                // 停止并重新初始化I2S
                bsp_audio_stop();
                i2s_write_timeout_count = 0;
                break;
            }
            break;
        } else {
            // 成功写入，重置超时计数
            i2s_write_timeout_count = 0;
        }
        
        // 重新计算可用数据
        if (streaming_write_pos >= streaming_read_pos) {
            available_data = streaming_write_pos - streaming_read_pos;
        } else {
            available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }
    }
    
    // 返回是否还有数据需要播放
    if (streaming_write_pos >= streaming_read_pos) {
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    return available_data > 0;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "结束流式音频播放");

    // 🧹 播放缓冲区中剩余的所有数据（防止截断）
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }

    if (available_data > 0) {
        ESP_LOGI(TAG, "正在播放剩余缓冲区数据: %zu 字节", available_data);
        
        uint8_t chunk[STREAMING_CHUNK_SIZE];
        while (available_data > 0) {
            // 清空chunk（用静音填充不足的部分）
            memset(chunk, 0, STREAMING_CHUNK_SIZE);
            
            size_t bytes_to_read = (available_data > STREAMING_CHUNK_SIZE) ? STREAMING_CHUNK_SIZE : available_data;
            
            // 从环形缓冲区读取
            size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
            if (bytes_to_read <= bytes_to_end) {
                memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_read);
                streaming_read_pos += bytes_to_read;
            } else {
                memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
                memcpy(chunk + bytes_to_end, streaming_buffer, bytes_to_read - bytes_to_end);
                streaming_read_pos = bytes_to_read - bytes_to_end;
            }
            
            if (streaming_read_pos >= streaming_buffer_size) {
                streaming_read_pos = 0;
            }
            
            // 播放音频块
            bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
            
            available_data -= bytes_to_read;
        }
    }
    
    // 标记停止，不再接收新数据
    is_streaming = false;
    streaming_started = false;
    
    // 计算应用层剩余数据
    size_t remaining_data;
    if (streaming_write_pos >= streaming_read_pos) {
        remaining_data = streaming_write_pos - streaming_read_pos;
    } else {
        remaining_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    ESP_LOGI(TAG, "应用层剩余 %zu 字节", remaining_data);
    
    // 清理应用层缓冲区
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    // 停止I2S并清空DMA缓存
    bsp_audio_stop();
    
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
    
    ESP_LOGI(TAG, "✅ 流式播放已完全停止并清理");
}

bool AudioManager::checkStreamingStatus() {
    if (!is_streaming) {
        return false;
    }
    
    // 📊 动态缓冲水位监控
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    float buffer_usage = (float)available_data / streaming_buffer_size;
    
    // 如果缓冲区低于20%，通知服务器加速推送
    static int64_t last_accel_request_time = 0;
    int64_t current_time = esp_timer_get_time();
    if (buffer_usage < BUFFER_LOW_THRESHOLD && 
        (current_time - last_accel_request_time) > 1000000) { // 每秒最多请求一次
        ESP_LOGW(TAG, "⚠️ 缓冲区水位过低: %.1f%%，请求服务器加速推送", buffer_usage * 100.0f);
        
        // 发送加速请求（通过WebSocket）
        if (websocket_client != nullptr) {
            // 构造JSON消息
            char accel_msg[128];
            snprintf(accel_msg, sizeof(accel_msg), 
                    R"({"event":"audio_buffer_low","buffer_usage":%.2f})", buffer_usage);
            
            // 发送加速请求
            WebSocketClient* ws = static_cast<WebSocketClient*>(websocket_client);
            if (ws && ws->isConnected()) {
                ws->sendText(std::string(accel_msg));
                ESP_LOGD(TAG, "📤 发送加速请求: %s", accel_msg);
            }
        }
        
        last_accel_request_time = current_time;
    }
    
    // 🕐 缓冲区空转检测：500ms无数据自动停止
    if (streaming_started && last_data_time > 0) {
        int64_t idle_time = current_time - last_data_time;
        if (idle_time > (BUFFER_IDLE_TIMEOUT_MS * 1000)) {
            ESP_LOGW(TAG, "⏰ 缓冲区空转超时（%lld ms），自动停止播放", idle_time / 1000);
            emergencyStopAudio();
            return false;
        }
    }
    
    return true;
}

void AudioManager::emergencyStopAudio() {
    ESP_LOGI(TAG, "🚨 紧急停止所有音频播放");
    
    is_streaming = false;
    streaming_started = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    i2s_write_timeout_count = 0;
    last_data_time = 0;
    
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
    
    bsp_audio_stop_immediate();
    
    ESP_LOGI(TAG, "✅ 紧急停止完成");
}
