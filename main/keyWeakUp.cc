#include "keyWeakUp.h"
#include "esp_err.h"
#include "esp_system.h"  
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h" // 流缓冲区
#include "freertos/semphr.h"
#include "esp_log.h"    
#include "driver/gpio.h"

#define KEY_PIN         GPIO_NUM_9        // 按键引脚（GPIO0）
static volatile bool s_flag = false;
int key_flag; // 长按1.5秒标志
int key_short_press; // 短按标志
int key_triple_press; // 连续三击标志
int key_allow_triple_press; // 仅待机态允许连续三击进入配网
int key_triple_press_active; // 连续三击判定窗口进行中

// 定义信号量句柄
SemaphoreHandle_t key_semaphore;

static const char *TAG = "KEY_WAKEUP";

void keyweakup_init(gpio_num_t pin)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << KEY_PIN;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE; 
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "按键GPIO%d初始化完成", pin);
}


//创建一个按键电平检测task
void keyweakup_task(void *pvParameters)
{
    int level=0;
    int click_count = 0;
    TickType_t last_click_tick = 0;
    const TickType_t triple_click_window = pdMS_TO_TICKS(1500);

    while (1) 
    {
        if (key_allow_triple_press && click_count > 0) {
            key_triple_press_active = 1;
            TickType_t now = xTaskGetTickCount();
            if ((now - last_click_tick) > triple_click_window) {
                click_count = 0;
                last_click_tick = 0;
                key_triple_press_active = 0;
            }
        } else if (!key_allow_triple_press) {
            click_count = 0;
            last_click_tick = 0;
            key_triple_press_active = 0;
        } else {
            key_triple_press_active = 0;
        }

        level = gpio_get_level(KEY_PIN);
        if (level == 1) 
        {
            level=0;
            // 按下处理（在任务中做去抖）
            vTaskDelay(pdMS_TO_TICKS(20)); // 简单去抖

            if (gpio_get_level(KEY_PIN) == 1)
            {
                // 开始计时，检测长按
                TickType_t start_tick = xTaskGetTickCount();
                int press_duration = 0;
                
                // 等待按键释放，同时计算按下时间
                while (gpio_get_level(KEY_PIN) == 1) 
                {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    press_duration = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
                    
                    // 检测是否达到3秒长按
                    if (press_duration >= 1500) {
                        ESP_LOGI(TAG, "检测到长按1.5秒，触发唤醒功能");
                        key_flag=1;
                        // 等待按键释放，避免重复触发
                        while (gpio_get_level(KEY_PIN) == 1) {
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        break;
                    }
                }
                
                // 如果按下时间不足1.5秒，触发短按
                if (press_duration < 1500 && press_duration > 100) { // 排除误触
                    if (key_allow_triple_press) {
                        TickType_t now = xTaskGetTickCount();
                        if (click_count == 0 || (now - last_click_tick) > triple_click_window) {
                            click_count = 1;
                        } else {
                            click_count++;
                        }
                        last_click_tick = now;

                        if (click_count >= 3) {
                            ESP_LOGI(TAG, "检测到连续三击，触发进入配网模式");
                            key_triple_press = 1;
                            click_count = 0;
                            last_click_tick = 0;
                            key_triple_press_active = 0;
                        }
                    } else {
                        ESP_LOGI(TAG, "检测到短按，触发中断对话功能");
                        key_short_press=1;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

