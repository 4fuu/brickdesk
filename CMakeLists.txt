cmake_minimum_required(VERSION 3.16)

project(brickdesk VERSION 0.1.0 LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(options)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(BRICKDESK_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
  "${BRICKDESK_GENERATED_INCLUDE_DIR}/brickdesk/version.h"
  @ONLY
)

add_library(brickdesk_warnings INTERFACE)
if(MSVC)
  target_compile_options(brickdesk_warnings INTERFACE /W4)
  if(BRICKDESK_WERROR)
    target_compile_options(brickdesk_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(brickdesk_warnings INTERFACE -Wall -Wextra)
  if(BRICKDESK_WERROR)
    target_compile_options(brickdesk_warnings INTERFACE -Werror)
  endif()
endif()

# --- SDL2 ---------------------------------------------------------------
# Prefer the SDL2 CMake package (consumed by cross builds and distro SDKs),
# fall back to pkg-config on plain host systems.
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
  set(BRICKDESK_SDL2_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
  set(BRICKDESK_SDL2_TARGET SDL2::SDL2-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(BRICKDESK_SDL2_TARGET PkgConfig::SDL2)
endif()

# --- GLES2 --------------------------------------------------------------
# Only GLES2 is available on the PowerVR GE8300. EGL is owned by SDL.
find_path(BRICKDESK_GLES2_INCLUDE_DIR NAMES GLES2/gl2.h)
find_library(BRICKDESK_GLES2_LIBRARY NAMES GLESv2)
if(NOT BRICKDESK_GLES2_INCLUDE_DIR OR NOT BRICKDESK_GLES2_LIBRARY)
  message(FATAL_ERROR "GLES2 headers or library not found (install libgles2-mesa-dev)")
endif()

# --- third-party --------------------------------------------------------
add_library(bd_stb STATIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_image_write_impl.c")
target_include_directories(bd_stb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
if(NOT MSVC)
  # Vendored code is not ours to keep warning-clean.
  target_compile_options(bd_stb PRIVATE -w)
endif()

# --- core library -------------------------------------------------------
add_library(brickdesk_core STATIC
  src/app/desktop.c
  src/gfx/capture.c
  src/gfx/renderer.c
  src/input/keys.c
  src/platform/host_sdl.c
)
target_include_directories(brickdesk_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${BRICKDESK_GENERATED_INCLUDE_DIR}"
  PRIVATE
    "${BRICKDESK_GLES2_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
)
target_link_libraries(brickdesk_core
  PUBLIC
    ${BRICKDESK_SDL2_TARGET}
  PRIVATE
    ${BRICKDESK_GLES2_LIBRARY}
    bd_stb
    brickdesk_warnings
)
target_compile_definitions(brickdesk_core PRIVATE
  BRICKDESK_BACKEND_NAME="${BRICKDESK_BACKEND}"
)

# --- application --------------------------------------------------------
add_executable(brickdesk src/main.c)
target_link_libraries(brickdesk PRIVATE brickdesk_core brickdesk_warnings)

# --- tests --------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
