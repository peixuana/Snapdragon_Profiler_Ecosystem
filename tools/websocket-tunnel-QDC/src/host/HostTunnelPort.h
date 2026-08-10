//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Host-side end of one port mapping: a WebSocket CLIENT that dials into the device
// tunnel's WebSocket server and forwards each multiplexed stream to a local TCP
// service (where SDPCore listens).
//
// This is the C++ / cross-platform (Linux + Windows) replacement for the Java
// HostTunnelPort. It is the mirror image of the device-side TunnelPort: same
// MuxProtocol carried inside WebSocket binary frames, but here we are the client
// (so our frames are masked, and the server's frames are not).
//
// Data flow (per port mapping, e.g. remoteWsPort=8900 forwardPort=6500):
//   - Connect a WebSocket to remoteHost:remoteWsPort (reached via ssh -L /
//     adb-forward, so typically 127.0.0.1:<localPort>).
//   - When the device sends CONNECT:<uuid>, open a TCP connection to
//     forwardHost:forwardPort and remember it under that uuid.
//   - Device data frames [uuid][bytes] are demuxed and written to the matching TCP
//     connection; bytes read back are wrapped as [uuid][bytes] and sent to the
//     device.
//   - DISCONNECT / socket close tears the matching stream down on both ends.
//   - Heartbeats are sent every 500ms so the QDC gateway does not reset the idle
//     forwarded WebSocket stream.

#ifndef QDC_TUNNEL_HOSTTUNNELPORT_H
#define QDC_TUNNEL_HOSTTUNNELPORT_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "MuxProtocol.h"
#include "NetCompat.h"
#include "UuidUtil.h"
#include "WebSocket.h"

namespace qdc
{
    class HostTunnelPort
    {
    public:
        HostTunnelPort(const std::string& remoteHost, uint16_t remoteWsPort,
                       const std::string& forwardHost, uint16_t forwardPort,
                       bool reconnect);
        ~HostTunnelPort();

        HostTunnelPort(const HostTunnelPort&) = delete;
        HostTunnelPort& operator=(const HostTunnelPort&) = delete;

        // Connect/serve loop. Reconnects after a drop (3s) unless reconnect==false or
        // Stop() has been called.
        void Run();

        // Signals the loop to stop (safe to call from a signal handler / other thread).
        void Stop();

        const std::string& Label() const { return m_label; }

    private:
        // A forwarded TCP connection to the local service, keyed by UUID.
        struct ForwardClient
        {
            net::socket_t       fd = net::kInvalidSocket;
            Uuid                uuid{};
            std::deque<uint8_t> pendingWrite; // bytes waiting to be written to this socket
        };

        enum class PollKind { Server, Client };
        struct PollEntry
        {
            PollKind    kind;
            std::string clientKey; // valid only when kind == Client
        };

        // A single connection attempt: connect + handshake, then pump until the server
        // connection drops or Stop() is called.
        void RunOnce();

        // Blocking TCP connect + WebSocket client handshake. Returns true and sets
        // m_serverFd (non-blocking) on success.
        bool ConnectAndHandshake();

        void CloseServer();
        void DropAllClients();

        void OnServerReadable(bool& shouldClose);
        void FlushServerWrite();
        // Wrap a MuxProtocol frame in a masked WebSocket binary frame and queue it.
        void QueueServerFrame(std::vector<uint8_t>&& muxFrame);
        void MaybeSendHeartbeat();

        void HandleControlFrame(const std::string& text);
        void HandleDataFrame(const std::vector<uint8_t>& payload);

        void OpenForwardClient(const std::string& hex);
        void OnClientReadable(ForwardClient& client, bool& shouldClose);
        void OnClientWritable(ForwardClient& client);
        void CloseForwardClient(const std::string& hex, bool notifyServer);

        const std::string m_remoteHost;
        const uint16_t    m_remoteWsPort;
        const std::string m_forwardHost;
        const uint16_t    m_forwardPort;
        const bool        m_reconnect;
        const std::string m_label;

        net::socket_t m_serverFd = net::kInvalidSocket;
        long long     m_lastHeartbeatMs = 0;

        std::vector<uint8_t> m_wsInBuf;         // raw bytes from server (WS frames)
        std::vector<uint8_t> m_wsAppBuf;        // reassembled WS payload (fragmentation)
        std::vector<uint8_t> m_muxReadBuf;      // partial MuxProtocol frames from server
        std::deque<uint8_t>  m_serverWriteBuf;  // bytes pending to server (WS frames)

        std::map<std::string, ForwardClient> m_clients; // key: uuid hex

        std::atomic<bool> m_running{ true };
    };
}

#endif // QDC_TUNNEL_HOSTTUNNELPORT_H
