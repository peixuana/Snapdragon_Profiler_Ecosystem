//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// 16-byte connection UUID helpers used to multiplex multiple TCP streams over a
// single tunnel connection. Mirrors the Java UuidUtil in the original
// websocket-tunnel-QDC implementation.

#ifndef QDC_TUNNEL_UUIDUTIL_H
#define QDC_TUNNEL_UUIDUTIL_H

#include <array>
#include <cstdint>
#include <string>

namespace qdc
{
    // A connection identifier is exactly 16 raw bytes.
    static constexpr size_t kUuidSize = 16;
    using Uuid = std::array<uint8_t, kUuidSize>;

    namespace UuidUtil
    {
        // Generate 16 random bytes (from /dev/urandom, falling back to rand()).
        Uuid RandomBytes();

        // Convert a 16-byte UUID to its 32-char lowercase hex representation.
        std::string ToHex(const Uuid& uuid);

        // Parse a 32-char hex string back into 16 bytes. Returns false on malformed input.
        bool FromHex(const std::string& hex, Uuid& out);
    }
}

#endif // QDC_TUNNEL_UUIDUTIL_H
