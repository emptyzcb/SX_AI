/**
 * @file bsp_board.h
 * @brief 🎮 BSP（Board Support Package）板级支持包 - 硬件抽象层
 * 
 * 这个文件就像“硬件说明书”，定义了操作音频硬件的各种函数。
 * 
 * 🎯 支持的硬件：
 * - 🎤 INMP441数字麦克风（高清录音）
 * - 🔊 MAX98357A数字功放（清晰播放）
 * - 📟️ ESP32-S3开发板（主控芯片）
 * 
 * 🔌 主要功能：
 * 1. 初始化音频输入输出
 * 2. 从麦克风读取声音
 * 3. 通过扬声器播放声音
 * 4. 管理I2S总线通信
 * 
 * @copyright Copyright 2021 Espressif Systems (Shanghai) Co. Ltd.
 *      Licensed under the Apache License, Version 2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 🚀 初始化开发板音频硬件
 *
 * 这是使用音频功能前必须调用的函数，它会：
 * 1. 初始化INMP441麦克风
 * 2. 设置好I2S通信协议
 * 3. 准备好所有音频相关的GPIO引脚
 *
 * @param sample_rate 采样率（推荐16000Hz，适合语音识别）
 * @param channel_format 声道数（1=单声道，2=立体声）
 * @param bits_per_chan 采样位数（16位=CD音质，32位=更高精度）
 * @return
 *    - ESP_OK: ✅ 初始化成功
 *    - 其他值: ❌ 初始化失败
 */
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan);

/**
 * @brief 🎤 从麦克风获取声音数据
 *
 * 这个函数就像“录音”，让您能够：
 * - 读取麦克风捕捉到的声音
 * - 获得可以用于语音识别的数据
 * - 按照需要的长度读取数据
 *
 * @param is_get_raw_channel 是否获取原始数据（true=不处理，false=经过优化）
 * @param buffer 存储音频数据的数组（您提供的“录音带”）
 * @param buffer_len 数组大小（字节数）
 * @return
 *    - ESP_OK: ✅ 读取成功
 *    - 其他值: ❌ 读取失败
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);

/**
 * @brief 清空I2S接收缓冲区
 * 
 * 用于在播放提示音后，清空由于没有及时读取而积压在DMA缓冲区中的旧音频（包括提示音本身的录音），
 * 避免这些旧音频被当作用户的语音发送给服务器，或者导致真正的用户语音被丢弃。
 */
void bsp_i2s_flush_rx(void);

/**
 * @brief 🎵 获取音频输入的声道数
 *
 * 告诉您麦克风是单声道还是立体声。
 * 目前我们使用单声道，节省内存和处理时间。
 *
 * @return 声道数（1=单声道，2=立体声）
 */
int bsp_get_feed_channel(void);

/**
 * @brief 🔊 初始化音频播放功能
 *
 * 这个函数专门为播放音频做准备，它会：
 * 1. 初始化MAX98357A功放
 * 2. 设置好音频输出通道
 * 3. 准备好扬声器接口
 *
 * @param sample_rate 采样率（推荐16000Hz，与录音保持一致）
 * @param channel_format 声道数（1=单声道，2=立体声）
 * @param bits_per_chan 采样位数（16位=标准音质）
 * @return
 *    - ESP_OK: ✅ 初始化成功
 *    - 其他值: ❌ 初始化失败
 */
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan);

/**
 * @brief 🎵 播放音频数据
 *
 * 这个函数就像“播放器”，它会：
 * - 把您提供的音频数据发送给扬声器
 * - 播放完后自动停止（避免噪音）
 * - 适合播放完整的音频文件
 *
 * @param audio_data 音频数据的内存地址（PCM格式）
 * @param data_len 音频数据的大小（字节数）
 * @return
 *    - ESP_OK: ✅ 播放成功
 *    - 其他值: ❌ 播放失败
 */
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len);

/**
 * @brief 🌊 流式播放音频数据
 *
 * 这个函数特别为“边下载边播放”设计：
 * - 播放完后不会停止I2S
 * - 可以连续播放多个音频片段
 * - 适合实时流媒体场景
 *
 * @param audio_data 音频数据的内存地址（PCM格式）
 * @param data_len 音频数据的大小（字节数）
 * @return
 *    - ESP_OK: ✅ 播放成功
 *    - 其他值: ❌ 播放失败
 */
esp_err_t bsp_play_audio_stream(const uint8_t *audio_data, size_t data_len);

/**
 * @brief 🛑️ 停止音频输出
 *
 * 这个函数可以：
 * - 立即停止音频播放
 * - 消除扬声器的噪音
 * - 关闭功放节省电量
 *
 * @return
 *    - ESP_OK: ✅ 停止成功
 *    - 其他值: ❌ 停止失败
 */
esp_err_t bsp_audio_stop(void);
esp_err_t bsp_audio_stop_immediate(void);
esp_err_t bsp_i2c_init(void);

#ifdef __cplusplus
}
#endif
