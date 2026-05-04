set(requires
    driver
    esp_driver_i2s
    esp_timer
    nvs_flash
    lwip
    esp_wifi
    esp_netif
    esp_websocket_client
    esp_http_server
    mbedtls
    bt
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       audio_manager.cc
                       wifi_manager.cc
                       websocket_client.cc
                       keyWeakUp.cc
                       power_control.cc
                       config_server.cc
                       device_id_manager.cc
                       ble_adv.cc
                       INCLUDE_DIRS
                       "."
                       REQUIRES ${requires}
                       )

