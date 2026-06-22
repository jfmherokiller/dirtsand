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

#include "MsgChannel.h"
#include "errors.h"

#ifdef _WIN32
#   include <cstring>
#else
#   include <sys/eventfd.h>
#   include <unistd.h>
#   include <errno.h>
#   include <cstring>
#endif


DS::MsgChannel::~MsgChannel()
{
#ifdef _WIN32
    if (m_semaphore != DS_INVALID_SOCK)
        closesocket(m_semaphore);
#else
    if (m_semaphore < 0)
        return;

    int result = close(m_semaphore);
    if (result < 0 && errno != EBADF) {
        ST::printf(stderr, "WARNING: Failed to close event semaphore: {}\n",
                   strerror(errno));
    }
#endif
}

ds_socket_t DS::MsgChannel::fd()
{
    std::lock_guard<std::mutex> guard(m_mutex);

#ifdef _WIN32
    if (m_semaphore != DS_INVALID_SOCK)
        return m_semaphore;

    m_semaphore = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_semaphore == INVALID_SOCKET)
        throw SystemError("Failed to create UDP socket for message channel", "WSA error");

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(m_semaphore, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(m_semaphore);
        m_semaphore = DS_INVALID_SOCK;
        throw SystemError("Failed to bind UDP socket for message channel", "WSA error");
    }

    int addrlen = sizeof(m_peerAddr);
    if (getsockname(m_semaphore, reinterpret_cast<sockaddr*>(&m_peerAddr), &addrlen) != 0) {
        closesocket(m_semaphore);
        m_semaphore = DS_INVALID_SOCK;
        throw SystemError("Failed to get address of UDP message channel socket", "WSA error");
    }

    return m_semaphore;
#else
    if (m_semaphore >= 0)
        return m_semaphore;

    m_semaphore = eventfd(0, EFD_SEMAPHORE);
    if (m_semaphore < 0)
        throw SystemError("Failed to create event semaphore", strerror(errno));
    return m_semaphore;
#endif
}

void DS::MsgChannel::putMessage(int type, void* payload)
{
    FifoMessage msg;
    msg.m_messageType = type;
    msg.m_payload = payload;
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_queue.push(msg);
    }

#ifdef _WIN32
    fd();  // ensure socket is initialised and m_peerAddr is populated
    char signal = 1;
    if (sendto(m_semaphore, &signal, 1, 0,
               reinterpret_cast<sockaddr*>(&m_peerAddr), sizeof(m_peerAddr)) != 1)
        throw SystemError("Failed to signal message channel", "sendto failed");
#else
    int result = eventfd_write(fd(), 1);
    if (result < 0)
        throw SystemError("Failed to write to event semaphore", strerror(errno));
#endif
}

DS::FifoMessage DS::MsgChannel::getMessage()
{
#ifdef _WIN32
    fd();  // ensure initialised
    char signal;
    if (recvfrom(m_semaphore, &signal, 1, 0, nullptr, nullptr) != 1)
        throw SystemError("Failed to wait on message channel", "recvfrom failed");
#else
    eventfd_t value;
    int result = eventfd_read(fd(), &value);
    if (result < 0)
        throw SystemError("Failed to read from event semaphore", strerror(errno));
#endif

    std::lock_guard<std::mutex> guard(m_mutex);
    FifoMessage msg = m_queue.front();
    m_queue.pop();
    return msg;
}

bool DS::MsgChannel::hasMessage()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    return !m_queue.empty();
}
