# CMakeLists.txt - kořenový soubor projektu
# Matter over Thread - ovládání jednobarevného LED pásku na ESP32-C6

cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

# Cesta k esp-matter komponentám.
# Pokud je nastavena promenna prostredi ESP_MATTER_PATH (napr. uvnitr
# oficialniho Docker image espressif/esp-matter), pouzije se ta.
# Jinak se predpoklada, ze projekt lezi 3 urovne uvnitr esp-matter/examples/.
if(DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
else()
    get_filename_component(ESP_MATTER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/components/esp_matter"
    "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/config/esp32/components"
    "${ESP_MATTER_PATH}/device_hal/device"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter_led_c6)
