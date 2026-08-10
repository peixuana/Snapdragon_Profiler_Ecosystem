//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

#include "HostTunnelPort.h"
#include "Logging.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace qdc
{
    namespace
    {
        constexpr size_t kReadChunk = 65536;

        // Heartbeat cadence (see TunnelPort.cpp). 500ms is comfortably below the QDC
        // gateway's ~1s idle reset window.
        constexpr long long kHeartbeatIntervalMs = 500;

        // Delay between reconnect attempts (matches the Java host's RECONNECT_DELAY_MS).
        constexpr int kReconnectDelayMs = 3000;

        // Backpressure high-watermark for bytes pending to the server (see TunnelPort).
        constexpr size_t kServerWriteHighWatermark = 8u * 1024u * 1024u;

        long long NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    HostTunnelPort::HostTunnelPort(const std::string& remoteHost, uint16_t remoteWsPort,
                                   const std::string& forwardHost, uint16_t forwardPort,
                                   bool reconnect) :
        m_remoteHost(remoteHost),
        m_remoteWsPort(remoteWsPort),
        m_forwardHost(forwardHost),
        m_forwardPort(forwardPort),
        m_reconnect(reconnect),
        m_label("ws:" + std::to_string(remoteWsPort) + " fwd:" + std::to_string(forwardPort))
    {
    }

    HostTunnelPort::~HostTunnelPort()
    {
        CloseServer();
    }

    void HostTunnelPort::Stop()
    {
        m_running.store(false);
    }

    void HostTunnelPort::Run()
    {
        while (m_running.load())
        {
            RunOnce();

            if (!m_running.load() || !m_reconnect)
            {
                break;
            }

            QDC_LOGI("[%s] reconnecting in %dms", m_label.c_str(), kReconnectDelayMs);
            for (int elapsed = 0; elapsed < kReconnectDelayMs && m_running.load(); elapsed += 100)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        CloseServer();
        QDC_LOGI("[%s] host tunnel stopped", m_label.c_str());
    }

    // ── Connect + WebSocket client handshake ────────────────────────────

    bool HostTunnelPort::ConnectAndHandshake()
    {
        // nosemgrep: javascript.lang.security.detect-insecure-websocket.detect-insecure-websocket
        QDC_LOGI("[%s] connecting to ws://%s:%u", m_label.c_str(), m_remoteHost.c_str(), m_remoteWsPort);

        m_serverFd = net::TcpConnect(m_remoteHost, m_remoteWsPort);
        if (m_serverFd == net::kInvalidSocket)
        {
            QDC_LOGW("[%s] connect to %s:%u failed", m_label.c_str(), m_remoteHost.c_str(), m_remoteWsPort);
            return false;
        }

        // Socket is still blocking here; send the upgrade request and read the response
        // synchronously before switching to the non-blocking event loop.
        WebSocket::ClientHandshake ch = WebSocket::BuildClientHandshake(m_remoteHost, m_remoteWsPort, "/");

        size_t sent = 0;
        while (sent < ch.request.size())
        {
            bool wouldBlock = false;
            long n = net::Send(m_serverFd, ch.request.data() + sent, ch.request.size() - sent, &wouldBlock);
            if (n > 0)
            {
                sent += static_cast<size_t>(n);
            }
            else if (n < 0 && wouldBlock)
            {
                continue; // blocking socket: transient, retry
            }
            else
            {
                QDC_LOGW("[%s] failed to send WebSocket handshake", m_label.c_str());
                CloseServer();
                return false;
            }
        }

        m_wsInBuf.clear();
        uint8_t buf[4096];
        while (true)
        {
            bool wouldBlock = false;
            long n = net::Recv(m_serverFd, buf, sizeof(buf), &wouldBlock);
            if (n > 0)
            {
                m_wsInBuf.insert(m_wsInBuf.end(), buf, buf + n);
            }
            else if (n < 0 && wouldBlock)
            {
                continue; // blocking socket: retry
            }
            else
            {
                QDC_LOGW("[%s] server closed during handshake", m_label.c_str());
                CloseServer();
                return false;
            }

            WebSocket::ServerHandshakeResult res = WebSocket::ParseServerHandshake(m_wsInBuf, ch.expectedAccept);
            if (!res.complete)
            {
                continue; // wait for the rest of the response headers
            }
            if (!res.valid)
            {
                QDC_LOGW("[%s] invalid WebSocket handshake response", m_label.c_str());
                CloseServer();
                return false;
            }
            // Drop the consumed response header bytes; keep any frame bytes that
            // followed for the frame decoder.
            m_wsInBuf.erase(m_wsInBuf.begin(), m_wsInBuf.begin() + res.consumed);
            break;
        }

        if (!net::SetNonBlocking(m_serverFd))
        {
            QDC_LOGW("[%s] failed to set server socket non-blocking", m_label.c_str());
            CloseServer();
            return false;
        }
        net::SetTcpNoDelay(m_serverFd);

        QDC_LOGI("[%s] WebSocket connected", m_label.c_str());
        m_lastHeartbeatMs = 0; // send a heartbeat promptly to keep the stream non-idle
        return true;
    }

    void HostTunnelPort::CloseServer()
    {
        net::CloseSocket(m_serverFd);
        DropAllClients();
        m_wsInBuf.clear();
        m_wsAppBuf.clear();
        m_muxReadBuf.clear();
        m_serverWriteBuf.clear();
    }

    void HostTunnelPort::DropAllClients()
    {
        for (auto& kv : m_clients)
        {
            net::CloseSocket(kv.second.fd);
        }
        m_clients.clear();
    }

    // ── Server (device WebSocket) I/O ───────────────────────────────────

    void HostTunnelPort::QueueServerFrame(std::vector<uint8_t>&& muxFrame)
    {
        if (m_serverFd == net::kInvalidSocket)
        {
            return;
        }
        // Client->server frames MUST be masked (RFC 6455).
        std::vector<uint8_t> ws = WebSocket::EncodeBinaryMasked(muxFrame.data(), muxFrame.size());
        m_serverWriteBuf.insert(m_serverWriteBuf.end(), ws.begin(), ws.end());
    }

    void HostTunnelPort::FlushServerWrite()
    {
        while (m_serverFd != net::kInvalidSocket && !m_serverWriteBuf.empty())
        {
            const size_t chunk = std::min(m_serverWriteBuf.size(), kReadChunk);
            std::vector<uint8_t> tmp(m_serverWriteBuf.begin(), m_serverWriteBuf.begin() + chunk);
            bool wouldBlock = false;
            long n = net::Send(m_serverFd, tmp.data(), tmp.size(), &wouldBlock);
            if (n > 0)
            {
                m_serverWriteBuf.erase(m_serverWriteBuf.begin(), m_serverWriteBuf.begin() + n);
            }
            else if (n < 0 && wouldBlock)
            {
                break; // try again when writable
            }
            else
            {
                QDC_LOGW("[%s] server send failed", m_label.c_str());
                CloseServer();
                break;
            }
        }
    }

    void HostTunnelPort::MaybeSendHeartbeat()
    {
        if (m_serverFd == net::kInvalidSocket)
        {
            return;
        }
        const long long now = NowMs();
        if (now - m_lastHeartbeatMs < kHeartbeatIntervalMs)
        {
            return;
        }
        m_lastHeartbeatMs = now;
        QueueServerFrame(MuxProtocol::EncodeHeartbeat());
        FlushServerWrite();
    }

    void HostTunnelPort::OnServerReadable(bool& shouldClose)
    {
        uint8_t buf[kReadChunk];
        while (true)
        {
            bool wouldBlock = false;
            long n = net::Recv(m_serverFd, buf, sizeof(buf), &wouldBlock);
            if (n > 0)
            {
                m_wsInBuf.insert(m_wsInBuf.end(), buf, buf + n);
            }
            else if (n == 0)
            {
                QDC_LOGI("[%s] server disconnected", m_label.c_str());
                shouldClose = true;
                return;
            }
            else
            {
                if (wouldBlock)
                {
                    break;
                }
                QDC_LOGW("[%s] server recv failed", m_label.c_str());
                shouldClose = true;
                return;
            }
        }

        // Decode WebSocket frames (server->client: unmasked). Their payload is the
        // MuxProtocol byte stream.
        WebSocket::Frame wsFrame;
        while (WebSocket::TryDecodeFrame(m_wsInBuf, wsFrame, /*requireMask=*/false))
        {
            m_wsInBuf.erase(m_wsInBuf.begin(), m_wsInBuf.begin() + wsFrame.consumed);

            switch (wsFrame.opcode)
            {
            case WebSocket::Opcode::Close:
                QDC_LOGI("[%s] server sent WebSocket close", m_label.c_str());
                shouldClose = true;
                return;
            case WebSocket::Opcode::Ping:
            {
                // Reply with a masked Pong echoing the payload (client frames masked).
                std::vector<uint8_t> pong = WebSocket::EncodeControlMasked(
                    WebSocket::Opcode::Pong, wsFrame.payload.data(), wsFrame.payload.size());
                m_serverWriteBuf.insert(m_serverWriteBuf.end(), pong.begin(), pong.end());
                FlushServerWrite();
                continue;
            }
            case WebSocket::Opcode::Pong:
                continue; // ignore
            default:
                break; // Binary / Text / Continuation carry app data
            }

            // Accumulate (handles fragmentation across continuation frames).
            m_wsAppBuf.insert(m_wsAppBuf.end(), wsFrame.payload.begin(), wsFrame.payload.end());
            if (!wsFrame.fin)
            {
                continue;
            }

            m_muxReadBuf.insert(m_muxReadBuf.end(), m_wsAppBuf.begin(), m_wsAppBuf.end());
            m_wsAppBuf.clear();

            DecodedFrame frame;
            bool muxFatal = false;
            while (MuxProtocol::TryDecode(m_muxReadBuf, frame, &muxFatal))
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
            if (muxFatal)
            {
                QDC_LOGW("[%s] corrupt MuxProtocol frame length; closing server connection", m_label.c_str());
                shouldClose = true;
                return;
            }
        }

        if (wsFrame.error)
        {
            QDC_LOGW("[%s] malformed WebSocket frame from server; closing connection", m_label.c_str());
            shouldClose = true;
            return;
        }
    }

    void HostTunnelPort::HandleControlFrame(const std::string& text)
    {
        static const std::string kConnect = "CONNECT:";
        static const std::string kDisconnect = "DISCONNECT:";
        if (text.compare(0, kConnect.size(), kConnect) == 0)
        {
            OpenForwardClient(text.substr(kConnect.size()));
        }
        else if (text.compare(0, kDisconnect.size(), kDisconnect) == 0)
        {
            CloseForwardClient(text.substr(kDisconnect.size()), /*notifyServer=*/false);
        }
        else
        {
            QDC_LOGV("[%s] unknown control frame: %.*s", m_label.c_str(),
                     static_cast<int>(text.size() > 60 ? 60 : text.size()), text.c_str());
        }
    }

    void HostTunnelPort::HandleDataFrame(const std::vector<uint8_t>& payload)
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

        it->second.pendingWrite.insert(it->second.pendingWrite.end(), data, data + dataSize);
        OnClientWritable(it->second);
    }

    // ── Forwarded local TCP connections (to SDPCore) ────────────────────

    void HostTunnelPort::OpenForwardClient(const std::string& hex)
    {
        Uuid uuid{};
        if (!UuidUtil::FromHex(hex, uuid))
        {
            QDC_LOGW("[%s] CONNECT with malformed uuid", m_label.c_str());
            return;
        }

        QDC_LOGI("[%s][%.8s] opening TCP connection to %s:%u",
                 m_label.c_str(), hex.c_str(), m_forwardHost.c_str(), m_forwardPort);

        net::socket_t fd = net::TcpConnect(m_forwardHost, m_forwardPort);
        if (fd == net::kInvalidSocket)
        {
            QDC_LOGW("[%s][%.8s] cannot connect to %s:%u",
                     m_label.c_str(), hex.c_str(), m_forwardHost.c_str(), m_forwardPort);
            // Tell the device this stream could not be established.
            QueueServerFrame(MuxProtocol::EncodeControl("DISCONNECT:" + hex));
            FlushServerWrite();
            return;
        }

        net::SetNonBlocking(fd);
        net::SetTcpNoDelay(fd);

        ForwardClient client;
        client.fd = fd;
        client.uuid = uuid;
        m_clients[hex] = client;

        QDC_LOGI("[%s][%.8s] TCP connection established", m_label.c_str(), hex.c_str());
    }

    void HostTunnelPort::OnClientReadable(ForwardClient& client, bool& shouldClose)
    {
        uint8_t buf[kReadChunk];
        while (true)
        {
            bool wouldBlock = false;
            long n = net::Recv(client.fd, buf, sizeof(buf), &wouldBlock);
            if (n > 0)
            {
                QueueServerFrame(MuxProtocol::EncodeData(client.uuid, buf, static_cast<size_t>(n)));

                // Backpressure: stop draining this socket if the server write buffer
                // has grown past the high-watermark.
                if (m_serverWriteBuf.size() >= kServerWriteHighWatermark)
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
                if (wouldBlock)
                {
                    break;
                }
                shouldClose = true;
                break;
            }
        }
        FlushServerWrite();
    }

    void HostTunnelPort::OnClientWritable(ForwardClient& client)
    {
        while (client.fd != net::kInvalidSocket && !client.pendingWrite.empty())
        {
            const size_t chunk = std::min(client.pendingWrite.size(), kReadChunk);
            std::vector<uint8_t> tmp(client.pendingWrite.begin(), client.pendingWrite.begin() + chunk);
            bool wouldBlock = false;
            long n = net::Send(client.fd, tmp.data(), tmp.size(), &wouldBlock);
            if (n > 0)
            {
                client.pendingWrite.erase(client.pendingWrite.begin(), client.pendingWrite.begin() + n);
            }
            else if (n < 0 && wouldBlock)
            {
                break; // retry when writable
            }
            else
            {
                CloseForwardClient(UuidUtil::ToHex(client.uuid), /*notifyServer=*/true);
                return;
            }
        }
    }

    void HostTunnelPort::CloseForwardClient(const std::string& hex, bool notifyServer)
    {
        auto it = m_clients.find(hex);
        if (it == m_clients.end())
        {
            return;
        }

        net::CloseSocket(it->second.fd);
        m_clients.erase(it);

        if (notifyServer)
        {
            QueueServerFrame(MuxProtocol::EncodeControl("DISCONNECT:" + hex));
            FlushServerWrite();
        }

        QDC_LOGI("[%s][%.8s] forwarded connection closed", m_label.c_str(), hex.c_str());
    }

    // ── Event loop (one connection attempt) ─────────────────────────────

    void HostTunnelPort::RunOnce()
    {
        if (!ConnectAndHandshake())
        {
            return;
        }

        // Drain any frame bytes that arrived immediately after the handshake, and send
        // the initial heartbeat.
        MaybeSendHeartbeat();
        {
            bool shouldClose = false;
            OnServerReadable(shouldClose);
            if (shouldClose)
            {
                CloseServer();
                return;
            }
        }

        while (m_running.load() && m_serverFd != net::kInvalidSocket)
        {
            std::vector<net::PollFd>  fds;
            std::vector<PollEntry>    entries;

            {
                short ev = net::kPollIn;
                if (!m_serverWriteBuf.empty())
                {
                    ev |= net::kPollOut;
                }
                fds.push_back(net::PollFd{ m_serverFd, ev, 0 });
                entries.push_back(PollEntry{ PollKind::Server, {} });
            }

            const bool acceptClientReads = m_serverWriteBuf.size() < kServerWriteHighWatermark;
            for (auto& kv : m_clients)
            {
                short ev = 0;
                if (acceptClientReads)
                {
                    ev |= net::kPollIn;
                }
                if (!kv.second.pendingWrite.empty())
                {
                    ev |= net::kPollOut;
                }
                fds.push_back(net::PollFd{ kv.second.fd, ev, 0 });
                entries.push_back(PollEntry{ PollKind::Client, kv.first });
            }

            int rc = net::Poll(fds.data(), fds.size(), 250 /* ms */);
            if (rc < 0)
            {
                QDC_LOGW("[%s] poll() failed", m_label.c_str());
                break;
            }
            if (rc == 0)
            {
                MaybeSendHeartbeat();
                continue;
            }

            for (size_t i = 0; i < fds.size(); ++i)
            {
                const short revents = fds[i].revents;
                if (revents == 0)
                {
                    continue;
                }

                if (entries[i].kind == PollKind::Server)
                {
                    if (m_serverFd == net::kInvalidSocket || m_serverFd != fds[i].fd)
                    {
                        break;
                    }
                    if (revents & (net::kPollErr | net::kPollHup | net::kPollNval))
                    {
                        CloseServer();
                        break;
                    }
                    if (revents & net::kPollOut)
                    {
                        FlushServerWrite();
                    }
                    if (m_serverFd == net::kInvalidSocket || m_serverFd != fds[i].fd)
                    {
                        break;
                    }
                    if (revents & net::kPollIn)
                    {
                        bool shouldClose = false;
                        OnServerReadable(shouldClose);
                        if (shouldClose)
                        {
                            CloseServer();
                        }
                    }
                }
                else // PollKind::Client
                {
                    auto it = m_clients.find(entries[i].clientKey);
                    if (it == m_clients.end() || it->second.fd != fds[i].fd)
                    {
                        continue; // closed or replaced during this pass
                    }

                    if (revents & (net::kPollErr | net::kPollHup | net::kPollNval))
                    {
                        CloseForwardClient(entries[i].clientKey, true);
                        continue;
                    }
                    if (revents & net::kPollOut)
                    {
                        OnClientWritable(it->second);
                    }
                    it = m_clients.find(entries[i].clientKey);
                    if (it == m_clients.end() || it->second.fd != fds[i].fd)
                    {
                        continue;
                    }
                    if (revents & net::kPollIn)
                    {
                        bool shouldClose = false;
                        OnClientReadable(it->second, shouldClose);
                        if (shouldClose)
                        {
                            CloseForwardClient(entries[i].clientKey, true);
                        }
                    }
                }
            }

            MaybeSendHeartbeat();
        }

        CloseServer();
    }
}
