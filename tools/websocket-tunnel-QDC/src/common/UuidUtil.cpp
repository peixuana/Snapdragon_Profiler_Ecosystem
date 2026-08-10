//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

#include "UuidUtil.h"

#include <cstdio>
#include <cstdlib>

namespace qdc
{
    namespace UuidUtil
    {
        Uuid RandomBytes()
        {
            Uuid uuid{};

            FILE* f = std::fopen("/dev/urandom", "rb");
            if (f != nullptr)
            {
                size_t read = std::fread(uuid.data(), 1, uuid.size(), f);
                std::fclose(f);
                if (read == uuid.size())
                {
                    return uuid;
                }
            }

            // Fallback: not cryptographically strong but sufficient for stream demux ids.
            for (size_t i = 0; i < uuid.size(); ++i)
            {
                uuid[i] = static_cast<uint8_t>(std::rand() & 0xFF);
            }
            return uuid;
        }

        std::string ToHex(const Uuid& uuid)
        {
            static const char* kHexDigits = "0123456789abcdef";
            std::string hex;
            hex.reserve(uuid.size() * 2);
            for (uint8_t b : uuid)
            {
                hex.push_back(kHexDigits[(b >> 4) & 0x0F]);
                hex.push_back(kHexDigits[b & 0x0F]);
            }
            return hex;
        }

        bool FromHex(const std::string& hex, Uuid& out)
        {
            if (hex.size() != kUuidSize * 2)
            {
                return false;
            }

            auto hexVal = [](char c, uint8_t& val) -> bool
            {
                if (c >= '0' && c <= '9') { val = static_cast<uint8_t>(c - '0'); return true; }
                if (c >= 'a' && c <= 'f') { val = static_cast<uint8_t>(c - 'a' + 10); return true; }
                if (c >= 'A' && c <= 'F') { val = static_cast<uint8_t>(c - 'A' + 10); return true; }
                return false;
            };

            for (size_t i = 0; i < kUuidSize; ++i)
            {
                uint8_t hi = 0;
                uint8_t lo = 0;
                if (!hexVal(hex[i * 2], hi) || !hexVal(hex[i * 2 + 1], lo))
                {
                    return false;
                }
                out[i] = static_cast<uint8_t>((hi << 4) | lo);
            }
            return true;
        }
    }
}
