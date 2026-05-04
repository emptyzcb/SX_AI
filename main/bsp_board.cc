/**
 * ESP32-S3-DevKitC-1 with INMP441 microphone board support
 * ESP32-S3-DevKitC-1 开发板配合 INMP441 麦克风的硬件抽象层实现
 *
 * @copyright Copyright 2021 Espressif Systems (Shanghai) Co. Ltd.
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *               http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include <string.h>
#include "bsp_board.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_system.h"
#include <stdio.h>
#include "es8311.h"


///////

#define BSP_I2C_SDA           (GPIO_NUM_1)
#define BSP_I2C_SCL           (GPIO_NUM_2)
#define BSP_I2C_NUM           I2C_NUM_0
#define BSP_I2C_FREQ_HZ       (100000)

#define EXAMPLE_RECV_BUF_SIZE   (2400)
#define EXAMPLE_SAMPLE_RATE     (8000)
#define EXAMPLE_VOICE_VOLUME    (60)

#define I2S_NUM I2S_NUM_0
#define I2S_MCK_IO      (GPIO_NUM_38)
#define I2S_BCK_IO      (GPIO_NUM_14)
#define I2S_WS_IO       (GPIO_NUM_13)
#define I2S_DO_IO       (GPIO_NUM_45)
#define I2S_DI_IO       (GPIO_NUM_11)
#define EXAMPLE_MCLK_MULTIPLE   I2S_MCLK_MULTIPLE_256
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
////////

// MAX98357A I2S 输出引脚配置
// MAX98357A 是一个数字音频功放，通过 I2S 接口接收音频数据
#define I2S_OUT_BCLK_PIN GPIO_NUM_15 // 位时钟信号 (Bit Clock)
#define I2S_OUT_LRC_PIN GPIO_NUM_16  // 左右声道时钟信号 (LR Clock)
#define I2S_OUT_DIN_PIN GPIO_NUM_7   // 数据输入信号 (Data Input)
#define I2S_OUT_SD_PIN GPIO_NUM_8    // Shutdown引脚 (可选，用于关闭功放)

// I2S 配置参数
#define SAMPLE_RATE 16000     // 采样率 16kHz，适合语音识别
#define BITS_PER_SAMPLE 16    // 每个采样点 16 位
#define CHANNELS 1            // 单声道配置

static const char *TAG = "bsp_board";

// I2S 接收通道句柄，用于管理音频数据接收
static i2s_chan_handle_t rx_handle = nullptr;
// I2S 发送通道句柄，用于管理音频数据播放
static i2s_chan_handle_t tx_handle = nullptr;
// I2S 发送通道状态标志
static bool tx_channel_enabled = false;
// 音频播放互斥锁：避免多个任务同时操作同一个 I2S 播放通道
static SemaphoreHandle_t s_audio_mutex = nullptr;

// 保存音频配置参数，以便在紧急停止时重置 I2S 通道
static uint32_t s_sample_rate = 16000;
static int s_channel_format = 1;
static int s_bits_per_chan = 16;

static void ensure_audio_mutex(void)
{
    if (s_audio_mutex == nullptr) {
        s_audio_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

esp_err_t bsp_i2c_init(void)
{
    i2c_config_t i2c_conf = {};

    i2c_conf.mode = I2C_MODE_MASTER;
    i2c_conf.sda_io_num = BSP_I2C_SDA;
    i2c_conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.scl_io_num = BSP_I2C_SCL;
    i2c_conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.master.clk_speed = BSP_I2C_FREQ_HZ;

    i2c_param_config(BSP_I2C_NUM, &i2c_conf);
    return i2c_driver_install(BSP_I2C_NUM, i2c_conf.mode, 0, 0, 0);
}

static esp_err_t es8311_codec_init(void)
{
    /* 初始化I2C接口 */
    ESP_ERROR_CHECK(bsp_i2c_init());
    vTaskDelay(100 / portTICK_PERIOD_MS); // 等待I2C稳定

    /* 初始化es8311芯片 */
    es8311_handle_t es_handle = es8311_create(BSP_I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");

    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
        .sample_frequency = EXAMPLE_SAMPLE_RATE
    };

    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    vTaskDelay(50 / portTICK_PERIOD_MS); // 等待芯片初始化完成

    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");

    // 配置麦克风为模拟麦克风（false表示模拟麦克风）
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");

    // 设置麦克风增益为30dB（根据实际情况调整）
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(es_handle, ES8311_MIC_GAIN_24DB), TAG, "set es8311 microphone gain failed");
    ESP_LOGI(TAG, "ES8311 microphone gain set to 24dB");

    // 设置输出音量
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
    ESP_LOGI(TAG, "ES8311 voice volume set to %d", EXAMPLE_VOICE_VOLUME);

    return ESP_OK;
}

/**
 * @brief 🚀 初始化开发板硬件
 *
 * 这是整个音频系统的“启动按钮”，它会：
 * - 初始化INMP441麦克风
 * - 设置好所有GPIO引脚
 * - 准备好录音功能
 *
 * @param sample_rate 采样率（Hz），推荐16000
 * @param channel_format 声道格式，1=单声道
 * @param bits_per_chan 每个采样点的位数，推荐16
 * @return esp_err_t 初始化结果
 */

static esp_err_t i2s_driver_init(void)
{
    /* 配置i2s发送和接收通道 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    /* 初始化i2s为std模式 并打开i2s发送通道 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), 
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    tx_channel_enabled = true;
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    return ESP_OK;
}

esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    ESP_LOGI(TAG, "🚀 正在初始化ESP32-S3-DevKitC-1 + INMP441麦克风");
    ESP_LOGI(TAG, "🎵 音频参数: 采样率=%ldHz, 声道数=%d, 位深=%d位",
             sample_rate, channel_format, bits_per_chan);

    ESP_RETURN_ON_ERROR(es8311_codec_init(), TAG, "ES8311初始化失败");
    return i2s_driver_init();
}

/**
 * @brief 🎤 从麦克风获取音频数据
 *
 * 这个函数就像“录音师”，它会：
 * 
 * 🎯 工作流程：
 * 1. 从I2S接口读取原始数据
 * 2. 对INMP441的输出进行格式转换
 * 3. 可选择性应用增益调整
 * 4. 确保数据适合语音识别
 *
 * @param is_get_raw_channel 是否获取原始数据（true=不处理）
 * @param buffer 存储音频数据的缓冲区
 * @param buffer_len 缓冲区长度（字节）
 * @return esp_err_t 读取结果
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;

    // 🎤 从I2S通道读取音频数据
    ret = i2s_channel_read(rx_handle, buffer, buffer_len, &bytes_read, portMAX_DELAY);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 读取I2S数据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🔍 检查读取的数据长度是否符合预期
    if (bytes_read != buffer_len)
    {
        ESP_LOGW(TAG, "⚠️ 预期读取%d字节，实际读取%d字节", buffer_len, bytes_read);
    }

    // 🎯 INMP441特定的数据处理
    // INMP441输出24位数据在 32位帧中，左对齐
    // 我们需要提取最高有效的16位用于语音识别
    if (!is_get_raw_channel)
    {
        int samples = buffer_len / sizeof(int16_t);

        // 🎶 对INMP441的数据进行处理
        // 麦克风输出左对齐数据，进行信号电平调整
        for (int i = 0; i < samples; i++)
        {
            // 当前使用原始信号电平（无增益）
            // 测试表明原始电平已足够满足唤醒词检测需求
            int32_t sample = static_cast<int32_t>(buffer[i]);

            // 🔊 可选：应用2倍增益以提升信号强度（当前已禁用）
            // 如果发现声音太小，可以取消下面这行的注释
            // sample = sample * 2;

            // 📦 限制在16位有符号整数范围内
            if (sample > 32767)
            {
                sample = 32767;
            }
            if (sample < -32768)
            {
                sample = -32768;
            }

            buffer[i] = static_cast<int16_t>(sample);
        }
    }

    return ESP_OK;
}

void bsp_i2s_flush_rx(void)
{
    if (rx_handle != nullptr) {
        // 禁用再启用通道，可以清空DMA缓冲区中的旧数据
        i2s_channel_disable(rx_handle);
        i2s_channel_enable(rx_handle);
        ESP_LOGI(TAG, "已清空I2S接收缓冲区");
    }
}

/**
 * @brief 🎵 获取音频输入通道数
 *
 * 返回当前麦克风的声道数。
 * 我们使用单声道，节省资源且足够语音识别使用。
 *
 * @return int 通道数（1=单声道）
 */
int bsp_get_feed_channel(void)
{
    return CHANNELS;
}

/**
 * @brief 🔊 初始化I2S输出接口用于MAX98357A功放
 *
 * 这个函数专门为MAX98357A功放配置I2S通信：
 * 
 * 🔧 I2S配置特点：
 * - 使用Philips标准协议
 * - 支持单声道/立体声
 * - 16位数据宽度
 * - 3W输出功率
 *
 * @param sample_rate 采样率（Hz）
 * @param channel_format 声道数（1=单声道，2=立体声）
 * @param bits_per_chan 每个采样点的位数（16或32）
 * @return esp_err_t 初始化结果
 */
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    esp_err_t ret = ESP_OK;
    ensure_audio_mutex();

    s_sample_rate = sample_rate;
    s_channel_format = channel_format;
    s_bits_per_chan = bits_per_chan;

    // 🔌 初始化MAX98357A的SD引脚（控制功放开关）
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_OUT_SD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
    ESP_LOGI(TAG, "✅ MAX98357A SD引脚已初始化（GPIO%d）", I2S_OUT_SD_PIN);

    // 🔧 创建I2S发送通道配置
    // ESP32作为主机（Master），提供时钟信号给功放
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16; // 增大缓冲区数量到16 (原来8)
    chan_cfg.dma_frame_num = 1024; // 增大每个缓冲区到1024帧 (原来512)
    chan_cfg.auto_clear = true; 
    ret = i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 创建I2S发送通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🎯 确定数据位宽度
    i2s_data_bit_width_t bit_width = (bits_per_chan == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    // 🎶 配置I2S标准模式（专门为MAX98357A优化）
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, (channel_format == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // MCLK：MAX98357A不需要主时钟
            .bclk = I2S_OUT_BCLK_PIN,  // BCLK：位时钟→ GPIO15
            .ws = I2S_OUT_LRC_PIN,     // LRC：左右声道时钟→ GPIO16
            .dout = I2S_OUT_DIN_PIN,   // DIN：数据输出→ GPIO7
            .din = I2S_GPIO_UNUSED,    // DIN：不需要（只播放不录音）
            .invert_flags = {
                .mclk_inv = false,     // 不反转主时钟
                .bclk_inv = false,     // 不反转位时钟
                .ws_inv = false,       // 不反转字选择
            },
        },
    };

    // 🚀 初始化I2S标准模式
    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 初始化I2S发送标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // ▶️ 启用I2S发送通道开始播放数据
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 启用I2S发送通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🟢 设置通道状态标志
    tx_channel_enabled = true;

    ESP_LOGI(TAG, "✅ I2S音频播放初始化成功");
    return ESP_OK;
}

/**
 * @brief 通过 I2S 播放音频数据
 *
 * 这个函数将音频数据发送到 MAX98357A 功放进行播放：
 * 1. 将音频数据写入 I2S 发送通道
 * 2. 确保数据完全发送
 *
 * @param audio_data 指向音频数据的指针
 * @param data_len 音频数据长度（字节）
 * @return esp_err_t 播放结果
 */
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    size_t total_written = 0;

    ensure_audio_mutex();
    if (s_audio_mutex != nullptr) {
        xSemaphoreTakeRecursive(s_audio_mutex, portMAX_DELAY);
    }

    if (tx_handle == nullptr)
    {
        ESP_LOGE(TAG, "❌ I2S发送通道未初始化");
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_data == nullptr || data_len == 0)
    {
        ESP_LOGE(TAG, "❌ 无效的音频数据");
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_ERR_INVALID_ARG;
    }

    // TX通道始终保持enabled（ES8311全双工），直接标记为播放中
    tx_channel_enabled = true;

    // 循环写入音频数据，确保所有数据都被发送
    while (total_written < data_len)
    {
        size_t bytes_to_write = data_len - total_written;
        
        // 将音频数据写入 I2S 发送通道
        ret = i2s_channel_write(tx_handle,
                                  audio_data + total_written,
                                  bytes_to_write,
                                  &bytes_written,
                                  portMAX_DELAY);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 写入I2S音频数据失败: %s", esp_err_to_name(ret));
            break;
        }

        total_written += bytes_written;

        // 显示播放进度（每10KB显示一次）
        if ((total_written % 10240) < bytes_written)
        {
            ESP_LOGD(TAG, "音频播放进度: %zu/%zu 字节 (%.1f%%)", 
                     total_written, data_len, (float)total_written * 100.0f / data_len);
        }
    }

    if (total_written != data_len)
    {
        ESP_LOGW(TAG, "音频数据写入不完整: 预期 %zu 字节，实际写入 %zu 字节", data_len, total_written);
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_FAIL;
    }

    // 播放完成后停止I2S输出以防止噪音
    esp_err_t stop_ret = bsp_audio_stop();
    if (stop_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "停止音频输出时出现警告: %s", esp_err_to_name(stop_ret));
    }

    ESP_LOGI(TAG, "音频播放完成，播放了 %zu 字节", total_written);
    if (s_audio_mutex != nullptr) {
        xSemaphoreGiveRecursive(s_audio_mutex);
    }
    return ESP_OK;
}

/**
 * @brief 通过 I2S 播放音频数据（流式版本，不停止I2S）
 *
 * 这个函数与 bsp_play_audio 类似，但不会在播放完成后停止I2S，
 * 适用于连续播放多个音频块的流式场景。
 *
 * @param audio_data 指向音频数据的指针
 * @param data_len 音频数据长度（字节）
 * @return esp_err_t 播放结果
 */
esp_err_t bsp_play_audio_stream(const uint8_t *audio_data, size_t data_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    size_t total_written = 0;

    ensure_audio_mutex();
    if (s_audio_mutex != nullptr) {
        xSemaphoreTakeRecursive(s_audio_mutex, portMAX_DELAY);
    }

    if (tx_handle == nullptr)
    {
        ESP_LOGE(TAG, "❌ I2S发送通道未初始化");
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_data == nullptr || data_len == 0)
    {
        ESP_LOGE(TAG, "❌ 无效的音频数据");
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_ERR_INVALID_ARG;
    }

    // TX通道始终保持enabled（ES8311全双工），直接标记为播放中
    tx_channel_enabled = true;

    // 循环写入音频数据，确保所有数据都被发送
    // DMA缓冲区已增大到32KB，可以一次写入更多数据
    while (total_written < data_len)
    {
        size_t bytes_to_write = data_len - total_written;
        
        // 将音频数据写入 I2S 发送通道（使用合理超时）
        ret = i2s_channel_write(tx_handle, audio_data + total_written, bytes_to_write, 
                               &bytes_written, pdMS_TO_TICKS(200));

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 写入I2S音频数据失败: %s", esp_err_to_name(ret));
            break;
        }

        total_written += bytes_written;

        // 显示播放进度（每10KB显示一次）
        if ((total_written % 10240) < bytes_written)
        {
            ESP_LOGD(TAG, "音频播放进度: %zu/%zu 字节 (%.1f%%)", 
                     total_written, data_len, (float)total_written * 100.0f / data_len);
        }
    }

    if (total_written != data_len)
    {
        ESP_LOGW(TAG, "音频数据写入不完整: 预期 %zu 字节，实际写入 %zu 字节", data_len, total_written);
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_FAIL;
    }

    // 注意：这里不调用 bsp_audio_stop()，保持I2S继续运行
    ESP_LOGD(TAG, "流式音频块播放完成，播放了 %zu 字节", total_written);
    if (s_audio_mutex != nullptr) {
        xSemaphoreGiveRecursive(s_audio_mutex);
    }
    return ESP_OK;
}

/**
 * @brief 停止 I2S 音频输出以防止噪音
 *
 * 这个函数会暂时禁用 I2S 发送通道，停止向 MAX98357A 发送数据，
 * 从而消除播放完成后的噪音。当需要再次播放音频时，
 * 可以重新启用通道。
 *
 * @return esp_err_t 停止结果
 */
esp_err_t bsp_audio_stop(void)
{
    esp_err_t ret = ESP_OK;
    ensure_audio_mutex();
    if (s_audio_mutex != nullptr) {
        xSemaphoreTakeRecursive(s_audio_mutex, portMAX_DELAY);
    }

    if (tx_handle == nullptr)
    {
        ESP_LOGW(TAG, "⚠️ I2S发送通道未初始化，无需停止");
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_OK;
    }

    // ES8311全双工：TX通道始终保持enabled以维持codec时钟
    // auto_clear=true保证无数据时DMA自动输出静音，无需disable TX
    if (tx_channel_enabled)
    {
        tx_channel_enabled = false;
        ESP_LOGI(TAG, "✅ I2S音频输出已停止");
    }

    if (s_audio_mutex != nullptr) {
        xSemaphoreGiveRecursive(s_audio_mutex);
    }
    return ESP_OK;
}

/**
 * @brief 🚨 紧急停止音频输出
 *
 * 这个函数会立即禁用 I2S 发送通道并关闭功放，不等待DMA缓冲区播放完成。
 * 适用于需要立即打断播放的场景（如按键唤醒）。
 *
 * @return esp_err_t 停止结果
 */
esp_err_t bsp_audio_stop_immediate(void)
{
    esp_err_t ret = ESP_OK;
    ensure_audio_mutex();
    if (s_audio_mutex != nullptr) {
        xSemaphoreTakeRecursive(s_audio_mutex, portMAX_DELAY);
    }

    if (tx_handle == nullptr)
    {
        if (s_audio_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_audio_mutex);
        }
        return ESP_OK;
    }

    if (tx_channel_enabled)
    {
        // ES8311全双工：不能disable/del TX（会停止codec时钟导致RX死锁）
        // auto_clear=true保证DMA在无数据时自动输出静音
        tx_channel_enabled = false;
        ESP_LOGI(TAG, "🚨 I2S音频输出已紧急停止");
    }

    if (s_audio_mutex != nullptr) {
        xSemaphoreGiveRecursive(s_audio_mutex);
    }
    return ESP_OK;
}

