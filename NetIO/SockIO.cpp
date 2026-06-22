/******************************************************************************
 * This file is part of dirtsand.                                             *
 *                                                                            *
 * dirtsand is free software: you can redistribute it and/or modify           *
 * it under the terms of the GNU Affero General Public License as             *
 * published by the Free Software Foundation, either version 3 of the         *
 * License, or (at your option) any later version.                            *
 *                                                                            *
 * dirtsand is distributed in the hope that it will be useful,                *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU Affero General Public License for more details.                        *
 *                                                                            *
 * You should have received a copy of the GNU Affero General Public License   *
 * along with dirtsand.  If not, see <http://www.gnu.org/licenses/>.          *
 ******************************************************************************/

#include "SockIO.h"
#include "errors.h"
#include "settings.h"

#include <string_theory/format>
#ifndef _WIN32
#   include <sys/sendfile.h>
#endif
#include <cstdio>
#include <cstring>
#include <mutex>

static const int SOCK_NO  = 0;
static const int SOCK_YES = 1;

struct SocketHandle_Private
{
    ds_socket_t m_sockfd;
    union {
        sockaddr         m_addr;
        sockaddr_in      m_in4addr;
        sockaddr_in6     m_in6addr;
        sockaddr_storage m_addrMax;
    };
    socklen_t m_addrLen;

    SocketHandle_Private(ds_socket_t fd)
        : m_sockfd(fd), m_addrLen(sizeof(m_addrMax)) { }
};

static void* get_in_addr(SocketHandle_Private* sock)
{
    if (sock->m_addr.sa_family == AF_INET)
        return &sock->m_in4addr.sin_addr;
    return &sock->m_in6addr.sin6_addr;
}

static uint16_t get_in_port(SocketHandle_Private* sock)
{
    if (sock->m_addr.sa_family == AF_INET)
        return ntohs(sock->m_in4addr.sin_port);
    return ntohs(sock->m_in6addr.sin6_port);
}

/* Portable getaddrinfo error string.  On Linux EAI_SYSTEM means errno holds
   the real error; Windows never returns EAI_SYSTEM from getaddrinfo. */
static const char* getaddrinfo_strerror(int err)
{
#ifdef _WIN32
    return gai_strerrorA(err);
#else
    return (err == EAI_SYSTEM) ? strerror(errno) : gai_strerror(err);
#endif
}

DS::SocketHandle DS::BindSocket(const char* address, const char* port)
{
    int result;
    ds_socket_t sockfd;

    addrinfo info;
    memset(&info, 0, sizeof(info));
    info.ai_family = AF_UNSPEC;
    info.ai_socktype = SOCK_STREAM;
    info.ai_flags = AI_PASSIVE;

    addrinfo* addrList;
    result = getaddrinfo(address, port, &info, &addrList);
    if (result != 0) {
        const char* error_text = getaddrinfo_strerror(result);
        auto message = ST::format("Failed to bind to {}:{}", address, port);
        throw SystemError(message.c_str(), error_text);
    }

    addrinfo* addr_iter;
    for (addr_iter = addrList; addr_iter != nullptr; addr_iter = addr_iter->ai_next) {
        sockfd = socket(addr_iter->ai_family, addr_iter->ai_socktype,
                        addr_iter->ai_protocol);
        if (sockfd == DS_INVALID_SOCK)
            continue;

        // Avoid annoying "Address already in use" messages when restarting
        // the server.
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, SOCK_OPT(SOCK_YES), sizeof(SOCK_YES)) < 0)
            ST::printf(stderr, "[Bind] Warning: Failed to set socket address reuse: {}\n", strerror(errno));
        if (bind(sockfd, addr_iter->ai_addr, addr_iter->ai_addrlen) == 0)
            break;
        ST::printf(stderr, "[Bind] {}\n", strerror(errno));

        // Couldn't bind, try the next one
        sock_close(sockfd);
    }
    freeaddrinfo(addrList);

    // Die if we didn't get a successful socket
    if (!addr_iter) {
        auto message = ST::format("Failed to bind to a usable socket on {}:{}", address, port);
        throw SystemError(message.c_str());
    }

    SocketHandle_Private* sockinfo = new SocketHandle_Private(sockfd);
    result = getsockname(sockfd, &sockinfo->m_addr, &sockinfo->m_addrLen);
    if (result != 0) {
        const char *error_text = strerror(errno);
        delete sockinfo;
        throw SystemError("Failed to get bound socket address", error_text);
    }
    return reinterpret_cast<SocketHandle>(sockinfo);
}

void DS::ListenSock(const DS::SocketHandle sock, int backlog)
{
    DS_ASSERT(sock);
    int result = listen(reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd, backlog);
    if (result < 0) {
        const char *error_text = strerror(errno);
        auto message = ST::format("Failed to listen on {}", DS::SockIpAddress(sock));
        throw SystemError(message.c_str(), error_text);
    }
}

DS::SocketHandle DS::AcceptSock(const DS::SocketHandle sock)
{
    DS_ASSERT(sock);
    SocketHandle_Private* sockp = reinterpret_cast<SocketHandle_Private*>(sock);

    SocketHandle_Private* client = new SocketHandle_Private(DS_INVALID_SOCK);
    client->m_sockfd = accept(sockp->m_sockfd, &client->m_addr, &client->m_addrLen);
    if (client->m_sockfd == DS_INVALID_SOCK) {
        delete client;
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEINVAL || err == WSAENOTSOCK) {
            throw DS::SockHup();
        } else if (err == WSAECONNABORTED) {
            return nullptr;
        } else {
            ST::printf(stderr, "Failed to accept incoming connection: WSA error {}\n", err);
            return nullptr;
        }
#else
        if (errno == EINVAL) {
            throw DS::SockHup();
        } else if (errno == ECONNABORTED) {
            return nullptr;
        } else {
            ST::printf(stderr, "Failed to accept incoming connection: {}\n",
                       strerror(errno));
            return nullptr;
        }
#endif
    }

    /* SO_RCVTIMEO takes a timeval on POSIX, but a DWORD of milliseconds on Windows */
#ifdef _WIN32
    DWORD tv = NET_TIMEOUT * 1000;
    if (setsockopt(client->m_sockfd, SOL_SOCKET, SO_RCVTIMEO, SOCK_OPT(tv), sizeof(tv)) < 0)
        ST::printf(stderr, "Warning: Failed to set recv timeout: WSA error {}\n", WSAGetLastError());
#else
    timeval tv;
    tv.tv_sec = NET_TIMEOUT;
    tv.tv_usec = 0;
    if (setsockopt(client->m_sockfd, SOL_SOCKET, SO_RCVTIMEO, SOCK_OPT(tv), sizeof(tv)) < 0)
        ST::printf(stderr, "Warning: Failed to set recv timeout: {}\n", strerror(errno));
#endif

    // eap-tastic protocols require Nagle's algo be disabled
    if (setsockopt(client->m_sockfd, IPPROTO_TCP, TCP_NODELAY, SOCK_OPT(SOCK_YES), sizeof(SOCK_YES)) < 0)
        ST::printf(stderr, "Warning: Failed to set TCP nodelay: {}\n", strerror(errno));
    return reinterpret_cast<SocketHandle>(client);
}

void DS::CloseSock(DS::SocketHandle sock)
{
    if (!sock) {
        fputs("WARNING: Tried to close invalid socket\n", stderr);
        return;
    }
    shutdown(reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd, SHUT_RDWR);
    sock_close(reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd);
}

void DS::FreeSock(DS::SocketHandle sock)
{
    CloseSock(sock);
    delete reinterpret_cast<SocketHandle_Private*>(sock);
}

ST::string DS::SockIpAddress(const DS::SocketHandle sock)
{
    char addrbuf[256];
    SocketHandle_Private* sockp = reinterpret_cast<SocketHandle_Private*>(sock);
    if (!inet_ntop(sockp->m_addr.sa_family, get_in_addr(sockp), addrbuf, 256)) {
        ST::printf(stderr, "Failed to get socket address: {}\n", strerror(errno));
        return ST_LITERAL("???");
    }
    return ST::format("{}/{}", addrbuf, get_in_port(sockp));
}

uint32_t DS::GetAddress4(const char* lookup)
{
    addrinfo info;
    memset(&info, 0, sizeof(info));
    info.ai_family = AF_INET;
    info.ai_socktype = SOCK_STREAM;
    info.ai_flags = 0;

    addrinfo* addrList;
    int result = getaddrinfo(lookup, nullptr, &info, &addrList);
    if (result != 0) {
        ST::printf(stderr, "WARNING: Failed to get address of {}: {}\n",
                   lookup, getaddrinfo_strerror(result));
        return 0;
    }
    if (!addrList) {
        ST::printf(stderr, "WARNING: No address info found for {}\n", lookup);
        return 0;
    }
    uint32_t addr = reinterpret_cast<sockaddr_in*>(addrList->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(addrList);

    return ntohl(addr);
}

ds_socket_t DS::SockFd(const DS::SocketHandle sock)
{
    return reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd;
}

void DS::SendBuffer(const DS::SocketHandle sock, const void* buffer, size_t size)
{
    do {
        ssize_t bytes = send(reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd,
                             reinterpret_cast<const char*>(buffer), size, 0);
        if (bytes < 0) {
            if (errno != EPIPE && errno != ECONNRESET) {
                const char *error_text = strerror(errno);
                ST::printf(stderr, "Failed to send to {}: {}\n",
                           DS::SockIpAddress(sock), error_text);
            }
            throw DS::SockHup();
        } else if (bytes == 0) {
            // Connection closed without error
            throw DS::SockHup();
        }

        size -= bytes;
        buffer = reinterpret_cast<const void*>(reinterpret_cast<const uint8_t*>(buffer) + bytes);
    } while (size > 0);
}

void DS::SendFile(const DS::SocketHandle sock, const void* buffer, size_t bufsz,
                  int fd, int64_t* offset, size_t fdsz)
{
    SocketHandle_Private* imp = reinterpret_cast<SocketHandle_Private*>(sock);

    // Send the prepended buffer
#ifndef _WIN32
    if (setsockopt(imp->m_sockfd, IPPROTO_TCP, TCP_CORK, SOCK_OPT(SOCK_YES), sizeof(SOCK_YES)) < 0)
        ST::printf(stderr, "Warning: Failed to set cork option: {}", strerror(errno));
#endif
    while (bufsz > 0) {
        ssize_t bytes = send(imp->m_sockfd, reinterpret_cast<const char*>(buffer), bufsz, 0);
        if (bytes < 0) {
            if (errno != EPIPE && errno != ECONNRESET) {
                const char *error_text = strerror(errno);
                ST::printf(stderr, "Failed to send to {}: {}\n",
                           DS::SockIpAddress(sock), error_text);
            }
            throw DS::SockHup();
        } else if (bytes == 0) {
            // Connection closed without error
            throw DS::SockHup();
        }

        bufsz -= bytes;
        buffer = reinterpret_cast<const void*>(reinterpret_cast<const uint8_t*>(buffer) + bytes);
    }

    // Now send the file data
#ifdef _WIN32
    /* Windows lacks sendfile(); use _read() + send() instead */
    while (fdsz > 0) {
        uint8_t chunk[32768];
        size_t toRead = (fdsz < sizeof(chunk)) ? fdsz : sizeof(chunk);
        int nread = _read(fd, chunk, static_cast<unsigned int>(toRead));
        if (nread <= 0)
            throw DS::SockHup();
        const char* sendptr = reinterpret_cast<const char*>(chunk);
        int remaining = nread;
        while (remaining > 0) {
            int sent = send(imp->m_sockfd, sendptr, remaining, 0);
            if (sent <= 0)
                throw DS::SockHup();
            sendptr += sent;
            remaining -= sent;
        }
        if (offset)
            *offset += nread;
        fdsz -= static_cast<size_t>(nread);
    }
#else
    while (fdsz > 0) {
        ssize_t bytes = sendfile(imp->m_sockfd, fd, reinterpret_cast<off_t*>(offset), fdsz);
        if (bytes < 0) {
            if (errno == EAGAIN) {
                continue;
            } else if (errno != EPIPE && errno != ECONNRESET) {
                const char *error_text = strerror(errno);
                ST::printf(stderr, "Failed to send to {}: {}\n",
                           DS::SockIpAddress(sock), error_text);
            }
            throw DS::SockHup();
        } else if (bytes == 0) {
            throw DS::SockHup();
        }
        fdsz -= bytes;
    }
    if (setsockopt(imp->m_sockfd, IPPROTO_TCP, TCP_CORK, SOCK_OPT(SOCK_NO), sizeof(SOCK_NO)) < 0)
        ST::printf(stderr, "Warning: Failed to set cork option: {}", strerror(errno));
#endif
}

void DS::RecvBuffer(const DS::SocketHandle sock, void* buffer, size_t size)
{
    while (size > 0) {
        ssize_t bytes = recv(reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd,
                             reinterpret_cast<char*>(buffer), size, 0);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            } else if (errno != ECONNRESET && errno != EAGAIN
                       && errno != EWOULDBLOCK && errno != EPIPE) {
                const char *error_text = strerror(errno);
                ST::printf(stderr, "Failed to recv from {}: {}\n",
                           DS::SockIpAddress(sock), error_text);
            }
            throw DS::SockHup();
        } else if (bytes == 0) {
            throw DS::SockHup();
        }

        size -= bytes;
        buffer = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(buffer) + bytes);
    }
}

size_t DS::PeekSize(const SocketHandle sock)
{
    ds_socket_t fd = reinterpret_cast<SocketHandle_Private*>(sock)->m_sockfd;

#ifdef _WIN32
    /* MSG_TRUNC is not available on connected Windows sockets; use FIONREAD instead */
    u_long available = 0;
    if (ioctlsocket(fd, FIONREAD, &available) == SOCKET_ERROR) {
        ST::printf(stderr, "Failed to peek from {}: WSA error {}\n",
                   DS::SockIpAddress(sock), WSAGetLastError());
        throw DS::SockHup();
    }
    if (available == 0)
        throw DS::SockHup();
    return static_cast<size_t>(available);
#else
    uint8_t buffer[256];
    ssize_t bytes = recv(fd, buffer, 256, MSG_PEEK | MSG_TRUNC);

    if (bytes < 0) {
        if (errno != ECONNRESET && errno != EAGAIN && errno != EWOULDBLOCK) {
            const char *error_text = strerror(errno);
            ST::printf(stderr, "Failed to peek from {}: {}\n",
                       DS::SockIpAddress(sock), error_text);
        }
        throw DS::SockHup();
    } else if (bytes == 0) {
        throw DS::SockHup();
    }

    return static_cast<size_t>(bytes);
#endif
}
