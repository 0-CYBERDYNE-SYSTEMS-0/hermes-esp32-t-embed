cmake_minimum_required(VERSION 3.16)

# S2 uses the ROM USB-OTG CDC driver; keep its pinned compatibility fix
# scoped to this target so other chips retain their SDK components.
if(IDF_TARGET STREQUAL "esp32s2")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/platform/esp32s2/esp_usb_cdc_rom_console")
endif()

if(ESP_SDR_T_EMBED_APPS)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/platform/hermes_core")
    set(ENV{ESP_SDR_T_EMBED_APPS} "1")
else()
    unset(ENV{ESP_SDR_T_EMBED_APPS})
endif()

if(NOT ESP_SDR_T_EMBED_APPS)
    set(ENV{IDF_COMPONENT_MANAGER} "0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_sdr)
