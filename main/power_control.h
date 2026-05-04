#ifndef __POWER_CONTROL_H__
#define __POWER_CONTROL_H__

#include "driver/gpio.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif


//初始化电源控制
void Power_Init(void);
//打开电源
void Power_Open(void);
//关闭电源
void Power_Close(void);
//打开mic电源
void Power_Open_Mic(void);
//关闭mic电源
void Power_Close_Mic(void);



#ifdef __cplusplus
}
#endif

#endif
