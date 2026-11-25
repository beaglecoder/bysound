# Wraps the portable C library in ../../../c as an ESP-IDF component named "bysound".
set(BYS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../c)
file(GLOB BYS_SOURCES "${BYS_ROOT}/src/*.c")
idf_component_register(SRCS ${BYS_SOURCES}
                       INCLUDE_DIRS "${BYS_ROOT}/include"
                       PRIV_INCLUDE_DIRS "${BYS_ROOT}/src")
