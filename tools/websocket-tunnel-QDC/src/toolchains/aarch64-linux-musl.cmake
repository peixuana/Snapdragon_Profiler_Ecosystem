#============================================================================================================
#
#                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                                SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================
#
# Cross-compilation toolchain file for targeting aarch64 Android / Qualcomm Linux
# using a musl-libc static cross-compiler.
#
# Two toolchain distributions are supported (auto-detected by compiler prefix):
#   * musl.cc          "aarch64-linux-musl-cross"          -> aarch64-linux-musl-g++
#   * cross-tools      "aarch64-unknown-linux-musl"         -> aarch64-unknown-linux-musl-g++
#
# Usage:
#   cmake -B build-target -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl.cmake
#
# The toolchain root is looked up in order:
#   1. $QDC_MUSL_ROOT environment variable
#   2. ~/qdc-toolchains/aarch64-linux-musl-cross      (musl.cc layout)
#   3. ~/qdc-toolchains/aarch64-unknown-linux-musl    (cross-tools layout)
#
# Download the musl.cc toolchain (once):
#   mkdir -p ~/qdc-toolchains
#   curl -L https://musl.cc/aarch64-linux-musl-cross.tgz | tar -xz -C ~/qdc-toolchains/

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

# Sysroot for staging/searching libraries (optional, but avoids accidental host-lib use).
# musl.cc keeps the sysroot at <root>/<triple>; cross-tools keeps it at <root>/<triple>/sysroot.
if(EXISTS "${_MUSL_ROOT}/${_MUSL_TRIPLE}/sysroot")
    set(CMAKE_SYSROOT "${_MUSL_ROOT}/${_MUSL_TRIPLE}/sysroot")
else()
    set(CMAKE_SYSROOT "${_MUSL_ROOT}/${_MUSL_TRIPLE}")
endif()
set(CMAKE_FIND_ROOT_PATH "${_MUSL_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Force target-only build + static link (no JVM / shared-lib deps on the device).
set(QDC_BUILD_TARGET ON  CACHE BOOL "Build qdc-tunnel-target (device binary)" FORCE)
set(QDC_BUILD_HOST   OFF CACHE BOOL "Build qdc-tunnel-host"            FORCE)
set(QDC_STATIC       ON  CACHE BOOL "Link statically"                  FORCE)
