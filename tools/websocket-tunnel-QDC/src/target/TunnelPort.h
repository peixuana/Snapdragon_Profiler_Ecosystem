//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// One independent tunnel for a single port mapping: a local TCP listener plus a
// WebSocket server socket that the SDP host connects into. Multiple concurrent TCP
// client streams are multiplexed over the single host connection using MuxProtocol.
//
// Equivalent to the Java TargetTunnelPort, but implemented with raw POSIX
// sockets and a single poll() event loop (no threads-per-connection). Runs on the
// device (Android or Qualcomm Linux / IoT).
//
// Data flow (per port mapping, e.g. tcpPort=6500 wsPort=8900):
//   - The device starts a WebSocket server listening on wsPort (8900). The host
//     tunnel client connects into it (via adb-forward + ssh -L, or ssh -L for IoT).
//   - The device also listens on TCP tcpPort (6500). Device-local TCP clients
//     connect there.
//   - When a local TCP client connects, the device assigns it a UUID and sends
//     CONNECT:<uuid> to the host over the WebSocket, then forwards its bytes as
//     [UUID][payload] MuxProtocol data frames inside WebSocket binary frames.
//   - Data frames from the host are demuxed by UUID and written back to the
//     matching device-local TCP client.
//   - On TCP or WebSocket close, streams are torn down (DISCONNECT / drop all TCP
//     clients).

#ifndef QDC_TUNNEL_TUNNELPORT_H
#define QDC_TUNNEL_TUNNELPORT_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "MuxProtocol.h"
#include "UuidUtil.h"
#include "WebSocket.h"

namespace qdc
{
    class TunnelPort
    {
    public:
        // tcpPort: device-local port the profiler clients connect to (e.g. 6500).
        // wsPort : port the SDP host connects into (e.g. 8900), reached via adb-forward.
        TunnelPort(const std::string& bindHost, uint16_t tcpPort, uint16_t wsPort);
        ~TunnelPort();

        TunnelPort(const TunnelPort&) = delete;
        TunnelPort& operator=(const TunnelPort&) = delete;

        // Runs the blocking poll() loop until Stop() is called or a fatal error occurs.
        // Returns false if the listeners could not be set up.
        bool Run();

        // Signals the loop to stop (safe to call from a signal handler / other thread).
        void Stop();

        const std::string& Label() const { return m_label; }

    private:
        // A device-local TCP client stream, keyed by UUID.
        struct TcpClient
        {
            int                   fd = -1;
            Uuid                  uuid{};
            std::deque<uint8_t>   pendingWrite; // bytes waiting to be written to this socket
            // True once the host has been told about this stream via CONNECT. When a
            // client is accepted before the WebSocket handshake completes, the CONNECT
            // is deferred (it would be dropped) and sent once the handshake finishes.
            bool                  connectSent = false;
        };

        // Identifies what a pollfd slot in the event loop refers to, so revents are
        // always attributed to the correct fd.
        enum class PollKind { TcpListener, WsListener, Host, Client };
        struct PollEntry
        {
            PollKind    kind;
            std::string clientKey; // valid only when kind == Client
        };

        bool SetupListeners();
        void CloseAll();

        int  AcceptTcpClient();
        void AcceptHostConnection();

        void OnTcpClientReadable(TcpClient& client, bool& shouldClose);
        void OnTcpClientWritable(TcpClient& client);
        void CloseTcpClient(const Uuid& uuid, bool notifyHost);

        void OnHostReadable(bool& shouldClose);
        void FlushHostWrite();
        void QueueHostFrame(std::vector<uint8_t>&& frame);
        // Send a Heartbeat frame to the host if one is due, to keep the QDC-forwarded
        // stream from being reset for idleness (~1s). Called each poll tick.
        void MaybeSendHeartbeat();
        void HandleControlFrame(const std::string& text);
        void HandleDataFrame(const std::vector<uint8_t>& payload);
        void DropAllTcpClients();

        // After the WebSocket handshake completes, send CONNECT for any clients that
        // were accepted while the handshake was still pending (their CONNECT would
        // otherwise have been silently dropped by QueueHostFrame).
        void SendDeferredConnects();

        // Tear down the current host connection and everything bound to it.
        void CloseHost();

        static int  MakeListener(const std::string& host, uint16_t port);
        static bool SetNonBlocking(int fd);

        const std::string m_bindHost;
        const uint16_t    m_tcpPort;
        const uint16_t    m_wsPort;
        const std::string m_label;

        int m_tcpListenFd = -1;
        int m_wsListenFd  = -1;
        int m_hostFd      = -1; // single active host connection

        // Monotonic timestamp (ms) of the last heartbeat sent to the host. Used to
        // send a Heartbeat frame at a fixed cadence so the QDC gateway does not reset
        // the forwarded ws stream for being idle.
        long long m_lastHeartbeatMs = 0;

        // WebSocket handshake state for the host connection. Until the HTTP Upgrade
        // completes, incoming bytes are the HTTP request; afterwards they are
        // WebSocket frames whose payload is the MuxProtocol byte stream.
        bool                 m_hostWsHandshakeDone = false;
        std::vector<uint8_t> m_hostWsInBuf;   // raw bytes from host (handshake, then WS frames)
        std::vector<uint8_t> m_hostWsAppBuf;  // reassembled WS payload (fragmentation)

        std::map<std::string, TcpClient> m_clients;      // key: uuid hex
        std::deque<uint8_t>              m_hostWriteBuf;  // bytes pending to host (WS frames)
        std::vector<uint8_t>             m_hostReadBuf;   // partial MuxProtocol frames from host

        std::atomic<bool> m_running{ true };
    };
}

#endif // QDC_TUNNEL_TUNNELPORT_H
