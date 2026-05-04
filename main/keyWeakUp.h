#ifndef KEYWEAKUP_H
#define KEYWEAKUP_H
#ifdef __cplusplus

extern "C" {
#endif
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"

// 外部声明信号量句柄（全局共享）
extern SemaphoreHandle_t key_semaphore;
extern int key_flag; // 长按1.5秒标志
extern int key_short_press; // 短按标志
extern int key_triple_press; // 连续三击标志
extern int key_allow_triple_press; // 是否允许连续三击进入配网
extern int key_triple_press_active; // 连续三击判定窗口进行中

void keyweakup_init(gpio_num_t pin);
void keyweakup_task(void *pvParameters);
#ifdef __cplusplus
}

#endif
#endif

