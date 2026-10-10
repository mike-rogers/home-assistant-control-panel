set(srcs "main.c"
         "ui_task.c"
         "input.c"
         "ano_seesaw.c"
         "panel_mqtt.c"
         "menu_cache.c"
         "device_cfg.c"
         "wifi_conn.c"
         "improv_ble.c"
         "adopt_server.c"
         "ota_update.c")

if(CONFIG_KPANEL_RENDER_LOG)
    list(APPEND srcs "render_log.c")
else()
    list(APPEND srcs "render_lcd.c" "backlight.c")
endif()

idf_component_register(SRCS ${srcs}
                       PRIV_REQUIRES panel_core
                                     esp_wifi
                                     esp_timer
                                     nvs_flash
                                     esp_http_server
                                     bt
                                     esp_https_ota
                                     app_update
                                     esp_lcd
                                     esp_driver_spi
                                     esp_driver_gpio
                                     esp_driver_i2c
                                     esp_driver_ledc
                                     esp_netif
                                     lwip
                       INCLUDE_DIRS ".")
