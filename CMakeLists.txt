cmake_minimum_required(VERSION 3.0)

project(wdd VERSION 0.2.0)

add_executable(wdd src/wdd.c)

install(TARGETS wdd RUNTIME DESTINATION .)

string(TOLOWER ${CMAKE_GENERATOR_PLATFORM} _arch)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME
    ${PROJECT_NAME}-${PROJECT_VERSION}-${_arch})

include(CPack)
