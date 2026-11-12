# CMake toolchain for the TrimUI Brick Pro (Allwinner A133P, aarch64).
#
# This file is standalone: point any CMake project at it and it will use the
# official TrimUI GCC 7.5 toolchain plus the SDK headers and the patched SDL2
# sysroot. Nothing here depends on brickdesk's own CMakeLists.txt.
#
# Environment overrides:
#   TRIMUI_TOOLCHAIN  toolchain root (default /opt/trimui/toolchain)
#   TRIMUI_SDK        SDK root (default /opt/trimui/sdk)
#   TRIMUI_SYSROOT    brickdesk target sysroot with the patched SDL2
#                     (default /opt/trimui/sysroot, then <repo>/build-target/sysroot)
#
# Usage:
#   cmake -S . -B build-target -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{TRIMUI_TOOLCHAIN})
    set(TRIMUI_TOOLCHAIN_ROOT "$ENV{TRIMUI_TOOLCHAIN}")
else()
    set(TRIMUI_TOOLCHAIN_ROOT "/opt/trimui/toolchain")
endif()

if(DEFINED ENV{TRIMUI_SDK})
    set(TRIMUI_SDK_ROOT "$ENV{TRIMUI_SDK}")
else()
    set(TRIMUI_SDK_ROOT "/opt/trimui/sdk")
endif()

if(DEFINED ENV{TRIMUI_SYSROOT})
    set(BRICKDESK_TARGET_SYSROOT "$ENV{TRIMUI_SYSROOT}")
elseif(EXISTS "/opt/trimui/sysroot/lib/libSDL2-2.0.so.0")
    set(BRICKDESK_TARGET_SYSROOT "/opt/trimui/sysroot")
else()
    get_filename_component(_bd_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(BRICKDESK_TARGET_SYSROOT "${_bd_repo_root}/build-target/sysroot")
endif()

set(TRIMUI_TOOLCHAIN_ROOT "${TRIMUI_TOOLCHAIN_ROOT}" CACHE PATH "TrimUI GCC 7.5 toolchain root")
set(TRIMUI_SDK_ROOT "${TRIMUI_SDK_ROOT}" CACHE PATH "TrimUI target SDK root")
set(BRICKDESK_TARGET_SYSROOT "${BRICKDESK_TARGET_SYSROOT}" CACHE PATH "Sysroot containing the patched SDL2")
set(TRIMUI_SDK_INCLUDE_DIR "${TRIMUI_SDK_ROOT}/usr/include" CACHE PATH "SDK headers (EGL, GLES2, ALSA)")
set(TRIMUI_SDK_LIB_DIR "${TRIMUI_SDK_ROOT}/usr/lib" CACHE PATH "SDK vendor libraries")

if(NOT EXISTS "${TRIMUI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc")
    message(FATAL_ERROR "aarch64-linux-gnu-gcc not found under ${TRIMUI_TOOLCHAIN_ROOT}")
endif()
if(NOT EXISTS "${TRIMUI_SDK_INCLUDE_DIR}/EGL/egl.h")
    message(FATAL_ERROR "EGL headers not found under ${TRIMUI_SDK_INCLUDE_DIR}")
endif()

set(CMAKE_C_COMPILER "${TRIMUI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TRIMUI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-g++")
set(CMAKE_AR "${TRIMUI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${TRIMUI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${TRIMUI_TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-strip" CACHE FILEPATH "")

# The compiler already carries its own glibc 2.25 sysroot. Do not set
# CMAKE_SYSROOT: it would hide the toolchain headers. Search target headers and
# libraries only in the toolchain sysroot, the patched SDL2 sysroot and the SDK.
set(_trimui_vendor_lib_dir "${BRICKDESK_TARGET_SYSROOT}/vendor-lib")
if(EXISTS "${_trimui_vendor_lib_dir}/libEGL.so")
    set(CMAKE_FIND_ROOT_PATH
        "${TRIMUI_TOOLCHAIN_ROOT}/aarch64-linux-gnu/libc"
        "${_trimui_vendor_lib_dir}"
        "${BRICKDESK_TARGET_SYSROOT}"
        "${TRIMUI_SDK_ROOT}/usr"
    )
else()
    set(CMAKE_FIND_ROOT_PATH
        "${TRIMUI_TOOLCHAIN_ROOT}/aarch64-linux-gnu/libc"
        "${BRICKDESK_TARGET_SYSROOT}"
        "${TRIMUI_SDK_ROOT}/usr"
    )
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# SDK headers do not include libc headers, so adding them globally is safe and
# lets plain CMake checks find EGL/GLES2 without project-specific flags.
set(CMAKE_C_FLAGS_INIT "-isystem ${TRIMUI_SDK_INCLUDE_DIR} -isystem ${BRICKDESK_TARGET_SYSROOT}/include")
set(CMAKE_CXX_FLAGS_INIT "-isystem ${TRIMUI_SDK_INCLUDE_DIR} -isystem ${BRICKDESK_TARGET_SYSROOT}/include")

# Vendor .so files have DT_NEEDED entries that live in the SDK. Resolve them
# from the filtered vendor directory so ld does not pick the SDK's glibc 2.23
# stubs. Without that directory (an older image), vendor blobs still link with
# --allow-shlib-undefined; their symbols are resolved on the device at runtime.
if(EXISTS "${_trimui_vendor_lib_dir}/libEGL.so")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath-link,${_trimui_vendor_lib_dir}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-rpath-link,${_trimui_vendor_lib_dir}")
else()
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")
endif()
