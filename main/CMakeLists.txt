set(esp_sdr_supported_targets esp32 esp32c3 esp32c5 esp32c6 esp32c61 esp32s2 esp32s3 esp32s31)
if(NOT IDF_TARGET IN_LIST esp_sdr_supported_targets)
    message(FATAL_ERROR "No ESP-SDR RF backend for ${IDF_TARGET}. Supported targets: ${esp_sdr_supported_targets}")
endif()

option(SAMPLE_RATE_PROBE "Build volatile hardware sample-rate diagnostics (never package)" OFF)

set(target_dir "targets/${IDF_TARGET}")
set(receiver "${target_dir}/receiver.c")
set(sources "common/burst_serial.c" "common/spectrum_stats.c")
set(dependencies esp_phy esp_wifi nvs_flash esp_timer esp_driver_uart esp-dsp)
set(private_dependencies)
option(RING_PROBE "Build bounded experimental ring diagnostics (never package)" OFF)
if(IDF_TARGET STREQUAL "esp32c61" OR IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32c3")
    list(APPEND sources "common/ring_capture.c")
endif()
set(probes RING_PROBE)
set(fragments)
if(NOT IDF_TARGET STREQUAL "esp32s3")
    list(APPEND sources "common/spectrum.c")
endif()

if(IDF_TARGET STREQUAL "esp32s2")
    list(APPEND dependencies esp_usb_cdc_rom_console)
    list(APPEND probes S2_RF_PROBE)
elseif(NOT IDF_TARGET STREQUAL "esp32")
    list(APPEND dependencies esp_driver_usb_serial_jtag)
endif()

if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND sources "common/ring_capture.c" "${target_dir}/s3_fft_rnd.S" "${target_dir}/s3_unpack.S" "${target_dir}/s3_core1.S")
    # SPEC-mode FFT; pinned submodule, the component manager stays disabled.
    list(APPEND dependencies esp-dsp)
    # Component requirements are collected before CONFIG_* options are
    # available, so declare these for all S3 profiles even when the UI source
    # is excluded. They have no effect unless referenced by the app.
    list(APPEND private_dependencies esp_driver_gpio esp_driver_spi esp_driver_pcnt
                                    esp_driver_i2c esp_driver_i2s)
    list(APPEND fragments "${target_dir}/linker.lf")
    list(APPEND probes SAMPLE_RATE_PROBE FILTER_REGISTER_PROBE S3_RF_PROBE)
    if(ESP_SDR_T_EMBED_APPS OR "$ENV{ESP_SDR_T_EMBED_APPS}")
        list(APPEND dependencies hermes_core)
    endif()
    if(CONFIG_ESP_SDR_T_EMBED_UI)
        list(APPEND sources "${target_dir}/t_embed_ui.c"
                            "${target_dir}/t_embed_gesture.c")
    endif()
    if(ESP_SDR_T_EMBED_APPS)
        if(NOT CONFIG_ESP_SDR_T_EMBED_UI)
            message(FATAL_ERROR "T-Embed apps require the T-Embed display UI")
        endif()
        list(APPEND sources "${target_dir}/t_embed_launcher.c"
                            "${target_dir}/t_embed_microphone.c"
                            "${target_dir}/t_embed_audio_input.c"
                            "${target_dir}/t_embed_hermes.cpp"
                            "${target_dir}/t_embed_hermes_audio.cpp"
                            "${target_dir}/t_embed_hermes_transport.cpp")
        list(APPEND private_dependencies esp_codec_dev esp_websocket_client heap
                                        mbedtls esp_event esp_netif esp_hw_support esp_system)
    endif()
elseif(IDF_TARGET STREQUAL "esp32c5" OR IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32c61")
    set(receiver "families/c5_c6_c61/receiver.c")
    if(IDF_TARGET STREQUAL "esp32c6" AND FILTER_REGISTER_PROBE)
        message(FATAL_ERROR "Filter register diagnostics are not characterized for C6")
    endif()
    list(APPEND probes SAMPLE_RATE_PROBE FILTER_REGISTER_PROBE C5_TUNE_PROBE)
endif()

if(CONFIG_ESP_SDR_T_EMBED_UI AND NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "The T-Embed display UI requires the ESP32-S3 target")
endif()

if(ESP_SDR_T_EMBED_APPS AND NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "T-Embed apps require the ESP32-S3 target")
endif()

if(IDF_TARGET STREQUAL "esp32s31")
    list(APPEND sources "${target_dir}/s31_fft_rnd.S" "${target_dir}/s31_unpack.S")
    list(APPEND fragments "${target_dir}/linker.lf")
endif()

if(IDF_TARGET STREQUAL "esp32c61" OR IDF_TARGET STREQUAL "esp32s31")
    list(APPEND sources "common/burst_gain_table.c")
endif()

idf_component_register(SRCS "${receiver}" ${sources}
    LDFRAGMENTS ${fragments}
    PRIV_INCLUDE_DIRS "common" "${target_dir}" "diagnostics"
    REQUIRES ${dependencies}
    PRIV_REQUIRES ${private_dependencies})

if(ESP_SDR_T_EMBED_APPS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP_SDR_T_EMBED_APPS=1)
endif()

# Keep the linker guard beside the backend whose SRAM aperture it protects.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/${target_dir}/sram_guard.ld")
if(IDF_TARGET STREQUAL "esp32c61" OR IDF_TARGET STREQUAL "esp32s31")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=phy_write_gain_mem")
endif()
foreach(probe IN LISTS probes)
    if(${probe})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${probe}=1")
    endif()
endforeach()
