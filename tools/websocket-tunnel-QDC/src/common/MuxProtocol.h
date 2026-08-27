//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Length-prefixed multiplexing protocol carried inside WebSocket binary frames.
//
// Every logical frame is wrapped as:
//
//     [1 byte frame type][4 byte big-endian payload length][payload...]
//
// Frame types:
//   - Control (text): payload is an ASCII string, either
//         "CONNECT:<uuid_hex>"    a new TCP client connected on the listener
//         "DISCONNECT:<uuid_hex>" that TCP client disconnected
//   - Data (binary): payload is [16-byte UUID][raw bytes] for that stream.
//   - Heartbeat: empty payload, sent periodically both ways to keep the QDC
//     gateway from resetting an idle forwarded stream.
//
// This lets one tunnel connection fan out to many concurrent TCP streams.

#ifndef QDC_TUNNEL_MUXPROTOCOL_H
#define QDC_TUNNEL_MUXPROTOCOL_H

#include <cstdint>
#include <string>
#include <vector>

#include "UuidUtil.h"

namespace qdc
{
    enum class FrameType : uint8_t
    {
        Control   = 0,
        Data      = 1,
        // Heartbeat: empty payload, sent periodically in both directions purely to
        // keep the underlying stream from going idle. The QDC jump host resets
        // forwarded streams that carry no application data for ~1s, so both ends
        // send heartbeats faster than that and ignore any received heartbeat.
        Heartbeat = 2
    };

    // Fixed header size: 1 byte type + 4 byte length.
    static constexpr size_t kFrameHeaderSize = 5;

    struct DecodedFrame
    {
        FrameType             type;
        std::vector<uint8_t>  payload;
    };

    namespace MuxProtocol
    {
        // Build a control frame carrying an ASCII text command (e.g. "CONNECT:<hex>").
        std::vector<uint8_t> EncodeControl(const std::string& text);

        // Build a heartbeat frame (empty payload). Sent periodically both ways to keep
        // the tunneled stream from being reset for idleness by the QDC jump host.
        std::vector<uint8_t> EncodeHeartbeat();

        // Build a data frame: [16-byte UUID][data...].
        std::vector<uint8_t> EncodeData(const Uuid& uuid, const uint8_t* data, size_t dataSize);

        // Attempt to decode one complete frame from the front of `buffer`.
        // On success, fills `out`, removes the consumed bytes from `buffer`, and returns true.
        // If there is not yet a full frame buffered, returns false and leaves `buffer` untouched.
        //
        // `fatal` (optional): set to true when the frame's declared length is corrupt
        // /hostile (exceeds the max we will buffer). In that case the header is dropped
        // to attempt a resync, but because a corrupt length means the stream framing is
        // no longer trustworthy, the caller MUST treat `fatal == true` as a reason to
        // close the connection rather than continuing to erase-and-retry indefinitely.
        bool TryDecode(std::vector<uint8_t>& buffer, DecodedFrame& out, bool* fatal = nullptr);

        // Split a data-frame payload into its UUID + inner data view.
        // Returns false if the payload is too small to contain a UUID.
        bool SplitData(const std::vector<uint8_t>& payload, Uuid& uuidOut, const uint8_t*& dataOut, size_t& dataSizeOut);
    }
}

#endif // QDC_TUNNEL_MUXPROTOCOL_H
