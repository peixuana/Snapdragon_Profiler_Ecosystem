//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

#include "WebSocket.h"

#include <array>
#include <cstring>
#include <random>

namespace qdc
{
    namespace
    {
        // GUID appended to Sec-WebSocket-Key before SHA-1, per RFC 6455.
        constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        // Fill n bytes with random data. Used for the client Sec-WebSocket-Key and the
        // per-frame masking key. Neither needs to be cryptographically strong: masking
        // exists only so intermediaries treat the payload as opaque, and the key is a
        // nonce echoed straight back in the handshake.
        void FillRandom(uint8_t* p, size_t n)
        {
            static thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, 255);
            for (size_t i = 0; i < n; ++i)
            {
                p[i] = static_cast<uint8_t>(dist(rng));
            }
        }

        // ── SHA-1 ───────────────────────────────────────────────────────
        // Minimal SHA-1 (RFC 3174). Produces a 20-byte digest.
        void Sha1(const uint8_t* data, size_t len, uint8_t out[20])
        {
            uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

            // Pre-processing: padded message length is a multiple of 64 bytes.
            const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
            std::vector<uint8_t> msg(data, data + len);
            msg.push_back(0x80);
            while (msg.size() % 64 != 56)
            {
                msg.push_back(0x00);
            }
            for (int i = 7; i >= 0; --i)
            {
                msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
            }

            auto rol = [](uint32_t v, int b) -> uint32_t { return (v << b) | (v >> (32 - b)); };

            for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
            {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                {
                    w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                           (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                           (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
                }
                for (int i = 16; i < 80; ++i)
                {
                    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }

                uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
                for (int i = 0; i < 80; ++i)
                {
                    uint32_t f, k;
                    if (i < 20)      { f = (b & c) | ((~b) & d);       k = 0x5A827999; }
                    else if (i < 40) { f = b ^ c ^ d;                  k = 0x6ED9EBA1; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d);k = 0x8F1BBCDC; }
                    else             { f = b ^ c ^ d;                  k = 0xCA62C1D6; }

                    uint32_t temp = rol(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = rol(b, 30); b = a; a = temp;
                }
                h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
            }

            uint32_t hs[5] = { h0, h1, h2, h3, h4 };
            for (int i = 0; i < 5; ++i)
            {
                out[i * 4]     = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
                out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
                out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
                out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
            }
        }

        // ── Base64 encode ───────────────────────────────────────────────
        std::string Base64Encode(const uint8_t* data, size_t len)
        {
            static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            size_t i = 0;
            while (i + 3 <= len)
            {
                uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                             (static_cast<uint32_t>(data[i + 1]) << 8) |
                             (static_cast<uint32_t>(data[i + 2]));
                out.push_back(tbl[(n >> 18) & 0x3F]);
                out.push_back(tbl[(n >> 12) & 0x3F]);
                out.push_back(tbl[(n >> 6) & 0x3F]);
                out.push_back(tbl[n & 0x3F]);
                i += 3;
            }
            if (len - i == 1)
            {
                uint32_t n = static_cast<uint32_t>(data[i]) << 16;
                out.push_back(tbl[(n >> 18) & 0x3F]);
                out.push_back(tbl[(n >> 12) & 0x3F]);
                out.push_back('=');
                out.push_back('=');
            }
            else if (len - i == 2)
            {
                uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                             (static_cast<uint32_t>(data[i + 1]) << 8);
                out.push_back(tbl[(n >> 18) & 0x3F]);
                out.push_back(tbl[(n >> 12) & 0x3F]);
                out.push_back(tbl[(n >> 6) & 0x3F]);
                out.push_back('=');
            }
            return out;
        }

        std::string AcceptFor(const std::string& key)
        {
            std::string concat = key + kWsGuid;
            uint8_t digest[20];
            Sha1(reinterpret_cast<const uint8_t*>(concat.data()), concat.size(), digest);
            return Base64Encode(digest, 20);
        }

        // Case-insensitive search for a header line "name:" and return its trimmed value.
        std::string FindHeader(const std::string& headers, const std::string& name)
        {
            // headers is the full request text; search line by line.
            size_t pos = 0;
            const size_t n = headers.size();
            while (pos < n)
            {
                size_t eol = headers.find("\r\n", pos);
                if (eol == std::string::npos)
                {
                    eol = n;
                }
                // Compare the header name case-insensitively up to ':'.
                size_t colon = headers.find(':', pos);
                if (colon != std::string::npos && colon < eol)
                {
                    std::string key = headers.substr(pos, colon - pos);
                    // trim
                    while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                    if (key.size() == name.size())
                    {
                        bool eq = true;
                        for (size_t i = 0; i < key.size(); ++i)
                        {
                            char a = key[i], b = name[i];
                            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                            if (a != b) { eq = false; break; }
                        }
                        if (eq)
                        {
                            std::string val = headers.substr(colon + 1, eol - colon - 1);
                            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
                            while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
                            return val;
                        }
                    }
                }
                if (eol == n) break;
                pos = eol + 2;
            }
            return std::string();
        }

        bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
        {
            if (needle.empty()) return true;
            std::string h = haystack, n = needle;
            for (char& c : h) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            for (char& c : n) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return h.find(n) != std::string::npos;
        }
    }

    namespace WebSocket
    {
        // [SERVER handshake 1/2] Parse the client (host tunnel) HTTP Upgrade request,
        // validate it is a WebSocket upgrade, and compute Sec-WebSocket-Accept.
        Handshake ParseHttpHandshake(const std::vector<uint8_t>& buffer)
        {
            Handshake hs;

            // Look for the end of the header block.
            const std::string text(buffer.begin(), buffer.end());
            size_t headerEnd = text.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                // Not all headers received yet. Guard against unbounded growth.
                if (buffer.size() > 16384)
                {
                    hs.complete = true;
                    hs.valid = false;
                    hs.consumed = buffer.size();
                }
                return hs;
            }

            hs.complete = true;
            hs.consumed = headerEnd + 4;

            std::string headers = text.substr(0, headerEnd + 2); // include trailing CRLF for line parsing

            std::string upgrade = FindHeader(headers, "Upgrade");
            std::string connection = FindHeader(headers, "Connection");
            std::string key = FindHeader(headers, "Sec-WebSocket-Key");

            // Validate this is a WebSocket upgrade request.
            if (!ContainsCaseInsensitive(upgrade, "websocket") ||
                !ContainsCaseInsensitive(connection, "upgrade") ||
                key.empty())
            {
                hs.valid = false;
                return hs;
            }

            // Sec-WebSocket-Accept = base64(sha1(key + GUID)) per RFC 6455.
            hs.acceptKey = AcceptFor(key);
            hs.valid = true;
            return hs;
        }

        // [SERVER handshake 2/2] Build the "HTTP/1.1 101 Switching Protocols" response.
        std::string BuildHandshakeResponse(const std::string& acceptKey)
        {
            std::string resp;
            resp += "HTTP/1.1 101 Switching Protocols\r\n";
            resp += "Upgrade: websocket\r\n";
            resp += "Connection: Upgrade\r\n";
            resp += "Sec-WebSocket-Accept: " + acceptKey + "\r\n";
            resp += "\r\n";
            return resp;
        }

        // [CLIENT handshake 1/2] Build the GET Upgrade request the host sends to the
        // device WebSocket server.
        ClientHandshake BuildClientHandshake(const std::string& host, uint16_t port, const std::string& path)
        {
            uint8_t keyBytes[16];
            FillRandom(keyBytes, sizeof(keyBytes));
            std::string key = Base64Encode(keyBytes, sizeof(keyBytes));

            ClientHandshake out;
            out.expectedAccept = AcceptFor(key);

            std::string p = path.empty() ? "/" : path;
            std::string req;
            req += "GET " + p + " HTTP/1.1\r\n";
            req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
            req += "Upgrade: websocket\r\n";
            req += "Connection: Upgrade\r\n";
            req += "Sec-WebSocket-Key: " + key + "\r\n";
            req += "Sec-WebSocket-Version: 13\r\n";
            req += "\r\n";
            out.request = req;
            return out;
        }

        // [CLIENT handshake 2/2] Parse the server's HTTP response to our upgrade.
        ServerHandshakeResult ParseServerHandshake(const std::vector<uint8_t>& buffer, const std::string& expectedAccept)
        {
            ServerHandshakeResult res;

            const std::string text(buffer.begin(), buffer.end());
            size_t headerEnd = text.find("\r\n\r\n");
            if (headerEnd == std::string::npos)
            {
                if (buffer.size() > 16384)
                {
                    res.complete = true;
                    res.valid = false;
                    res.consumed = buffer.size();
                }
                return res;
            }

            res.complete = true;
            res.consumed = headerEnd + 4;

            std::string headers = text.substr(0, headerEnd + 2);

            // Status line must indicate 101 Switching Protocols.
            size_t firstEol = headers.find("\r\n");
            std::string statusLine = (firstEol == std::string::npos) ? headers : headers.substr(0, firstEol);
            if (statusLine.find(" 101") == std::string::npos)
            {
                res.valid = false;
                return res;
            }

            std::string upgrade = FindHeader(headers, "Upgrade");
            if (!ContainsCaseInsensitive(upgrade, "websocket"))
            {
                res.valid = false;
                return res;
            }

            if (!expectedAccept.empty())
            {
                std::string accept = FindHeader(headers, "Sec-WebSocket-Accept");
                if (accept != expectedAccept)
                {
                    res.valid = false;
                    return res;
                }
            }

            res.valid = true;
            return res;
        }

        static std::vector<uint8_t> EncodeFrame(Opcode opcode, const uint8_t* data, size_t size, bool mask)
        {
            std::vector<uint8_t> f;
            f.push_back(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(opcode))); // FIN=1

            const uint8_t maskBit = mask ? 0x80 : 0x00;
            if (size < 126)
            {
                f.push_back(static_cast<uint8_t>(maskBit | size));
            }
            else if (size <= 0xFFFF)
            {
                f.push_back(static_cast<uint8_t>(maskBit | 126));
                f.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
                f.push_back(static_cast<uint8_t>(size & 0xFF));
            }
            else
            {
                f.push_back(static_cast<uint8_t>(maskBit | 127));
                uint64_t s = static_cast<uint64_t>(size);
                for (int i = 7; i >= 0; --i)
                {
                    f.push_back(static_cast<uint8_t>((s >> (i * 8)) & 0xFF));
                }
            }

            if (mask)
            {
                uint8_t maskKey[4];
                FillRandom(maskKey, sizeof(maskKey));
                f.insert(f.end(), maskKey, maskKey + 4);
                for (size_t i = 0; i < size; ++i)
                {
                    f.push_back(static_cast<uint8_t>(data[i] ^ maskKey[i & 3]));
                }
            }
            else if (data != nullptr && size > 0)
            {
                f.insert(f.end(), data, data + size);
            }
            return f;
        }

        std::vector<uint8_t> EncodeBinary(const uint8_t* data, size_t size)
        {
            return EncodeFrame(Opcode::Binary, data, size, /*mask=*/false);
        }

        std::vector<uint8_t> EncodeControl(Opcode opcode, const uint8_t* data, size_t size)
        {
            return EncodeFrame(opcode, data, size, /*mask=*/false);
        }

        std::vector<uint8_t> EncodeBinaryMasked(const uint8_t* data, size_t size)
        {
            return EncodeFrame(Opcode::Binary, data, size, /*mask=*/true);
        }

        std::vector<uint8_t> EncodeControlMasked(Opcode opcode, const uint8_t* data, size_t size)
        {
            return EncodeFrame(opcode, data, size, /*mask=*/true);
        }

        bool TryDecodeFrame(const std::vector<uint8_t>& buffer, Frame& out, bool requireMask)
        {
            // Need at least the 2-byte header.
            if (buffer.size() < 2)
            {
                return false;
            }

            const uint8_t b0 = buffer[0];
            const uint8_t b1 = buffer[1];

            const bool fin = (b0 & 0x80) != 0;
            const uint8_t opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t payloadLen = b1 & 0x7F;

            size_t pos = 2;
            if (payloadLen == 126)
            {
                if (buffer.size() < pos + 2) return false;
                payloadLen = (static_cast<uint64_t>(buffer[pos]) << 8) | buffer[pos + 1];
                pos += 2;
            }
            else if (payloadLen == 127)
            {
                if (buffer.size() < pos + 8) return false;
                payloadLen = 0;
                for (int i = 0; i < 8; ++i)
                {
                    payloadLen = (payloadLen << 8) | buffer[pos + i];
                }
                pos += 8;
            }

            // RFC 6455 §5.1: a client->server frame MUST be masked; a server->client
            // frame MUST NOT be masked. `requireMask` encodes which peer role we are
            // decoding for; a mismatch is a protocol violation - fail the connection.
            if (masked != requireMask)
            {
                out.error = true;
                return false;
            }

            // Guard against a malformed/hostile length field: refuse to buffer or
            // allocate for an oversized payload. Without this the caller's input
            // buffer would grow without bound waiting for a payload that never
            // fully arrives (memory exhaustion). Fail the connection instead.
            if (payloadLen > kMaxFramePayload)
            {
                out.error = true;
                return false;
            }

            uint8_t maskKey[4] = { 0, 0, 0, 0 };
            if (masked)
            {
                if (buffer.size() < pos + 4) return false;
                for (int i = 0; i < 4; ++i)
                {
                    maskKey[i] = buffer[pos + i];
                }
                pos += 4;
            }

            if (buffer.size() < pos + payloadLen)
            {
                return false; // full payload not yet buffered
            }

            out.fin = fin;
            out.payload.resize(static_cast<size_t>(payloadLen));
            for (uint64_t i = 0; i < payloadLen; ++i)
            {
                uint8_t byte = buffer[pos + static_cast<size_t>(i)];
                if (masked)
                {
                    byte ^= maskKey[i & 3];
                }
                out.payload[static_cast<size_t>(i)] = byte;
            }
            out.consumed = pos + static_cast<size_t>(payloadLen);

            switch (opcode)
            {
            case 0x0: out.opcode = Opcode::Continuation; break;
            case 0x1: out.opcode = Opcode::Text;         break;
            case 0x2: out.opcode = Opcode::Binary;       break;
            case 0x8: out.opcode = Opcode::Close;        break;
            case 0x9: out.opcode = Opcode::Ping;         break;
            case 0xA: out.opcode = Opcode::Pong;         break;
            default:  out.opcode = Opcode::Binary;       break;
            }
            return true;
        }
    }
}
