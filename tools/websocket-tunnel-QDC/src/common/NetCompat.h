//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Thin cross-platform sockets shim so the host tunnel can be built natively for
// both Linux (POSIX sockets + poll) and Windows (Winsock2 + WSAPoll) without any
// third-party dependency. The device-side target binary uses raw POSIX sockets
// directly (it only ever runs on Android / Qualcomm Linux); this shim exists for
// the portable host binary and the shared byte-pump logic.

#ifndef QDC_TUNNEL_NETCOMPAT_H
#define QDC_TUNNEL_NETCOMPAT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace qdc
{
    namespace net
    {
#ifdef _WIN32
        using socket_t = uintptr_t; // matches Winsock SOCKET
        constexpr socket_t kInvalidSocket = ~static_cast<socket_t>(0);
#else
        using socket_t = int;
        constexpr socket_t kInvalidSocket = -1;
#endif

        // poll() event flags, re-exported so callers never include platform headers.
        extern const short kPollIn;
        extern const short kPollOut;
        extern const short kPollErr;
        extern const short kPollHup;
        extern const short kPollNval;

        struct PollFd
        {
            socket_t fd;
            short    events;
            short    revents;
        };

        // Process-wide init/teardown (WSAStartup on Windows; no-op on POSIX).
        bool Init();
        void Shutdown();

        // Ignore SIGPIPE so a broken socket write returns an error instead of killing
        // the process (POSIX only; no-op on Windows).
        void IgnoreSigpipe();

        void CloseSocket(socket_t& fd);
        bool SetNonBlocking(socket_t fd);
        bool SetTcpNoDelay(socket_t fd);

        // Blocking TCP connect to host:port (numeric IP or hostname, IPv4/IPv6).
        // Returns kInvalidSocket on failure.
        socket_t TcpConnect(const std::string& host, uint16_t port);

        // recv/send wrappers. Return >0 (bytes), 0 (peer closed), or -1 (error).
        // On a -1 return, *wouldBlock is set true when the cause was EAGAIN/
        // EWOULDBLOCK (i.e. retry later), false for a real error.
        long Recv(socket_t fd, void* buf, size_t len, bool* wouldBlock);
        long Send(socket_t fd, const void* buf, size_t len, bool* wouldBlock);

        // poll() wrapper (WSAPoll on Windows). timeoutMs<0 blocks indefinitely.
        int Poll(PollFd* fds, size_t n, int timeoutMs);
    }
}

#endif // QDC_TUNNEL_NETCOMPAT_H
