#============================================================================================================
#
#                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                                SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================
#
# C++ WebSocket reverse tunnel for QDC (Android + Qualcomm Linux / IoT). Builds two
# self-contained, dependency-free executables:
#
#   qdc-tunnel-target - device / TARGET side (reverse tunnel server). POSIX only.
#                       Built as a single static aarch64 ELF (musl) that runs on
#                       both Android and Qualcomm Linux, so deployment is one small
#                       push instead of a JRE/DEX bundle.
#
#   qdc-tunnel-host   - HOST side (WebSocket client + forwarder). Cross-platform
#                       (Linux + Windows) via the NetCompat sockets shim.
#
# Typical configurations:
#
#   # Native (this dev machine): build both for local loopback testing
#   cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release && cmake --build build/native
#
#   # Device target: single static aarch64 ELF (Android + Qualcomm Linux)
#   cmake -S . -B build/target -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-musl.cmake
#   cmake --build build/target            # -> qdc-tunnel-target
#
#   # Windows host: native .exe (no MSVC / no runtime install needed)
#   cmake -S . -B build/win -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=toolchains/x86_64-w64-mingw32.cmake
#   cmake --build build/win               # -> qdc-tunnel-host.exe

cmake_minimum_required(VERSION 3.13)

project(qdc_tunnel_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(QDC_BUILD_TARGET "Build the device-side qdc-tunnel-target binary (POSIX only)" ON)
option(QDC_BUILD_HOST   "Build the host-side qdc-tunnel-host binary"                  ON)
option(QDC_STATIC       "Link the C/C++ runtime statically"                    OFF)

# The device-side target uses raw POSIX sockets (poll.h, arpa/inet.h, ...) and can
# never build for Windows. The mingw toolchain also disables it, but guard here too.
if(WIN32)
    set(QDC_BUILD_TARGET OFF)
endif()

# Protocol layer shared by both binaries (pure byte manipulation, no sockets).
set(QDC_COMMON_SRC
    common/Logging.cpp
    common/UuidUtil.cpp
    common/MuxProtocol.cpp
    common/WebSocket.cpp
)

find_package(Threads REQUIRED)

# Apply the size-reduction and static-link options that matter for a binary that is
# pushed to a bandwidth-constrained QDC device.
function(qdc_tune tgt is_device)
    target_include_directories(${tgt} PRIVATE common)

    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4 /WX- /D_CRT_SECURE_NO_WARNINGS)
        if(is_device)
            target_compile_options(${tgt} PRIVATE /EHs-)
            target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:/O2 /Gy>)
            target_link_options(${tgt} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
        else()
            target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:/O2>)
        endif()
        if(QDC_STATIC)
            target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:/MT> $<$<CONFIG:Debug>:/MTd>)
        endif()
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
        if(is_device)
            target_compile_options(${tgt} PRIVATE
                -fno-exceptions
                $<$<CONFIG:Release>:-O2>
                $<$<CONFIG:Release>:-ffunction-sections>
                $<$<CONFIG:Release>:-fdata-sections>
            )
            target_link_options(${tgt} PRIVATE
                $<$<CONFIG:Release>:-Wl,--gc-sections>
                $<$<CONFIG:Release>:-Wl,-s>
            )
        else()
            target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:-O2>)
            target_link_options(${tgt} PRIVATE $<$<CONFIG:Release>:-Wl,-s>)
        endif()
        if(QDC_STATIC)
            target_link_options(${tgt} PRIVATE -static)
        endif()
    endif()

    target_link_libraries(${tgt} PRIVATE Threads::Threads)
endfunction()

# ── Device / target binary ──────────────────────────────────────────────────
if(QDC_BUILD_TARGET)
    add_executable(qdc-tunnel-target
        ${QDC_COMMON_SRC}
        target/TunnelPort.cpp
        target/main.cpp
    )
    target_include_directories(qdc-tunnel-target PRIVATE target)
    qdc_tune(qdc-tunnel-target TRUE)
endif()

# ── Host binary ───────────────────────────────────────────────────────────────
if(QDC_BUILD_HOST)
    add_executable(qdc-tunnel-host
        ${QDC_COMMON_SRC}
        common/NetCompat.cpp
        host/HostTunnelPort.cpp
        host/main.cpp
    )
    target_include_directories(qdc-tunnel-host PRIVATE host)
    qdc_tune(qdc-tunnel-host FALSE)
    if(WIN32)
        target_link_libraries(qdc-tunnel-host PRIVATE ws2_32)
    endif()
endif()
