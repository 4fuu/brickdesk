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
add_library(bd_stb STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_image_impl.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_image_write_impl.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_truetype_impl.c"
)
target_include_directories(bd_stb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
if(NOT MSVC)
  # Vendored code is not ours to keep warning-clean.
  target_compile_options(bd_stb PRIVATE -w)
endif()

# --- core library -------------------------------------------------------
add_library(brickdesk_core STATIC
  src/app/desktop.c
  src/app/theme.c
  src/app/ui.c
  src/app/wall.c
  src/gfx/batch.c
  src/gfx/capture.c
  src/gfx/image.c
  src/gfx/renderer.c
  src/gfx/text.c
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
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>
    brickdesk_input
  PRIVATE
    ${BRICKDESK_GLES2_LIBRARY}
    bd_stb
    brickdesk_warnings
)
target_compile_definitions(brickdesk_core
  PUBLIC
    BRICKDESK_ASSET_DIR="${BRICKDESK_ASSET_DIR}"
  PRIVATE
    BRICKDESK_BACKEND_NAME="${BRICKDESK_BACKEND}"
)

# --- configuration ------------------------------------------------------
add_library(bd_inih STATIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/inih/ini.c")
target_include_directories(bd_inih PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/inih")
# The config layer reports file:line for every diagnostic.
target_compile_definitions(bd_inih PUBLIC INI_HANDLER_LINENO=1)
if(NOT MSVC)
  # Vendored code is not ours to keep warning-clean.
  target_compile_options(bd_inih PRIVATE -w)
endif()

if(BRICKDESK_BACKEND STREQUAL "target")
  set(_bd_config_dir "/mnt/SDCARD/brickdesk/config")
  set(_bd_apps_dir "/mnt/SDCARD/Apps")
  set(_bd_state_dir "/mnt/SDCARD/brickdesk/state")
else()
  set(_bd_config_dir "${CMAKE_CURRENT_SOURCE_DIR}/config")
  set(_bd_apps_dir "${CMAKE_CURRENT_SOURCE_DIR}/apps")
  set(_bd_state_dir "/tmp/brickdesk-state")
endif()
set(BRICKDESK_CONFIG_DIR "${_bd_config_dir}" CACHE PATH
    "Default configuration directory baked into the binaries")
set(BRICKDESK_APPS_DIR "${_bd_apps_dir}" CACHE PATH
    "Default application directory baked into the session layer")
set(BRICKDESK_STATE_DIR "${_bd_state_dir}" CACHE PATH
    "Default application state root baked into the session layer")
add_library(brickdesk_config STATIC
  src/config/config.c
  src/config/schema.c
)
target_include_directories(brickdesk_config
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(brickdesk_config PRIVATE
  BRICKDESK_BUILTIN_CONFIG_DIR="${BRICKDESK_CONFIG_DIR}"
)
target_link_libraries(brickdesk_config
  PUBLIC bd_inih
  PRIVATE brickdesk_warnings
)

# --- performance --------------------------------------------------------
# Profile resolution, lease arbitration, sysfs probing and write
# transactions.  Kept free of SDL so brickdesk-perfd can run without a
# display context and the policy tests can use a fake sysfs tree.
add_library(brickdesk_perf STATIC
  src/perfd/policy.c
  src/perfd/sysfs.c
)
target_include_directories(brickdesk_perf
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_perf
  PUBLIC brickdesk_config
  PRIVATE brickdesk_warnings
)

# --- input --------------------------------------------------------------
# Keys, hotkey parsing/state machine, evdev and input.ini mapping. Kept free
# of SDL so brickdesk-inputd and bd-input can run on the device without a
# display context.
add_library(brickdesk_input STATIC
  src/input/device_map.c
  src/input/evdev.c
  src/input/hotkey.c
  src/input/keys.c
)
target_include_directories(brickdesk_input
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_input
  PUBLIC brickdesk_config
  PRIVATE brickdesk_warnings
)

# --- session ------------------------------------------------------------
# App manifests, process lifecycle, app IPC and the last-frame handoff.
# Deliberately free of SDL/GL/config so brickdesk-session can stay small.
add_library(brickdesk_session STATIC
  src/session/app_protocol.c
  src/session/json_util.c
  src/session/last_frame.c
  src/session/manifest.c
  src/session/session.c
)
target_include_directories(brickdesk_session
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsmn"
)
target_compile_definitions(brickdesk_session PRIVATE
  BRICKDESK_APPS_DIR="${BRICKDESK_APPS_DIR}"
  BRICKDESK_STATE_DIR="${BRICKDESK_STATE_DIR}"
)
target_link_libraries(brickdesk_session PRIVATE brickdesk_warnings)

# --- application --------------------------------------------------------
add_executable(brickdesk src/main.c)
target_link_libraries(brickdesk PRIVATE brickdesk_core brickdesk_config brickdesk_warnings)

# Reference app for the session protocol. It writes the last-frame handoff
# and a state file; see docs/app-protocol.md.
add_executable(brickdesk-demo apps/demo/main.c)
target_include_directories(brickdesk-demo PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsmn"
  "${BRICKDESK_GLES2_INCLUDE_DIR}"
)
target_link_libraries(brickdesk-demo PRIVATE
  ${BRICKDESK_SDL2_TARGET}
  ${BRICKDESK_GLES2_LIBRARY}
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>
  brickdesk_warnings
)

# --- input daemon and tools ---------------------------------------------
add_executable(brickdesk-inputd src/input/inputd.c)
target_link_libraries(brickdesk-inputd PRIVATE brickdesk_input brickdesk_config brickdesk_warnings)

add_executable(bd-input tools/bd-input.c)
target_link_libraries(bd-input PRIVATE brickdesk_input brickdesk_config brickdesk_warnings)

add_executable(brickdesk-perfd src/perfd/perfd.c)
target_link_libraries(brickdesk-perfd PRIVATE brickdesk_perf brickdesk_config brickdesk_warnings)

# --- tools --------------------------------------------------------------
add_executable(brickdesk-config tools/brickdesk-config.c)
target_link_libraries(brickdesk-config PRIVATE brickdesk_config brickdesk_warnings)

# --- tests --------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(BRICKDESK_BUILD_BENCH AND NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(bench)
endif()
