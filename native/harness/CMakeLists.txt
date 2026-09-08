# SentinelUI — CEF OSR validation harness (Phase 0 gate).
# Builds osr_harness.exe against the pinned CEF binary distribution.
#
# Configure (Ninja + MSVC via vcvars64):
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCEF_ROOT=<abs path to cef distro> ..
# Build:
#   cmake --build .
# (tools/build-harness.ps1 wraps both steps with vswhere + vcvars64 + Ninja.)

cmake_minimum_required(VERSION 3.21)
project(sentinel_osr_harness)

# Locate the CEF binary distribution (exactly like the distro's own bootstrap,
# "option E": CEF_ROOT env variable, or an explicit OSR_CEF_ROOT cache var).
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

# Resolve the target output directory ($<CONFIGURATION>-aware) before defining targets.
SET_CEF_TARGET_OUT_DIR()

# Builds the libcef_dll_wrapper target from the distribution.
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

# Console subsystem on purpose: the harness writes its [OSR] gate log to stdout.
add_executable(osr_harness osr_harness.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(osr_harness)
# Colocate the executable with the staged CEF runtime (libcef.dll, Resources, ...)
# so the exe finds libcef at startup.
set_target_properties(osr_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CEF_TARGET_OUT_DIR})

# Link the correct (Debug|Release) libcef import lib per configuration.
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
target_link_libraries(osr_harness libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
add_dependencies(osr_harness libcef_dll_wrapper)

# Stage the CEF binaries/resources next to the harness executable
# (libcef.dll, icudtl.dat, locales, *.pak, worker-less: the exe self-spawns).
COPY_FILES("osr_harness" "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
COPY_FILES("osr_harness" "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")