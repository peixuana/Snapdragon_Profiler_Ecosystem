//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Minimal, dependency-free WebSocket (RFC 6455) helper shared by both tunnel ends.
//
//   Target (device) side  -> WebSocket SERVER:
//       ParseHttpHandshake / BuildHandshakeResponse complete the HTTP Upgrade;
//       EncodeBinary emits unmasked server->client frames; TryDecodeFrame (with
//       requireMask=true) decodes the masked client->server frames.
//
//   Host side             -> WebSocket CLIENT:
//       BuildClientHandshake produces the GET Upgrade request; ParseServerHandshake
//       validates the 101 response; EncodeBinaryMasked emits masked client->server
//       frames (clients MUST mask per RFC 6455); TryDecodeFrame (with
//       requireMask=false) decodes the unmasked server->client frames.
//
// Only the subset needed by the tunnel is implemented: binary/text data frames,
// ping/pong and close control frames, and fragmentation reassembly.

#ifndef QDC_TUNNEL_WEBSOCKET_H
#define QDC_TUNNEL_WEBSOCKET_H

#include <cstdint>
#include <string>
#include <vector>

namespace qdc
{
    namespace WebSocket
    {
        // ── Server side handshake ───────────────────────────────────────

        // Result of trying to parse an incoming HTTP request as a WS handshake.
        struct Handshake
        {
            bool        complete = false;   // full request headers received
            bool        valid    = false;   // it is a valid WS upgrade request
            std::string acceptKey;          // value for Sec-WebSocket-Accept
            size_t      consumed = 0;       // bytes consumed from the buffer
        };

        // Attempt to parse a WebSocket upgrade request from the front of `buffer`.
        // Returns complete=false if the header block (terminated by CRLFCRLF) is
        // not fully present yet. When complete && valid, acceptKey is filled with
        // the base64 Sec-WebSocket-Accept value to send back.
        Handshake ParseHttpHandshake(const std::vector<uint8_t>& buffer);

        // Build the HTTP 101 Switching Protocols response for the given accept key.
        std::string BuildHandshakeResponse(const std::string& acceptKey);

        // ── Client side handshake ───────────────────────────────────────

        struct ClientHandshake
        {
            std::string request;         // full HTTP GET Upgrade request to send
            std::string expectedAccept;  // Sec-WebSocket-Accept we expect back
        };

        // Build a WebSocket client upgrade request for ws://host:port<path>. A fresh
        // random Sec-WebSocket-Key is generated; expectedAccept is the value the
        // server must return in Sec-WebSocket-Accept for a valid handshake.
        ClientHandshake BuildClientHandshake(const std::string& host, uint16_t port, const std::string& path);

        struct ServerHandshakeResult
        {
            bool   complete = false; // full response header block received
            bool   valid    = false; // it was a 101 with the expected accept key
            size_t consumed = 0;     // bytes consumed from the buffer
        };

        // Parse the server's HTTP response to a client upgrade. When expectedAccept
        // is non-empty it is validated against the Sec-WebSocket-Accept header.
        ServerHandshakeResult ParseServerHandshake(const std::vector<uint8_t>& buffer, const std::string& expectedAccept);

        // ── Framing ─────────────────────────────────────────────────────

        // Frame opcodes we care about.
        enum class Opcode : uint8_t
        {
            Continuation = 0x0,
            Text         = 0x1,
            Binary       = 0x2,
            Close        = 0x8,
            Ping         = 0x9,
            Pong         = 0xA,
            Incomplete   = 0xFF // internal: not enough bytes yet
        };

        // Maximum payload we are willing to buffer/decode for a single WebSocket
        // frame. Mirrors MuxProtocol::kMaxFramePayload so a malformed/hostile length
        // field cannot drive an unbounded allocation or unbounded input buffering.
        constexpr uint64_t kMaxFramePayload = 16u * 1024u * 1024u;

        struct Frame
        {
            Opcode               opcode = Opcode::Incomplete;
            bool                 fin    = true;
            std::vector<uint8_t> payload;
            size_t               consumed = 0; // bytes consumed from the buffer
            // Set when the frame is fatally malformed (payload exceeds
            // kMaxFramePayload, or the mask bit does not match what this role
            // requires). The caller must close the connection rather than retry.
            bool                 error = false;
        };

        // Encode an application payload as a single server->client binary frame
        // (FIN=1, opcode=Binary, no mask - per RFC servers must not mask).
        std::vector<uint8_t> EncodeBinary(const uint8_t* data, size_t size);

        // Encode a control frame (Pong/Close) with an optional payload, unmasked
        // (server side).
        std::vector<uint8_t> EncodeControl(Opcode opcode, const uint8_t* data, size_t size);

        // Client-side encoders. Per RFC 6455 every client->server frame MUST be
        // masked with a fresh random 4-byte key; these generate one internally.
        std::vector<uint8_t> EncodeBinaryMasked(const uint8_t* data, size_t size);
        std::vector<uint8_t> EncodeControlMasked(Opcode opcode, const uint8_t* data, size_t size);

        // Try to decode one complete frame from the front of `buffer`. On success
        // fills `out` (including consumed byte count) and returns true; the caller
        // removes `out.consumed` bytes. Returns false when a full frame is not yet
        // buffered.
        //
        // requireMask selects the peer role: a server decoding client frames requires
        // them masked (requireMask=true); a client decoding server frames requires
        // them unmasked (requireMask=false). A frame whose mask bit violates the
        // requirement is flagged as a fatal error.
        bool TryDecodeFrame(const std::vector<uint8_t>& buffer, Frame& out, bool requireMask = true);
    }
}

#endif // QDC_TUNNEL_WEBSOCKET_H
