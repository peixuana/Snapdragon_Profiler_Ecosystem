//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

#include "MuxProtocol.h"

#include <cstring>

namespace qdc
{
    namespace
    {
        // Maximum payload size we are willing to buffer for a single frame (16 MB).
        // Guards against a malformed/hostile length field causing huge allocations.
        constexpr uint32_t kMaxFramePayload = 16u * 1024u * 1024u;

        void AppendHeader(std::vector<uint8_t>& out, FrameType type, uint32_t length)
        {
            out.push_back(static_cast<uint8_t>(type));
            out.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
            out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(length & 0xFF));
        }
    }

    namespace MuxProtocol
    {
        std::vector<uint8_t> EncodeControl(const std::string& text)
        {
            std::vector<uint8_t> frame;
            frame.reserve(kFrameHeaderSize + text.size());
            AppendHeader(frame, FrameType::Control, static_cast<uint32_t>(text.size()));
            frame.insert(frame.end(), text.begin(), text.end());
            return frame;
        }

        std::vector<uint8_t> EncodeHeartbeat()
        {
            std::vector<uint8_t> frame;
            frame.reserve(kFrameHeaderSize);
            AppendHeader(frame, FrameType::Heartbeat, 0);
            return frame;
        }

        std::vector<uint8_t> EncodeData(const Uuid& uuid, const uint8_t* data, size_t dataSize)
        {
            const uint32_t payloadLen = static_cast<uint32_t>(kUuidSize + dataSize);
            std::vector<uint8_t> frame;
            frame.reserve(kFrameHeaderSize + payloadLen);
            AppendHeader(frame, FrameType::Data, payloadLen);
            frame.insert(frame.end(), uuid.begin(), uuid.end());
            if (data != nullptr && dataSize > 0)
            {
                frame.insert(frame.end(), data, data + dataSize);
            }
            return frame;
        }

        bool TryDecode(std::vector<uint8_t>& buffer, DecodedFrame& out, bool* fatal)
        {
            if (fatal != nullptr)
            {
                *fatal = false;
            }

            if (buffer.size() < kFrameHeaderSize)
            {
                return false;
            }

            const uint8_t typeByte = buffer[0];
            const uint32_t length =
                (static_cast<uint32_t>(buffer[1]) << 24) |
                (static_cast<uint32_t>(buffer[2]) << 16) |
                (static_cast<uint32_t>(buffer[3]) << 8)  |
                (static_cast<uint32_t>(buffer[4]));

            if (length > kMaxFramePayload)
            {
                // Corrupt/hostile length: the stream framing is no longer trustworthy.
                // Drop the header (best-effort resync) but flag the failure as fatal so
                // the caller closes the connection instead of erasing-and-retrying
                // forever on a garbage stream.
                buffer.erase(buffer.begin(), buffer.begin() + kFrameHeaderSize);
                if (fatal != nullptr)
                {
                    *fatal = true;
                }
                return false;
            }

            if (buffer.size() < kFrameHeaderSize + length)
            {
                // Not a full frame yet.
                return false;
            }

            switch (typeByte)
            {
            case static_cast<uint8_t>(FrameType::Control):
                out.type = FrameType::Control;
                break;
            case static_cast<uint8_t>(FrameType::Heartbeat):
                out.type = FrameType::Heartbeat;
                break;
            default:
                out.type = FrameType::Data;
                break;
            }
            out.payload.assign(buffer.begin() + kFrameHeaderSize,
                               buffer.begin() + kFrameHeaderSize + length);

            buffer.erase(buffer.begin(), buffer.begin() + kFrameHeaderSize + length);
            return true;
        }

        bool SplitData(const std::vector<uint8_t>& payload, Uuid& uuidOut, const uint8_t*& dataOut, size_t& dataSizeOut)
        {
            if (payload.size() < kUuidSize)
            {
                return false;
            }
            std::memcpy(uuidOut.data(), payload.data(), kUuidSize);
            dataOut = payload.data() + kUuidSize;
            dataSizeOut = payload.size() - kUuidSize;
            return true;
        }
    }
}
