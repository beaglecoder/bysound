# ESP-IDF component (ESP32-S3, C3, C6) and, outside ESP-IDF, a static library for the host.
# In ESP-IDF the demodulator options (engine, robust, audible, max rate) come from the Kconfig in this directory.
file(GLOB BYS_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
if(ESP_PLATFORM)
  idf_component_register(SRCS ${BYS_SOURCES} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src")
else()
  cmake_minimum_required(VERSION 3.10)
  project(bysound C)
  add_library(bysound STATIC ${BYS_SOURCES})
  target_include_directories(bysound PUBLIC include PRIVATE src)
  target_compile_features(bysound PUBLIC c_std_99)
  target_link_libraries(bysound PUBLIC m)
endif()
