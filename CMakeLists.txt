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
set_target_properties(bd_stb PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
  src/plugin/render.c
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
    brickdesk_plugin
    brickdesk_session
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
set_target_properties(bd_inih PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The config layer reports file:line for every diagnostic.
target_compile_definitions(bd_inih PUBLIC INI_HANDLER_LINENO=1)
if(NOT MSVC)
  # Vendored code is not ours to keep warning-clean.
  target_compile_options(bd_inih PRIVATE -w)
endif()

if(BRICKDESK_BACKEND STREQUAL "target")
  set(_bd_config_dir "/mnt/SDCARD/brickdesk/config")
  set(_bd_apps_dir "/mnt/SDCARD/brickdesk/apps")
  set(_bd_state_dir "/mnt/SDCARD/brickdesk/state")
  set(_bd_plugin_dir "/mnt/SDCARD/brickdesk/plugins")
else()
  set(_bd_config_dir "${CMAKE_CURRENT_SOURCE_DIR}/config")
  set(_bd_apps_dir "${CMAKE_CURRENT_SOURCE_DIR}/apps")
  set(_bd_state_dir "/tmp/brickdesk-state")
  set(_bd_plugin_dir "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
endif()
set(BRICKDESK_CONFIG_DIR "${_bd_config_dir}" CACHE PATH
    "Default configuration directory baked into the binaries")
set(BRICKDESK_APPS_DIR "${_bd_apps_dir}" CACHE PATH
    "Default application directory baked into the session layer")
set(BRICKDESK_STATE_DIR "${_bd_state_dir}" CACHE PATH
    "Default application state root baked into the session layer")
set(BRICKDESK_PLUGIN_DIR "${_bd_plugin_dir}" CACHE PATH
    "Default plugin directory baked into the binaries")
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
set_target_properties(brickdesk_config PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(brickdesk_config
  PUBLIC bd_inih
  PRIVATE brickdesk_warnings
)

# --- device capabilities ------------------------------------------------
# Brightness (sysfs and /dev/disp) and ALSA volume/mute/path control.  The
# library has no SDL or config dependency so the settings page, the in-app
# menu and the bd-av diagnostic tool can share it.  ALSA controls are reached
# through the kernel control UAPI; no libasound is required on the target.
add_library(brickdesk_device STATIC
  src/device/audio.c
  src/device/audio_alsa.c
  src/device/brightness.c
  src/device/brightness_common.c
)
target_include_directories(brickdesk_device
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/device"
)
target_link_libraries(brickdesk_device PRIVATE brickdesk_warnings)
# The in-app menu shim is a shared library and links this static archive.  The
# device code must therefore be position independent; keep it that way for the
# shell and the bd-av tool too.
set_target_properties(brickdesk_device PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- device feedback ----------------------------------------------------
# Standard Linux LED class and evdev force-feedback backends.  The library is
# deliberately free of SDL, config and UI dependencies so plugins, the shell
# and the bd-feedback diagnostic tool can share it.  Vendor TG4040 LED/motor
# nodes are probe-only; see docs/device-feedback.md.
add_library(brickdesk_feedback STATIC
  src/device/feedback.c
  src/device/feedback_linux.c
)
target_include_directories(brickdesk_feedback
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/device"
)
target_link_libraries(brickdesk_feedback PRIVATE brickdesk_warnings)

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

# --- device power -------------------------------------------------------
# The power state machine and Linux sleep/shutdown backend.  Free of SDL,
# GLES, ALSA and backlight code: the shell supplies those as hooks.  The CLI
# is a read-only capability probe unless state-changing flags are explicit.
add_library(brickdesk_power STATIC
  src/device/power.c
  src/device/power_linux.c
)
target_include_directories(brickdesk_power
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_power PRIVATE brickdesk_warnings)

add_executable(bd-power src/device/power_probe_main.c)
target_link_libraries(bd-power PRIVATE brickdesk_power brickdesk_warnings)

# --- device battery -----------------------------------------------------
# Read-only power_supply battery/charger view and the UI-independent
# low-battery policy.  The sysfs root is injectable, so the same code runs on
# the device and against fixture trees on the host.  It never writes sysfs,
# PMIC registers or the power state machine; the consumer owns safety actions.
add_library(brickdesk_battery STATIC
  src/device/battery.c
  src/device/battery_history.c
  src/device/battery_linux.c
)
target_include_directories(brickdesk_battery
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/device"
)
target_link_libraries(brickdesk_battery PRIVATE brickdesk_warnings)

add_executable(bd-battery src/device/battery_probe_main.c)
target_link_libraries(bd-battery PRIVATE brickdesk_battery brickdesk_warnings)

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

# --- system information ------------------------------------------------
# Read-only /proc and /sys sampling used by the event loop.  Kept free of
# SDL/GL so it can be tested with a fixture root and does not grow the
# session library.
add_library(brickdesk_sysinfo STATIC
  src/system/sysinfo.c
  src/system/bootlogo.c
)
target_include_directories(brickdesk_sysinfo
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_sysinfo PRIVATE brickdesk_warnings)

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

# --- v3 home page -------------------------------------------------------
# The home page is a reusable module: the event loop owns input/session and
# feeds it actions, items, running state and a monotonic timestamp. It links
# the session library only for the M1-06 last-frame reader.
add_library(brickdesk_home STATIC
  src/home/home.c
  src/home/home_draw.c
)
target_include_directories(brickdesk_home
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_home
  PUBLIC brickdesk_core brickdesk_session
  PRIVATE brickdesk_warnings
)

# --- desktop event loop -------------------------------------------------
# Owns input, the session, the plugin host and the home page.  It sits above
# both brickdesk_core (renderer/UI) and brickdesk_home so neither has to know
# about the other's callers.
add_library(brickdesk_shell STATIC
  src/app/app_history.c
  src/app/shell.c
  src/app/notice.c
  src/app/perf_client.c
  src/app/led.c
  src/app/haptics.c
)
target_include_directories(brickdesk_shell
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_shell
  PUBLIC
    brickdesk_home
    brickdesk_core
    brickdesk_config
    brickdesk_input
    brickdesk_plugin
    brickdesk_session
    brickdesk_sysinfo
    brickdesk_device
    brickdesk_battery
    brickdesk_power
    brickdesk_feedback
  PRIVATE brickdesk_warnings
)

# --- plugins ------------------------------------------------------------
# Manifest parsing, lifecycle, JSON IPC, client SDK and the shared-memory
# tile surface. Kept free of SDL/GL so plugin lifecycle tests can run
# without a display; src/plugin/render.c is compiled into the core library.
add_library(brickdesk_plugin STATIC
  src/plugin/client.c
  src/plugin/host.c
  src/plugin/ipc.c
  src/plugin/json.c
  src/plugin/jsmn_impl.c
  src/plugin/manifest.c
  src/plugin/ui_tree.c
)
target_include_directories(brickdesk_plugin
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsmn"
)
target_compile_definitions(brickdesk_plugin
  PUBLIC BRICKDESK_PLUGIN_DIR="${BRICKDESK_PLUGIN_DIR}"
  PRIVATE BRICKDESK_STATE_DIR="${BRICKDESK_STATE_DIR}"
)
target_link_libraries(brickdesk_plugin PRIVATE brickdesk_warnings)

# --- v3 settings page ---------------------------------------------------
# Schema-driven settings UI.  Owns focus, rows, sub-pages and damage; the
# event loop only feeds actions and time.  Links the plugin host because the
# plugins page renders the host's declarative settings tree, and the perf
# policy because the performance page lists perf.ini profiles.
add_library(brickdesk_settings STATIC
  src/settings/settings.c
  src/settings/settings_draw.c
  src/settings/system_stub.c
)
target_include_directories(brickdesk_settings
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(brickdesk_settings
  PUBLIC brickdesk_core brickdesk_config brickdesk_input brickdesk_plugin
  PRIVATE brickdesk_perf brickdesk_battery brickdesk_sysinfo brickdesk_warnings
)

target_link_libraries(brickdesk_shell PUBLIC brickdesk_settings)

# --- in-app menu shim (M1-09) ------------------------------------------
# LD_PRELOAD overlay for dynamically linked SDL2/EGL applications. It has no
# SDL, EGL or GLES link-time dependency: the present calls and GL entry points
# are resolved from the host process, so the library still loads into apps
# that only use one of the paths. docs/menu-shim.md covers mode selection.
find_package(Threads REQUIRED)
add_library(brickdesk-menu SHARED
  src/menu/clients.c
  src/menu/render.c
  src/menu/shim.c
  src/menu/system.c
  src/session/last_frame.c
)
set_target_properties(brickdesk-menu PROPERTIES
  OUTPUT_NAME "brickdesk-menu"
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden
)
target_include_directories(brickdesk-menu PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${BRICKDESK_GLES2_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
)
target_compile_definitions(brickdesk-menu PRIVATE
  BRICKDESK_ASSET_DIR="${BRICKDESK_ASSET_DIR}"
)
target_link_libraries(brickdesk-menu PRIVATE
  brickdesk_config
  brickdesk_device
  bd_stb
  Threads::Threads
  ${CMAKE_DL_LIBS}
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>
  brickdesk_warnings
)
if(NOT MSVC)
  # Keep every static-library symbol (config, stb, last-frame) local; only the
  # hooks and the bd_menu_* API are interposed into the application.
  target_link_options(brickdesk-menu PRIVATE
    "LINKER:--exclude-libs,ALL"
    "LINKER:--no-undefined"
    "LINKER:-soname,libbrickdesk-menu.so"
  )
endif()

# --- application --------------------------------------------------------
add_executable(brickdesk src/main.c)
target_link_libraries(brickdesk PRIVATE brickdesk_shell brickdesk_warnings)
target_compile_definitions(brickdesk PRIVATE
  BRICKDESK_STATE_DIR="${BRICKDESK_STATE_DIR}")

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

# --- sample plugins -----------------------------------------------------
# Built into plugins/hello/bin so the host default directory works from the
# repository and staged device trees. hello-py is source-only (host tests).
add_executable(bd_hello_plugin plugins/hello/main.c)
target_link_libraries(bd_hello_plugin PRIVATE brickdesk_plugin brickdesk_warnings)
set_target_properties(bd_hello_plugin PROPERTIES
  OUTPUT_NAME hello
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins/hello/bin")

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

add_executable(bd-av tools/bd-av.c)
target_link_libraries(bd-av PRIVATE brickdesk_device brickdesk_warnings)

add_executable(bd-feedback
  tools/bd-feedback.c
  tools/bd-feedback-play.c
)
target_link_libraries(bd-feedback PRIVATE brickdesk_feedback brickdesk_warnings)

# --- tests --------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(BRICKDESK_BUILD_BENCH)
  add_subdirectory(bench)
endif()
