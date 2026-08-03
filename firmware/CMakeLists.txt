# SPDX-FileCopyrightText: 2025 Outdoor Camera Project
# SPDX-License-Identifier: Apache-2.0
#
# 顶层 CMakeLists.txt
# 目标芯片: ESP32-S3
# 框架: ESP-IDF v5.1 及以上
#
# 户外长期独立运行定时拍照固件

# ESP-IDF 构建系统要求 CMake >= 3.16
cmake_minimum_required(VERSION 3.16)

# 校验 ESP-IDF 版本不低于 5.1
if(NOT DEFINED IDF_VERSION_MAJOR OR NOT DEFINED IDF_VERSION_MINOR)
    message(FATAL_ERROR "未检测到 ESP-IDF 构建系统，请使用 idf.py 构建。")
endif()
if(IDF_VERSION_MAJOR LESS 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR LESS 1))
    message(FATAL_ERROR "本工程要求 ESP-IDF v5.1 或更高版本，当前为 v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
endif()

# 引入 ESP-IDF 构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 额外组件目录：components/ 下的所有子目录均作为独立组件
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

project(outdoor-camera-firmware)
