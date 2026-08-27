#============================================================================================================
#
#                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                                SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================
#
# Cross-compilation toolchain file for building the HOST tunnel binary for
# aarch64 (ARM64) Linux hosts, using the same musl-libc static cross-compiler as
# the device build. Produces a static qdc-tunnel-host that runs on any aarch64
# Linux distribution (Graviton/Ampere servers, ARM64 workstations, etc.).
#
# This is the ARM64-host counterpart of x86_64-w64-mingw32.cmake / native x86_64:
# it builds qdc-tunnel-host (NOT the device qdc-tunnel-target).
#
# Usage:
#   cmake -B build-host-arm64 -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl-host.cmake
#
# The toolchain root is looked up exactly like aarch64-linux-musl.cmake:
#   1. $QDC_MUSL_ROOT environment variable
#   2. ~/qdc-toolchains/aarch64-linux-musl-cross      (musl.cc layout)
#   3. ~/qdc-toolchains/aarch64-unknown-linux-musl    (cross-tools layout)

set(CMAKE_SYSTEM_NAME    Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Locate the cross-compiler root.
if(DEFINED ENV{QDC_MUSL_ROOT})
    set(_MUSL_ROOT "$ENV{QDC_MUSL_ROOT}")
elseif(EXISTS "$ENV{HOME}/qdc-toolchains/aarch64-linux-musl-cross")
    set(_MUSL_ROOT "$ENV{HOME}/qdc-toolchains/aarch64-linux-musl-cross")
else()
    set(_MUSL_ROOT "$ENV{HOME}/qdc-toolchains/aarch64-unknown-linux-musl")
endif()

# Detect the compiler prefix by probing for the gcc driver of each known layout.
if(EXISTS "${_MUSL_ROOT}/bin/aarch64-linux-musl-gcc")
    set(_MUSL_TRIPLE "aarch64-linux-musl")
elseif(EXISTS "${_MUSL_ROOT}/bin/aarch64-unknown-linux-musl-gcc")
    set(_MUSL_TRIPLE "aarch64-unknown-linux-musl")
else()
    message(FATAL_ERROR
        "No aarch64 musl gcc driver found under '${_MUSL_ROOT}/bin'. "
        "Set QDC_MUSL_ROOT to the extracted toolchain directory.")
endif()

set(CMAKE_C_COMPILER   "${_MUSL_ROOT}/bin/${_MUSL_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${_MUSL_ROOT}/bin/${_MUSL_TRIPLE}-g++")

if(EXISTS "${_MUSL_ROOT}/${_MUSL_TRIPLE}/sysroot")
    set(CMAKE_SYSROOT "${_MUSL_ROOT}/${_MUSL_TRIPLE}/sysroot")
else()
    set(CMAKE_SYSROOT "${_MUSL_ROOT}/${_MUSL_TRIPLE}")
endif()
set(CMAKE_FIND_ROOT_PATH "${_MUSL_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Host-only build; the device binary is produced by aarch64-linux-musl.cmake.
set(QDC_BUILD_TARGET OFF CACHE BOOL "Build qdc-tunnel-target (device binary)" FORCE)
set(QDC_BUILD_HOST   ON  CACHE BOOL "Build qdc-tunnel-host"            FORCE)
set(QDC_STATIC       ON  CACHE BOOL "Link statically"                  FORCE)
