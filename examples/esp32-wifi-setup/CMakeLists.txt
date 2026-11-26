cmake_minimum_required(VERSION 3.16)
# Reuses the bysound component (esp32/components/bysound) and the board audio code of the esp32 demo (esp32/main/audio.c) by relative path.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../esp32/components)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../esp32/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bysound_wifi_setup)
