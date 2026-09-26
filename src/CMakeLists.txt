# SPDX-License-Identifier: GPL-3.0-or-later
#
# Three targets from commit #1 (BUILD-SPEC requirement 1):
#   librasterloomcore.a  - all image math, no Qt at all
#   rasterloom-cli       - headless driver, no Qt
#   rasterloom           - Qt6 Widgets GUI, no image math
#
# Sources are globbed with CONFIGURE_DEPENDS so that a feature lane adding files under its own
# src/core/<domain>/ directory never has to edit this file.

# ---- core ---------------------------------------------------------------------------------------
file(GLOB_RECURSE RL_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp")
file(GLOB_RECURSE RL_CORE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.hpp")
# core/selftest/ is its own library (rasterloomselftest, below): it embeds the frozen golden corpus.
list(FILTER RL_CORE_SOURCES EXCLUDE REGEX "/core/selftest/")
list(FILTER RL_CORE_HEADERS EXCLUDE REGEX "/core/selftest/")

# LZ4 for warm/cold tile storage (core/tile/memory.cpp, BUILD-SPEC req 2).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)

add_library(rasterloomcore STATIC ${RL_CORE_SOURCES} ${RL_CORE_HEADERS})
set_target_properties(rasterloomcore PROPERTIES OUTPUT_NAME rasterloomcore)
target_include_directories(rasterloomcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rasterloomcore PUBLIC PNG::PNG ZLIB::ZLIB JPEG::JPEG TIFF::TIFF nlohmann_json::nlohmann_json)
target_link_libraries(rasterloomcore PRIVATE PkgConfig::LZ4)
target_compile_definitions(rasterloomcore PUBLIC RL_VERSION_STRING="${PROJECT_VERSION}")
rl_warnings(rasterloomcore)

# ---- embedded self-test (core side, no Qt) -------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/core/selftest/selftest.cmake")

# ---- headless CLI -------------------------------------------------------------------------------
file(GLOB RL_CLI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp")
add_executable(rasterloom-cli ${RL_CLI_SOURCES})
target_link_libraries(rasterloom-cli PRIVATE rasterloomcore rasterloomselftest)
rl_warnings(rasterloom-cli)

# ---- GUI ----------------------------------------------------------------------------------------
if(RL_BUILD_GUI)
    # Floor is Qt 6.9 (BUILD-SPEC addendum): no Qt API newer than 6.9 may be used.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)   # GuiPrivate is used on purpose (tablet/platform details)
    find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg GuiPrivate)

    # Qt-Advanced-Docking-System, pinned to v5.1.1 and linked shared (docs/research/qt-platform.md
    # section 3). BUILD_EXAMPLES / BUILD_STATIC are ADS's own (un-prefixed) option names. For an
    # offline build, pass -DFETCHCONTENT_SOURCE_DIR_QTADS=/path/to/Qt-Advanced-Docking-System-5.1.1.
    set(QT_VERSION_MAJOR 6)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC   OFF CACHE BOOL "" FORCE)
    set(RL_ADS_VERSION 5.1.1)
    include(FetchContent)
    # DOWNLOAD_EXTRACT_TIMESTAMP exists from CMake 3.24; CI's ubuntu:22.04 has 3.22.
    set(RL_ADS_FETCH_OPTS)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        list(APPEND RL_ADS_FETCH_OPTS DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    FetchContent_Declare(qtads
        URL https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/archive/refs/tags/v${RL_ADS_VERSION}.tar.gz
        URL_HASH SHA256=9ce947acbb1984946f9145c976e42e6984f99b98fb414dd7354a290af6d4ff04
        ${RL_ADS_FETCH_OPTS})
    FetchContent_MakeAvailable(qtads)

    # All GUI code except main() goes into a static library so tests/gui links the real widgets.
    file(GLOB RL_GUI_LIB_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/gui/*.hpp")
    list(REMOVE_ITEM RL_GUI_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gui/main.cpp")
    add_library(rasterloomgui STATIC ${RL_GUI_LIB_SOURCES})
    set_target_properties(rasterloomgui PROPERTIES AUTOMOC ON)
    target_link_libraries(rasterloomgui PUBLIC
        rasterloomcore Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg
        ads::qtadvanceddocking-qt6)
    # liblz4 for --version --verbose / --third-party-notices (the core links it privately).
    target_link_libraries(rasterloomgui PRIVATE PkgConfig::LZ4)
    target_compile_definitions(rasterloomgui PUBLIC RL_ADS_VERSION="${RL_ADS_VERSION}")
    rl_warnings(rasterloomgui)

    add_executable(rasterloom "${CMAKE_CURRENT_SOURCE_DIR}/gui/main.cpp")
    set_target_properties(rasterloom PROPERTIES AUTOMOC ON)
    target_link_libraries(rasterloom PRIVATE rasterloomgui rasterloomselftest)
    rl_warnings(rasterloom)
endif()
