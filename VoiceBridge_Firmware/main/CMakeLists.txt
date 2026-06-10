idf_component_register(
    SRCS
        "main.c"
        "led.c"
        "gesture.c"
        "audio.c"
        "ws_client.c"
        "protocol.c"
        "recorder.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        esp_system
        esp_websocket_client
        button
        es8311
        spiffs
        mbedtls
        esp-tls
        freertos
        log
        json
        esp_task_wdt
)

# Автоматически заливать папку data/ (TTS wav файлы) в SPIFFS при idf.py flash
# Папка data/ должна находиться в корне проекта (рядом с main/)
spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)
