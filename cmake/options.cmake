# Brickdesk build options.
#
# This file is included from the top-level CMakeLists.txt. It must stay
# toolchain-agnostic: cross builds inject their compiler and sysroot through a
# CMake toolchain file, not through anything here.

option(BRICKDESK_WERROR "Treat compiler warnings as errors" OFF)

set(BRICKDESK_BACKEND "host" CACHE STRING "Platform backend to build (host or target)")
set_property(CACHE BRICKDESK_BACKEND PROPERTY STRINGS host target)

if(NOT BRICKDESK_BACKEND STREQUAL "host" AND NOT BRICKDESK_BACKEND STREQUAL "target")
  message(FATAL_ERROR "BRICKDESK_BACKEND must be 'host' or 'target', got '${BRICKDESK_BACKEND}'")
endif()

option(BRICKDESK_BUILD_HARNESS "Build the screenshot/input replay harness" ON)
option(BRICKDESK_BUILD_BENCH "Build the renderer benchmark" ON)

if(BRICKDESK_BACKEND STREQUAL "target")
  set(_bd_default_asset_dir "/mnt/SDCARD/brickdesk/assets")
else()
  set(_bd_default_asset_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()
set(BRICKDESK_ASSET_DIR "${_bd_default_asset_dir}" CACHE PATH
    "Directory containing fonts and demo assets at runtime")
