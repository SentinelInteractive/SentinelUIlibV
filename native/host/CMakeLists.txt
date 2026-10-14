# SentinelUI: in-game ASI compositor; no CEF in this module.
# Responsibilities: hook Present/WndProc, spawn + supervise the dedicated
# CEF helper process, composite its shared-memory frames, export the Sui* API.
#
# Configure (Ninja + MSVC, see tools/build-host.ps1):
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..

cmake_minimum_required(VERSION 3.21)
project(sentinel_host)

set(MINHOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook" CACHE PATH "MinHook checkout")
if(NOT EXISTS "${MINHOOK_ROOT}/src/hook.c")
  message(FATAL_ERROR "MinHook not found at ${MINHOOK_ROOT}")
endif()
add_subdirectory("${MINHOOK_ROOT}" minhook_build)

add_library(sentinel_asi SHARED
  host.cpp
  suilog.cpp
  crash.cpp
  hooks.cpp
  ipc.cpp
  layers.cpp
  renderer.cpp)
set_target_properties(sentinel_asi PROPERTIES
  PREFIX ""
  SUFFIX ".asi"
  OUTPUT_NAME "SentinelUI")
target_include_directories(sentinel_asi PRIVATE "${MINHOOK_ROOT}/include")
target_link_libraries(sentinel_asi PRIVATE minhook d3d11 dxgi d3dcompiler)
add_dependencies(sentinel_asi minhook)