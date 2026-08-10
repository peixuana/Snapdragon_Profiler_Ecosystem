#============================================================================================================
#
#                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                                SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================
#
# Cross-compilation toolchain file for building the host tunnel binary for
# Windows ARM64 (aarch64) using llvm-mingw (LLVM/Clang with mingw-w64 headers +
# runtime). This is the ARM64 counterpart of x86_64-w64-mingw32.cmake, for
# Windows-on-ARM hosts (Snapdragon X / Copilot+ PCs, etc.).
#
# Usage:
#   cmake -B build-win-arm64 -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-w64-mingw32.cmake
#
# The llvm-mingw toolchain is looked up in order:
#   1. $QDC_MINGW_ROOT environment variable
#   2. ~/qdc-toolchains/llvm-mingw
#
# Download llvm-mingw (once, Linux host) - see x86_64-w64-mingw32.cmake for the
# full curl/tar recipe; the same distribution bundles the aarch64 target triple.

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{QDC_MINGW_ROOT})
    set(_MINGW_ROOT "$ENV{QDC_MINGW_ROOT}")
else()
    set(_MINGW_ROOT "$ENV{HOME}/qdc-toolchains/llvm-mingw")
endif()

set(CMAKE_C_COMPILER   "${_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_MINGW_ROOT}/bin/aarch64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${_MINGW_ROOT}/bin/aarch64-w64-mingw32-windres")

set(CMAKE_FIND_ROOT_PATH "${_MINGW_ROOT}/aarch64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Host-only build; qdc-tunnel-target runs on POSIX devices only.
set(QDC_BUILD_TARGET OFF CACHE BOOL "Build qdc-tunnel-target (device binary)" FORCE)
set(QDC_BUILD_HOST   ON  CACHE BOOL "Build qdc-tunnel-host"            FORCE)
set(QDC_STATIC       ON  CACHE BOOL "Link statically"                  FORCE)
