# SentinelUI — in-game native host.
# Builds:
#   SentinelUI.asi        (SHARED module, exported C API, CEF + MinHook)
#   SentinelUI_Worker.exe (CEF subprocess)
# Staged together with the full CEF runtime in CEF_TARGET_OUT_DIR.
#
# Configure (Ninja + MSVC, see tools/build-host.ps1):
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCEF_ROOT=<cef distro> ..

cmake_minimum_required(VERSION 3.21)
project(sentinel_host)

# ---- CEF bootstrap (same pattern as the Phase-0 harness) ----
set(OSR_CEF_ROOT "" CACHE PATH "Path to the CEF binary distribution")
if(NOT OSR_CEF_ROOT AND DEFINED ENV{CEF_ROOT})
  set(OSR_CEF_ROOT "$ENV{CEF_ROOT}")
endif()
if(NOT OSR_CEF_ROOT OR NOT IS_DIRECTORY "${OSR_CEF_ROOT}/cmake")
  message(FATAL_ERROR "CEF not found: pass -DOSR_CEF_ROOT=<path> or set the CEF_ROOT env variable.")
endif()
set(CEF_ROOT "${OSR_CEF_ROOT}")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
SET_CEF_TARGET_OUT_DIR()

# ---- hooking library (vendored) ----
set(MINHOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook" CACHE PATH "MinHook checkout")
if(NOT EXISTS "${MINHOOK_ROOT}/src/hook.c")
  message(FATAL_ERROR "MinHook not found at ${MINHOOK_ROOT} (git clone https://github.com/TsudaKageyu/minhook)")
endif()
add_subdirectory("${MINHOOK_ROOT}" minhook_build)

# Builds the libcef_dll_wrapper target from the distribution.
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

# ---- main host module: SentinelUI.asi ----
set(SENTINEL_HOST_SRCS
  host.cpp
  cef_host.cpp
  suilog.cpp
  hooks.cpp
  renderer.cpp
)
add_library(sentinel_asi SHARED ${SENTINEL_HOST_SRCS})
set_target_properties(sentinel_asi PROPERTIES
  PREFIX ""
  SUFFIX ".asi"
  OUTPUT_NAME "SentinelUI")
SET_LIBRARY_TARGET_PROPERTIES(sentinel_asi)

ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
target_include_directories(sentinel_asi PRIVATE "${MINHOOK_ROOT}/include")
target_link_libraries(sentinel_asi libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} minhook)
add_dependencies(sentinel_asi libcef_dll_wrapper minhook)

# ---- worker subprocess: SentinelUI_Worker.exe ----
# WIN32 subsystem: subprocesses must not pop console windows.
add_executable(sentinel_worker WIN32 worker.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(sentinel_worker)
set_target_properties(sentinel_worker PROPERTIES
  OUTPUT_NAME "SentinelUI_Worker"
  RUNTIME_OUTPUT_DIRECTORY ${CEF_TARGET_OUT_DIR})
target_link_libraries(sentinel_worker libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
add_dependencies(sentinel_worker libcef_dll_wrapper)

# ---- stage the CEF runtime next to the host module ----
COPY_FILES("sentinel_asi" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES("sentinel_asi" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")