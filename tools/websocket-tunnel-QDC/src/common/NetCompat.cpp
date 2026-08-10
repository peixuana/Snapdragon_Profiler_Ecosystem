//============================================================================================================
//
//                    Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                                SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

#include "NetCompat.h"

#include <cstring>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <csignal>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace qdc
{
    namespace net
    {
        const short kPollIn   = POLLIN;
        const short kPollOut  = POLLOUT;
        const short kPollErr  = POLLERR;
        const short kPollHup  = POLLHUP;
        const short kPollNval = POLLNVAL;

#ifdef _WIN32
        namespace
        {
            bool WouldBlockError()
            {
                return WSAGetLastError() == WSAEWOULDBLOCK;
            }
        }

        bool Init()
        {
            WSADATA wsa;
            return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }

        void Shutdown()
        {
            WSACleanup();
        }

        void IgnoreSigpipe()
        {
            // No SIGPIPE on Windows.
        }

        void CloseSocket(socket_t& fd)
        {
            if (fd != kInvalidSocket)
            {
                ::closesocket(static_cast<SOCKET>(fd));
                fd = kInvalidSocket;
            }
        }

        bool SetNonBlocking(socket_t fd)
        {
            u_long mode = 1;
            return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
        }
#else
        namespace
        {
            bool WouldBlockError()
            {
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
        }

        bool Init() { return true; }
        void Shutdown() {}

        void IgnoreSigpipe()
        {
            ::signal(SIGPIPE, SIG_IGN);
        }

        void CloseSocket(socket_t& fd)
        {
            if (fd != kInvalidSocket)
            {
                ::close(fd);
                fd = kInvalidSocket;
            }
        }

        bool SetNonBlocking(socket_t fd)
        {
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags < 0)
            {
                return false;
            }
            return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
        }
#endif

        bool SetTcpNoDelay(socket_t fd)
        {
            int one = 1;
            return ::setsockopt(static_cast<
#ifdef _WIN32
                                SOCKET
#else
                                int
#endif
                                >(fd),
                                IPPROTO_TCP, TCP_NODELAY,
                                reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
        }

        socket_t TcpConnect(const std::string& host, uint16_t port)
        {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            addrinfo* result = nullptr;
            const std::string portStr = std::to_string(port);
            if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr)
            {
                return kInvalidSocket;
            }

            socket_t fd = kInvalidSocket;
            for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next)
            {
#ifdef _WIN32
                SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (s == INVALID_SOCKET)
                {
                    continue;
                }
                if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
                {
                    fd = static_cast<socket_t>(s);
                    break;
                }
                ::closesocket(s);
#else
                int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (s < 0)
                {
                    continue;
                }
                if (::connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
                {
                    fd = s;
                    break;
                }
                ::close(s);
#endif
            }

            ::freeaddrinfo(result);
            if (fd != kInvalidSocket)
            {
                SetTcpNoDelay(fd);
            }
            return fd;
        }

        long Recv(socket_t fd, void* buf, size_t len, bool* wouldBlock)
        {
            if (wouldBlock)
            {
                *wouldBlock = false;
            }
#ifdef _WIN32
            int n = ::recv(static_cast<SOCKET>(fd), static_cast<char*>(buf), static_cast<int>(len), 0);
#else
            ssize_t n = ::recv(fd, buf, len, 0);
#endif
            if (n < 0 && wouldBlock)
            {
                *wouldBlock = WouldBlockError();
            }
            return static_cast<long>(n);
        }

        long Send(socket_t fd, const void* buf, size_t len, bool* wouldBlock)
        {
            if (wouldBlock)
            {
                *wouldBlock = false;
            }
#ifdef _WIN32
            int n = ::send(static_cast<SOCKET>(fd), static_cast<const char*>(buf), static_cast<int>(len), 0);
#else
            ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
#endif
            if (n < 0 && wouldBlock)
            {
                *wouldBlock = WouldBlockError();
            }
            return static_cast<long>(n);
        }

        int Poll(PollFd* fds, size_t n, int timeoutMs)
        {
#ifdef _WIN32
            std::vector<WSAPOLLFD> native(n);
            for (size_t i = 0; i < n; ++i)
            {
                native[i].fd = static_cast<SOCKET>(fds[i].fd);
                native[i].events = fds[i].events;
                native[i].revents = 0;
            }
            int rc = ::WSAPoll(native.empty() ? nullptr : native.data(), static_cast<ULONG>(n), timeoutMs);
            for (size_t i = 0; i < n; ++i)
            {
                fds[i].revents = native[i].revents;
            }
            return rc;
#else
            std::vector<pollfd> native(n);
            for (size_t i = 0; i < n; ++i)
            {
                native[i].fd = fds[i].fd;
                native[i].events = fds[i].events;
                native[i].revents = 0;
            }
            int rc = ::poll(native.empty() ? nullptr : native.data(), static_cast<nfds_t>(n), timeoutMs);
            for (size_t i = 0; i < n; ++i)
            {
                fds[i].revents = native[i].revents;
            }
            return rc;
#endif
        }
    }
}
