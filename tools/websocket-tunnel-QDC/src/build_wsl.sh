#!/usr/bin/env bash
#============================================================================================================
#
#                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                                SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
BUILD="$SRC/build/native"

echo "==> cmake: $(cmake --version | head -1)"
echo "==> g++: $(g++ --version | head -1)"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQDC_BUILD_HOST=ON \
    -DQDC_BUILD_TARGET=ON

cmake --build "$BUILD"

echo ""
echo "==> Built binaries:"
find "$BUILD" -maxdepth 1 -name "qdc-tunnel*" -type f -exec ls -lh {} \;
