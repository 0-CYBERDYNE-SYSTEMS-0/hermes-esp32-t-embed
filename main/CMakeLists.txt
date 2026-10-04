if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Hermes T-Embed requires the ESP32-S3 target")
endif()

set(target_dir "targets/esp32s3")
idf_component_register(
    SRCS "${target_dir}/t_embed_launcher.c"
         "${target_dir}/t_embed_ui.c"
         "${target_dir}/t_embed_gesture.c"
         "${target_dir}/t_embed_audio_input.c"
         "${target_dir}/t_embed_hermes.cpp"
         "${target_dir}/t_embed_hermes_audio.cpp"
         "${target_dir}/t_embed_hermes_transport.cpp"
    PRIV_INCLUDE_DIRS "${target_dir}"
    REQUIRES hermes_core esp_wifi nvs_flash esp_timer
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_pcnt
                  esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag
                  esp_codec_dev esp_websocket_client heap mbedtls
                  esp_event esp_netif esp_hw_support esp_system)
