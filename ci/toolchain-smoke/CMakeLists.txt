cmake_minimum_required(VERSION 3.16)
project(brickdesk_toolchain_smoke C)

# Small project used by .github/workflows/target.yml to prove that
# cmake/toolchain-aarch64.cmake works outside brickdesk's own CMakeLists.txt.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_library(EGL_LIBRARY NAMES EGL)
find_library(GLES2_LIBRARY NAMES GLESv2)
if(NOT EGL_LIBRARY OR NOT GLES2_LIBRARY)
    message(FATAL_ERROR "EGL/GLESv2 not found; check TRIMUI_SDK")
endif()

add_executable(toolchain_smoke main.c)
target_include_directories(toolchain_smoke PRIVATE "${TRIMUI_SDK_INCLUDE_DIR}")
target_link_libraries(toolchain_smoke PRIVATE "${EGL_LIBRARY}" "${GLES2_LIBRARY}")
