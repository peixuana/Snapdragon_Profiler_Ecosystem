//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

#include "TunnelPort.h"
#include "Logging.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace qdc
{
    namespace
    {
        constexpr size_t kReadChunk = 65536;

        // Heartbeat cadence. The QDC jump host resets forwarded streams that carry no
        // application data for ~1s, which prevents the host WebSocket connection from
        // ever stabilising (connect -> idle -> reset -> reconnect). Sending a small
        // Heartbeat frame well under that interval keeps the ws stream "busy" so the
        // gateway does not reset it. 500ms is comfortably below the ~1s reset window.
        constexpr long long kHeartbeatIntervalMs = 500;

        long long NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // High-watermark for bytes pending to the host. When m_hostWriteBuf grows
        // beyond this (host send blocked / slow), we stop reading from local TCP
        // clients (drop POLLIN on them) so their kernel receive windows fill and the
        // peers throttle - i.e. apply backpressure instead of buffering without bound.
        constexpr size_t kHostWriteHighWatermark = 8u * 1024u * 1024u;

        void CloseFd(int& fd)
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }
    }

    TunnelPort::TunnelPort(const std::string& bindHost, uint16_t tcpPort, uint16_t wsPort) :
        m_bindHost(bindHost),
        m_tcpPort(tcpPort),
        m_wsPort(wsPort),
        m_label("tcp:" + std::to_string(tcpPort) + " ws:" + std::to_string(wsPort))
    {
    }

    TunnelPort::~TunnelPort()
    {
        CloseAll();
    }

    void TunnelPort::Stop()
    {
        m_running.store(false);
    }

    bool TunnelPort::SetNonBlocking(int fd)
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
        {
            return false;
        }
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    int TunnelPort::MakeListener(const std::string& host, uint16_t port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            QDC_LOGE("socket() failed for port %u: %s", port, std::strerror(errno));
            return -1;
        }

        // NOTE: deliberately NOT setting SO_REUSEADDR here. If a previous qdc-tunnel
        // instance is still running (e.g. a second connect flow launched a duplicate
        // before the first exited), SO_REUSEADDR would let this instance co-bind the
        // same port. The kernel then load-balances incoming host connections across
        // both processes, so a host's WebSocket HTTP Upgrade and its subsequent frames
        // can land on different processes - the handshake state never matches and the
        // connection is torn down within milliseconds (the connect/close/reconnect
        // storm). Without SO_REUSEADDR the duplicate instance fails to bind and exits,
        // leaving exactly one process owning the ports.

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (host.empty() || host == "0.0.0.0")
        {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            QDC_LOGE("Invalid bind host '%s' for port %u", host.c_str(), port);
            CloseFd(fd);
            return -1;
        }

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            QDC_LOGE("bind() failed for port %u: %s", port, std::strerror(errno));
            CloseFd(fd);
            return -1;
        }

        if (::listen(fd, 16) != 0)
        {
            QDC_LOGE("listen() failed for port %u: %s", port, std::strerror(errno));
            CloseFd(fd);
            return -1;
        }

        if (!SetNonBlocking(fd))
        {
            QDC_LOGE("failed to set non-blocking for listener on port %u", port);
            CloseFd(fd);
            return -1;
        }

        return fd;
    }

    bool TunnelPort::SetupListeners()
    {
        m_tcpListenFd = MakeListener(m_bindHost, m_tcpPort);
        if (m_tcpListenFd < 0)
        {
            return false;
        }

        // The device-side WebSocket server's listening socket is created here: the
        // device listens on wsPort (8900/8902); the host tunnel client connects in
        // later. (MakeListener does socket -> bind -> listen.)
        m_wsListenFd = MakeListener(m_bindHost, m_wsPort);
        if (m_wsListenFd < 0)
        {
            CloseFd(m_tcpListenFd);
            return false;
        }

        QDC_LOGI("[%s] listening (tcp for local clients, ws for host)", m_label.c_str());
        return true;
    }

    void TunnelPort::CloseAll()
    {
        DropAllTcpClients();
        CloseFd(m_hostFd);
        CloseFd(m_tcpListenFd);
        CloseFd(m_wsListenFd);
        m_hostWriteBuf.clear();
        m_hostReadBuf.clear();
    }

    void TunnelPort::DropAllTcpClients()
    {
        for (auto& kv : m_clients)
        {
            CloseFd(kv.second.fd);
        }
        m_clients.clear();
    }

    // Tear down the current host connection and all state bound to it. Centralised so
    // every host-teardown path (send error, POLLHUP, WS close, decode error) is
    // consistent.
    void TunnelPort::CloseHost()
    {
        CloseFd(m_hostFd);
        DropAllTcpClients();
        m_hostWriteBuf.clear();
        m_hostReadBuf.clear();
        m_hostWsInBuf.clear();
        m_hostWsAppBuf.clear();
        m_hostWsHandshakeDone = false;
    }

    // ── Host connection ─────────────────────────────────────────────────

    // accept() the host (WebSocket client) connecting into the device WebSocket
    // server port. This only completes the TCP accept; the WebSocket protocol
    // handshake happens in OnHostReadable() Phase 1.
    void TunnelPort::AcceptHostConnection()
    {
        int fd = ::accept(m_wsListenFd, nullptr, nullptr);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                QDC_LOGW("[%s] accept(host) failed: %s", m_label.c_str(), std::strerror(errno));
            }
            return;
        }

        // If we already have a host connection, REPLACE it with the new one, mirroring
        // the Java reference server (TargetTunnelPort.onOpen: close old, accept new).
        // The host side is a single connector per ws port that reconnects after any
        // drop; when it reconnects, the device may still be holding the stale fd of the
        // previous (already dead) connection. Rejecting the new one (the previous
        // behaviour) left the device wedged on a dead fd while the live host connection
        // was refused - producing the endless connect/close/reconnect loop. Dropping
        // the old fd and taking the new one lets the fresh connection through.
        if (m_hostFd >= 0)
        {
            QDC_LOGW("[%s] host connection already active; replacing with new connection", m_label.c_str());
            CloseHost();
        }

        SetNonBlocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        m_hostFd = fd;
        // Reset WebSocket handshake state; the host must complete the HTTP Upgrade
        // before any MuxProtocol frames flow (mirrors the Java WebSocket server).
        m_hostWsHandshakeDone = false;
        m_hostWsInBuf.clear();
        m_hostWsAppBuf.clear();
        QDC_LOGI("[%s] host connected (awaiting WebSocket handshake)", m_label.c_str());
    }

    // Send a Heartbeat frame if the interval has elapsed. Keeps the QDC-forwarded ws
    // stream from being reset for idleness. Only meaningful once the host is connected
    // and the WebSocket handshake has completed.
    void TunnelPort::MaybeSendHeartbeat()
    {
        if (m_hostFd < 0 || !m_hostWsHandshakeDone)
        {
            return;
        }
        const long long now = NowMs();
        if (now - m_lastHeartbeatMs < kHeartbeatIntervalMs)
        {
            return;
        }
        m_lastHeartbeatMs = now;
        QueueHostFrame(MuxProtocol::EncodeHeartbeat());
        FlushHostWrite();
    }

    void TunnelPort::QueueHostFrame(std::vector<uint8_t>&& frame)
    {
        if (m_hostFd < 0 || !m_hostWsHandshakeDone)
        {
            return; // no host connected / not yet upgraded; drop
        }
        // After the handshake, everything sent to the host is wrapped in a WebSocket
        // binary frame (server frames are not masked). The matching decode happens in
        // OnHostReadable() Phase 2 via WebSocket::TryDecodeFrame.
        std::vector<uint8_t> ws = WebSocket::EncodeBinary(frame.data(), frame.size());
        m_hostWriteBuf.insert(m_hostWriteBuf.end(), ws.begin(), ws.end());
    }

    void TunnelPort::FlushHostWrite()
    {
        while (m_hostFd >= 0 && !m_hostWriteBuf.empty())
        {
            // deque is not contiguous; copy a chunk out to write.
            const size_t chunk = std::min(m_hostWriteBuf.size(), kReadChunk);
            std::vector<uint8_t> tmp(m_hostWriteBuf.begin(), m_hostWriteBuf.begin() + chunk);
            ssize_t n = ::send(m_hostFd, tmp.data(), tmp.size(), 0);
            if (n > 0)
            {
                m_hostWriteBuf.erase(m_hostWriteBuf.begin(), m_hostWriteBuf.begin() + n);
            }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break; // try again when writable
            }
            else
            {
                QDC_LOGW("[%s] host send failed: %s", m_label.c_str(), std::strerror(errno));
                CloseHost();
                break;
            }
        }
    }

    // Send CONNECT for any streams that were accepted before the WebSocket handshake
    // completed (their CONNECT would have been dropped by QueueHostFrame). Without
    // this the host never learns about those streams and they hang forever.
    void TunnelPort::SendDeferredConnects()
    {
        for (auto& kv : m_clients)
        {
            if (!kv.second.connectSent)
            {
                QueueHostFrame(MuxProtocol::EncodeControl("CONNECT:" + kv.first));
                kv.second.connectSent = true;
                QDC_LOGI("[%s][%.8s] sent deferred CONNECT after handshake", m_label.c_str(), kv.first.c_str());
            }
        }
        FlushHostWrite();
    }

    void TunnelPort::OnHostReadable(bool& shouldClose)
    {
        uint8_t buf[kReadChunk];
        while (true)
        {
            ssize_t n = ::recv(m_hostFd, buf, sizeof(buf), 0);
            if (n > 0)
            {
                m_hostWsInBuf.insert(m_hostWsInBuf.end(), buf, buf + n);
            }
            else if (n == 0)
            {
                QDC_LOGI("[%s] host disconnected", m_label.c_str());
                shouldClose = true;
                return;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                QDC_LOGW("[%s] host recv failed: %s", m_label.c_str(), std::strerror(errno));
                shouldClose = true;
                return;
            }
        }

        // Phase 1: complete the WebSocket HTTP Upgrade handshake. As the WebSocket
        // server, parse the client Upgrade request and reply 101 Switching Protocols;
        // once that succeeds this connection is a live WebSocket.
        if (!m_hostWsHandshakeDone)
        {
            WebSocket::Handshake hs = WebSocket::ParseHttpHandshake(m_hostWsInBuf);
            if (!hs.complete)
            {
                return; // wait for the rest of the request headers
            }
            if (!hs.valid)
            {
                QDC_LOGW("[%s] invalid WebSocket handshake; closing host connection", m_label.c_str());
                shouldClose = true;
                return;
            }
            std::string resp = WebSocket::BuildHandshakeResponse(hs.acceptKey);
            m_hostWriteBuf.insert(m_hostWriteBuf.end(), resp.begin(), resp.end());
            m_hostWsInBuf.erase(m_hostWsInBuf.begin(), m_hostWsInBuf.begin() + hs.consumed);
            m_hostWsHandshakeDone = true;   // WebSocket established; frame-based from here on
            FlushHostWrite();
            QDC_LOGI("[%s] WebSocket handshake complete", m_label.c_str());
            // Send a heartbeat immediately after the handshake so the freshly
            // established (and initially idle) forwarded stream is not reset by the
            // QDC gateway before any real data flows.
            m_lastHeartbeatMs = 0;
            MaybeSendHeartbeat();
            // Now that we can actually queue frames, tell the host about any streams
            // that connected during the handshake window (whose CONNECT was deferred).
            SendDeferredConnects();
            // fall through to decode any WS frames that arrived after the request.
        }

        // Phase 2: decode WebSocket frames; their payload is the MuxProtocol stream.
        WebSocket::Frame wsFrame;
        while (WebSocket::TryDecodeFrame(m_hostWsInBuf, wsFrame))
        {
            m_hostWsInBuf.erase(m_hostWsInBuf.begin(), m_hostWsInBuf.begin() + wsFrame.consumed);

            switch (wsFrame.opcode)
            {
            case WebSocket::Opcode::Close:
                QDC_LOGI("[%s] host sent WebSocket close", m_label.c_str());
                shouldClose = true;
                return;
            case WebSocket::Opcode::Ping:
            {
                // Reply with a Pong echoing the payload.
                std::vector<uint8_t> pong = WebSocket::EncodeControl(
                    WebSocket::Opcode::Pong, wsFrame.payload.data(), wsFrame.payload.size());
                m_hostWriteBuf.insert(m_hostWriteBuf.end(), pong.begin(), pong.end());
                FlushHostWrite();
                continue;
            }
            case WebSocket::Opcode::Pong:
                continue; // ignore
            default:
                break; // Binary / Text / Continuation carry app data
            }

            // Accumulate (handles fragmentation across continuation frames).
            m_hostWsAppBuf.insert(m_hostWsAppBuf.end(), wsFrame.payload.begin(), wsFrame.payload.end());
            if (!wsFrame.fin)
            {
                continue; // wait for the final fragment
            }

            // Feed the reassembled application bytes to the MuxProtocol decoder.
            m_hostReadBuf.insert(m_hostReadBuf.end(), m_hostWsAppBuf.begin(), m_hostWsAppBuf.end());
            m_hostWsAppBuf.clear();

            DecodedFrame frame;
            bool muxFatal = false;
            while (MuxProtocol::TryDecode(m_hostReadBuf, frame, &muxFatal))
            {
                if (frame.type == FrameType::Control)
                {
                    HandleControlFrame(std::string(frame.payload.begin(), frame.payload.end()));
                }
                else if (frame.type == FrameType::Heartbeat)
                {
                    // Keepalive only - nothing to do.
                }
                else
                {
                    HandleDataFrame(frame.payload);
                }
            }
            // A corrupt MuxProtocol length means the framing is no longer trustworthy;
            // close the connection rather than silently resyncing forever on garbage.
            if (muxFatal)
            {
                QDC_LOGW("[%s] corrupt MuxProtocol frame length; closing host connection", m_label.c_str());
                shouldClose = true;
                return;
            }
        }

        // A fatally malformed WebSocket frame (oversized payload, or a client frame
        // that was not masked as RFC 6455 requires) is unrecoverable: fail the
        // connection instead of leaving the un-decodable bytes to grow m_hostWsInBuf.
        if (wsFrame.error)
        {
            QDC_LOGW("[%s] malformed WebSocket frame from host; closing connection", m_label.c_str());
            shouldClose = true;
            return;
        }
    }

    void TunnelPort::HandleControlFrame(const std::string& text)
    {
        // Host tells us a stream was closed on its side.
        static const std::string kDisconnect = "DISCONNECT:";
        if (text.compare(0, kDisconnect.size(), kDisconnect) == 0)
        {
            std::string hex = text.substr(kDisconnect.size());
            Uuid uuid{};
            if (UuidUtil::FromHex(hex, uuid))
            {
                CloseTcpClient(uuid, false);
            }
        }
        else
        {
            QDC_LOGV("[%s] unknown control frame: %.*s", m_label.c_str(),
                     static_cast<int>(text.size() > 60 ? 60 : text.size()), text.c_str());
        }
    }

    void TunnelPort::HandleDataFrame(const std::vector<uint8_t>& payload)
    {
        Uuid uuid{};
        const uint8_t* data = nullptr;
        size_t dataSize = 0;
        if (!MuxProtocol::SplitData(payload, uuid, data, dataSize))
        {
            return;
        }

        std::string key = UuidUtil::ToHex(uuid);
        auto it = m_clients.find(key);
        if (it == m_clients.end())
        {
            QDC_LOGV("[%s] data for unknown stream %.8s", m_label.c_str(), key.c_str());
            return;
        }

        // Queue for write to the device-local client; actual write happens when writable.
        it->second.pendingWrite.insert(it->second.pendingWrite.end(), data, data + dataSize);
        OnTcpClientWritable(it->second);
    }

    // ── Device-local TCP clients (device listens on tcpPort, accepts here) ──

    int TunnelPort::AcceptTcpClient()
    {
        int fd = ::accept(m_tcpListenFd, nullptr, nullptr);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                QDC_LOGW("[%s] accept(tcp) failed: %s", m_label.c_str(), std::strerror(errno));
            }
            return -1;
        }

        SetNonBlocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (m_hostFd < 0)
        {
            // No host tunnel yet; we cannot forward this client's traffic.
            QDC_LOGW("[%s] rejecting local client: no host connection", m_label.c_str());
            ::close(fd);
            return -1;
        }

        TcpClient client;
        client.fd = fd;
        client.uuid = UuidUtil::RandomBytes();
        std::string key = UuidUtil::ToHex(client.uuid);

        // Notify host of the new stream - but only if the WebSocket handshake is
        // already complete. If it is not, QueueHostFrame would silently drop the
        // CONNECT (frames cannot flow before the upgrade), orphaning the stream so
        // the host never learns of it and this client hangs forever. Defer the
        // CONNECT (connectSent = false) and send it from SendDeferredConnects() once
        // the handshake finishes.
        if (m_hostWsHandshakeDone)
        {
            QueueHostFrame(MuxProtocol::EncodeControl("CONNECT:" + key));
            FlushHostWrite();
            client.connectSent = true;
        }
        else
        {
            QDC_LOGI("[%s][%.8s] local client connected before WS handshake; deferring CONNECT",
                     m_label.c_str(), key.c_str());
            client.connectSent = false;
        }

        m_clients[key] = client;

        QDC_LOGI("[%s][%.8s] local client connected", m_label.c_str(), key.c_str());
        return fd;
    }

    void TunnelPort::OnTcpClientReadable(TcpClient& client, bool& shouldClose)
    {
        uint8_t buf[kReadChunk];
        while (true)
        {
            ssize_t n = ::recv(client.fd, buf, sizeof(buf), 0);
            if (n > 0)
            {
                // Forward as a data frame to the host.
                QueueHostFrame(MuxProtocol::EncodeData(client.uuid, buf, static_cast<size_t>(n)));

                // Backpressure: if the host write buffer has grown past the
                // high-watermark (host send is blocked/slow), stop draining this
                // socket now.
                if (m_hostWriteBuf.size() >= kHostWriteHighWatermark)
                {
                    break;
                }
            }
            else if (n == 0)
            {
                shouldClose = true;
                break;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                shouldClose = true;
                break;
            }
        }
        FlushHostWrite();
    }

    void TunnelPort::OnTcpClientWritable(TcpClient& client)
    {
        while (client.fd >= 0 && !client.pendingWrite.empty())
        {
            const size_t chunk = std::min(client.pendingWrite.size(), kReadChunk);
            std::vector<uint8_t> tmp(client.pendingWrite.begin(), client.pendingWrite.begin() + chunk);
            ssize_t n = ::send(client.fd, tmp.data(), tmp.size(), 0);
            if (n > 0)
            {
                client.pendingWrite.erase(client.pendingWrite.begin(), client.pendingWrite.begin() + n);
            }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break; // retry when writable
            }
            else
            {
                // Write error: close this stream and tell the host.
                CloseTcpClient(client.uuid, true);
                return;
            }
        }
    }

    void TunnelPort::CloseTcpClient(const Uuid& uuid, bool notifyHost)
    {
        std::string key = UuidUtil::ToHex(uuid);
        auto it = m_clients.find(key);
        if (it == m_clients.end())
        {
            return;
        }

        CloseFd(it->second.fd);
        m_clients.erase(it);

        if (notifyHost)
        {
            QueueHostFrame(MuxProtocol::EncodeControl("DISCONNECT:" + key));
            FlushHostWrite();
        }

        QDC_LOGI("[%s][%.8s] local client closed", m_label.c_str(), key.c_str());
    }

    // ── Event loop ──────────────────────────────────────────────────────

    bool TunnelPort::Run()
    {
        if (!SetupListeners())
        {
            return false;
        }

        while (m_running.load())
        {
            std::vector<pollfd>    fds;
            std::vector<PollEntry> entries;

            fds.push_back(pollfd{ m_tcpListenFd, POLLIN, 0 });
            entries.push_back(PollEntry{ PollKind::TcpListener, {} });

            fds.push_back(pollfd{ m_wsListenFd, POLLIN, 0 });
            entries.push_back(PollEntry{ PollKind::WsListener, {} });

            if (m_hostFd >= 0)
            {
                short ev = POLLIN;
                if (!m_hostWriteBuf.empty())
                {
                    ev |= POLLOUT;
                }
                fds.push_back(pollfd{ m_hostFd, ev, 0 });
                entries.push_back(PollEntry{ PollKind::Host, {} });
            }

            // Backpressure: only arm POLLIN on local clients while the host write
            // buffer is below the high-watermark.
            const bool acceptClientReads = m_hostWriteBuf.size() < kHostWriteHighWatermark;
            for (auto& kv : m_clients)
            {
                short ev = 0;
                if (acceptClientReads)
                {
                    ev |= POLLIN;
                }
                if (!kv.second.pendingWrite.empty())
                {
                    ev |= POLLOUT;
                }
                fds.push_back(pollfd{ kv.second.fd, ev, 0 });
                entries.push_back(PollEntry{ PollKind::Client, kv.first });
            }

            // Poll with a short timeout so the heartbeat cadence (500ms) is honoured
            // even when there is no socket activity. The QDC gateway resets idle
            // forwarded streams within ~1s, so we must keep sending heartbeats.
            int rc = ::poll(fds.data(), fds.size(), 250 /* ms */);
            if (rc < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                QDC_LOGE("[%s] poll() failed: %s", m_label.c_str(), std::strerror(errno));
                break;
            }
            if (rc == 0)
            {
                // Timeout: no socket activity. Still send a heartbeat if one is due so
                // the idle forwarded ws stream is not reset by the QDC gateway.
                MaybeSendHeartbeat();
                continue; // re-evaluate m_running
            }

            for (size_t i = 0; i < fds.size(); ++i)
            {
                const short revents = fds[i].revents;
                if (revents == 0)
                {
                    continue;
                }

                switch (entries[i].kind)
                {
                case PollKind::TcpListener:
                    if (revents & POLLIN)
                    {
                        AcceptTcpClient();
                    }
                    break;

                case PollKind::WsListener:
                    if (revents & POLLIN)
                    {
                        AcceptHostConnection();
                    }
                    break;

                case PollKind::Host:
                {
                    if (m_hostFd < 0 || m_hostFd != fds[i].fd)
                    {
                        break;
                    }
                    if (revents & (POLLERR | POLLHUP | POLLNVAL))
                    {
                        CloseHost();
                        break;
                    }
                    if (revents & POLLOUT)
                    {
                        FlushHostWrite();
                    }
                    if (m_hostFd < 0 || m_hostFd != fds[i].fd)
                    {
                        break;
                    }
                    if (revents & POLLIN)
                    {
                        bool shouldClose = false;
                        OnHostReadable(shouldClose);
                        if (shouldClose)
                        {
                            CloseHost();
                        }
                    }
                    break;
                }

                case PollKind::Client:
                {
                    auto it = m_clients.find(entries[i].clientKey);
                    if (it == m_clients.end() || it->second.fd != fds[i].fd)
                    {
                        break; // closed or replaced during this pass
                    }

                    if (revents & (POLLERR | POLLHUP | POLLNVAL))
                    {
                        CloseTcpClient(it->second.uuid, true);
                        break;
                    }
                    if (revents & POLLOUT)
                    {
                        OnTcpClientWritable(it->second);
                    }
                    it = m_clients.find(entries[i].clientKey);
                    if (it == m_clients.end() || it->second.fd != fds[i].fd)
                    {
                        break;
                    }
                    if (revents & POLLIN)
                    {
                        bool shouldClose = false;
                        OnTcpClientReadable(it->second, shouldClose);
                        if (shouldClose)
                        {
                            CloseTcpClient(it->second.uuid, true);
                        }
                    }
                    break;
                }
                }
            }

            // After servicing socket events, send a heartbeat if the cadence is due.
            // Combined with the timeout path above, this keeps the forwarded ws stream
            // continuously non-idle so the QDC gateway does not reset it.
            MaybeSendHeartbeat();
        }

        CloseAll();
        QDC_LOGI("[%s] tunnel stopped", m_label.c_str());
        return true;
    }
}
