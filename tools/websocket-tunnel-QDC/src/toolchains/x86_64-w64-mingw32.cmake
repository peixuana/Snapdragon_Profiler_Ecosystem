#============================================================================================================
#
#                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                                SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================
#
# Cross-compilation toolchain file for building the host tunnel binary for
# Windows x86-64 using llvm-mingw (LLVM/Clang with mingw-w64 headers + runtime).
#
# Usage:
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchains/x86_64-w64-mingw32.cmake
#
# The llvm-mingw toolchain is looked up in order:
#   1. $QDC_MINGW_ROOT environment variable
#   2. ~/qdc-toolchains/llvm-mingw
#
# Download llvm-mingw (once, Linux host):
#   mkdir -p ~/qdc-toolchains
#   # See https://github.com/mstorsjo/llvm-mingw/releases for the latest tag:
#   MINGW_TAG=20240619
#   curl -L "https://github.com/mstorsjo/llvm-mingw/releases/download/${MINGW_TAG}/llvm-mingw-${MINGW_TAG}-ucrt-ubuntu-20.04-x86_64.tar.xz" \
#       | tar -xJ -C ~/qdc-toolchains/
#   mv ~/qdc-toolchains/llvm-mingw-* ~/qdc-toolchains/llvm-mingw

set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{QDC_MINGW_ROOT})
    set(_MINGW_ROOT "$ENV{QDC_MINGW_ROOT}")
else()
    set(_MINGW_ROOT "$ENV{HOME}/qdc-toolchains/llvm-mingw")
endif()

set(CMAKE_C_COMPILER   "${_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")

set(CMAKE_FIND_ROOT_PATH "${_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Host-only build; qdc-tunnel-target runs on POSIX devices only.
set(QDC_BUILD_TARGET OFF CACHE BOOL "Build qdc-tunnel-target (device binary)" FORCE)
set(QDC_BUILD_HOST   ON  CACHE BOOL "Build qdc-tunnel-host"            FORCE)
set(QDC_STATIC       ON  CACHE BOOL "Link statically"                  FORCE)
