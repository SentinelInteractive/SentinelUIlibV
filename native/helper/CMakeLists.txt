# SentinelUI — dedicated CEF host process (SentinelUI_Host.exe).
# EXE-hosted so CEF runs in its own process (the shape proven by the Phase-0
# harness): no DLL-embedding, no cross-process CEF lifecycle issues, and a page
# crash can never take the game down.

cmake_minimum_required(VERSION 3.21)
project(sentinel_helper)

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

# Builds the libcef_dll_wrapper target from the distribution.
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

add_executable(sentinel_helper WIN32
  helper.cpp
  helper_app.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../host/suilog.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(sentinel_helper)
target_include_directories(sentinel_helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../host)
set_target_properties(sentinel_helper PROPERTIES
  OUTPUT_NAME "SentinelUI_Host"
  RUNTIME_OUTPUT_DIRECTORY ${CEF_TARGET_OUT_DIR})

ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
target_link_libraries(sentinel_helper libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
add_dependencies(sentinel_helper libcef_dll_wrapper)

# Stage the CEF runtime next to the helper (self-subprocess model).
COPY_FILES("sentinel_helper" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES("sentinel_helper" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")