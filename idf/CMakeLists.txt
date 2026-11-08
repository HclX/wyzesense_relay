# The ESP32-S3 relay firmware (issue #22): build with tools/idf.sh build (the pinned ESP-IDF, tools/toolchain.lock).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ows_relay)
