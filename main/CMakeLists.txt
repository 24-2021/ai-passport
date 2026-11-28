idf_component_register(
    SRCS "main.c"
         "kfb24_proto.c"
         "kfb24_config.c"
         "kfb24_ble.c"
         "kfb24_link.c"
         "kfb24_audio.c"
         "kfb24_wake.c"
         "kfb24_adpcm.c"
         "kfb24_power.c"
         "kfb24_ui.c"
         "fonts/kfb24_14.c"
         "fonts/kfb24_16.c"
         "fonts/kfb24_22.c"
         "fonts/kfb24_reply_16.c"
    INCLUDE_DIRS "."
    REQUIRES bt bsp esp_driver_usb_serial_jtag esp_event esp_netif esp_timer esp_wifi lwip nvs_flash esp-sr
)
