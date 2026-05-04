/**
 * @file power_control.cc
 * @brief ESP32-S3 电源管理控制实现
 *
 * 功能说明：
 * 1. MCU_PWR_EN (IO7) - 电源保持控制，拉高后芯片自锁电源
 * 2. PWR_KEY_CHECK (IO16) - 电源按键检测，支持长按开机/关机
 * 3. EN_PWR_MIC (IO15) - 麦克风功放电源开关
 *
 * 技术实现：
 * - 使用软件扫描方式检测按键高电平持续时长
 * - 连续高电平达到 2 秒后，切换开关机状态
 * - 完全独立，不依赖项目其他模块
 */
#include "power_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"

static const char *TAG = "PowerControl";

#define POWER_IO_MCU_PWR_EN   GPIO_NUM_7
#define POWER_IO_PWR_KEY_CHECK GPIO_NUM_16
#define POWER_IO_EN_PWR_MIC   GPIO_NUM_15
#define POWER_LONG_PRESS_MS 2000
#define POWER_KEY_SCAN_PERIOD_MS 20

// 按键扫描任务句柄
static TaskHandle_t s_power_key_task_handle = NULL;

// 电源状态标志位：true=开机，false=关机
static bool s_power_on_flag = false;

// 统一设置开关机状态：
// - 开机：EN_PWR_MIC=1, MCU_PWR_EN=1
// - 关机：EN_PWR_MIC=0, MCU_PWR_EN=0
static void set_power_state(bool power_on)
{

    gpio_set_level(POWER_IO_MCU_PWR_EN, power_on ? 1 : 0);
    s_power_on_flag = power_on;
    ESP_LOGI(TAG, "Power state -> %s", power_on ? "ON" : "OFF");
}

static void power_key_task(void *arg)
{
    (void)arg;
    bool key_high_prev = false;
    bool long_press_handled = false;
    TickType_t press_start_tick = 0;

    while (1) {
        // 需求：POWER_IO_PWR_KEY_CHECK 连续保持 2 秒高电平，才触发一次开/关机
        bool key_is_high = (gpio_get_level(POWER_IO_PWR_KEY_CHECK) == 1);
        if (key_is_high)
        {
            if (!key_high_prev)
            {
                key_high_prev = true;
                long_press_handled = false;
                press_start_tick = xTaskGetTickCount();
            }
            else if (!long_press_handled)
            {
                TickType_t elapsed = xTaskGetTickCount() - press_start_tick;
                if (elapsed >= pdMS_TO_TICKS(POWER_LONG_PRESS_MS))
                {
                    // 使用状态标志位进行开关机切换，不依赖 GPIO 回读取反
                    if (s_power_on_flag) {
                        set_power_state(false);
                    } else {
                        set_power_state(true);
                    }
                    ESP_LOGI(TAG, "Power key high >= 2s, toggle power");
                    long_press_handled = true;
                }
            }
        }
        else
        {
            // 松手后允许下一次长按再次触发
            key_high_prev = false;
            long_press_handled = false;
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_KEY_SCAN_PERIOD_MS));
    }
}

void Power_Init(void)
{
// MCU_PWR_EN (IO7)
// 电源保持控制脚：上电后默认拉低，避免误自锁
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << POWER_IO_MCU_PWR_EN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(POWER_IO_MCU_PWR_EN, 0);

// EN_PWR_MIC (IO15)
// 麦克风功放电源控制脚：默认关闭
    gpio_config_t io_conf2;
    io_conf2.pin_bit_mask = (1ULL << POWER_IO_EN_PWR_MIC);
    io_conf2.mode = GPIO_MODE_OUTPUT;
    io_conf2.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf2.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf2.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf2);
    gpio_set_level(POWER_IO_EN_PWR_MIC, 0);

    // 初始化时状态为关机
    s_power_on_flag = false;

// PWR_KEY_CHECK (IO16)
// 电源按键输入脚：按需求“高电平有效”，使用下拉，软件扫描高电平持续时间
    gpio_config_t io_conf3;
    io_conf3.pin_bit_mask = (1ULL << POWER_IO_PWR_KEY_CHECK);
    io_conf3.mode = GPIO_MODE_INPUT;
    io_conf3.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf3.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf3.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf3);

    // 创建按键扫描任务：软件模拟“长按 2 秒开/关机”
    if (s_power_key_task_handle == NULL) {
        BaseType_t task_ok = xTaskCreate(power_key_task, "power_key_task", 2048, NULL, 10, &s_power_key_task_handle);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "create power_key_task failed");
            return;
        }
    }

    ESP_LOGI(TAG, "PWR_KEY_CHECK configured: high-level long-press 2s to toggle power");
}

// 打开电源
void Power_Open(void)
{
    // 开机时同时使能麦克风供电与主电源保持
    set_power_state(true);
}

// 关闭电源
void Power_Close(void)
{
    // 关机时同时关闭麦克风供电与主电源保持
    set_power_state(false);
}

// 打开 mic 电源
void Power_Open_Mic(void)
{
    // 打开麦克风功放电源
    gpio_set_level(POWER_IO_EN_PWR_MIC, 1);
}

// 关闭 mic 电源
void Power_Close_Mic(void)
{
    // 关闭麦克风功放电源
    gpio_set_level(POWER_IO_EN_PWR_MIC, 0);
}
