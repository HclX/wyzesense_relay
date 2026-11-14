# The ESP32-S3 relay firmware (issue #22): build with tools/idf.sh build (the pinned ESP-IDF, tools/toolchain.lock).
cmake_minimum_required(VERSION 3.16)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/version.py"
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(PROJECT_VER STREQUAL "")
    set(PROJECT_VER "unknown")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ows_relay)
