#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 配网模式下启动 BLE：可连接广播 + GATT 服务 FFE0（写 FFE1 下发
 * wifi_config JSON，FFE2 Notify 回复 wifi_config_ack）。设备名 SX_AIToy。
 */
void start_ble_advertising(void);

/**
 * @brief WiFi 扫描前暂停 BLE 广播，减少与 APSTA 扫描的射频冲突（配网页更不易掉线）。
 * 与 ble_resume_advertising_after_wifi_scan 成对调用；未开启 BT 或未在广播时为安全空操作。
 */
void ble_pause_for_wifi_scan(void);
void ble_resume_advertising_after_wifi_scan(void);

#ifdef __cplusplus
}
#endif
