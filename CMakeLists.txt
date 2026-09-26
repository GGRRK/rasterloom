# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

project(Rasterloom
    VERSION 0.1.0
    DESCRIPTION "Native Linux raster image editor"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(RL_BUILD_GUI   "Build the Qt6 GUI (rasterloom)" ON)
option(RL_BUILD_TESTS "Build unit tests and CTest checks" ON)
option(RL_WERROR      "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RasterloomFlags)

find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)   # file I/O: JPEG import/export, PSD thumbnail (IRB 1036)
find_package(TIFF REQUIRED)   # file I/O: TIFF import/export
find_package(nlohmann_json 3 REQUIRED)

add_subdirectory(src)

if(RL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# install() rules: bin/, the ADS library, desktop/metainfo/icons, selftest scripts, share/doc/rasterloom.
include(RasterloomInstall)
