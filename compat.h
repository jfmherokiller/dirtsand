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

#ifndef _DS_COMPAT_H
#define _DS_COMPAT_H

/* Platform portability shim.  Include this wherever POSIX socket or OS
   primitives are needed; it pulls in the right headers for each platform
   and supplies the compatibility layer that keeps the rest of the code clean. */

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <io.h>          /* _read() */
#   include <stdlib.h>      /* _byteswap_* */
#   include <process.h>     /* _getpid */
#   include <cstdint>

    /* POSIX process types — MSVC only (MinGW gets pid_t from its own headers) */
#   ifdef _MSC_VER
    typedef int pid_t;
#   define getpid _getpid
#   endif

#   ifdef _MSC_VER
    /* GCC/Clang byte-swap built-ins — map to MSVC intrinsics */
#       define __builtin_bswap16(x)  _byteswap_ushort(x)
#       define __builtin_bswap32(x)  _byteswap_ulong(x)
#       define __builtin_bswap64(x)  _byteswap_uint64(x)
    /* MSVC does not ship ssize_t; MinGW provides it via its own sys/types.h */
#       include <BaseTsd.h>
        typedef SSIZE_T ssize_t;
#   endif

    /* Socket descriptor type and sentinel */
    typedef SOCKET   ds_socket_t;
    static const ds_socket_t DS_INVALID_SOCK = INVALID_SOCKET;

    /* off_t is absent on MSVC and only 32-bit in the MinGW CRT; use int64_t */
    //typedef int64_t  off_t;

    /* close() on a socket must be closesocket() on Windows */
#   define sock_close(fd)   closesocket(fd)

    /* POSIX shutdown flag isn't defined by Winsock2 */
#   ifndef SHUT_RDWR
#       define SHUT_RDWR SD_BOTH
#   endif

    /* setsockopt() on Windows requires const char* for the option value;
       POSIX takes const void*.  Use this macro for all option-value arguments. */
#   define SOCK_OPT(x)  (reinterpret_cast<const char*>(&(x)))

#else   /* POSIX */

#   include <sys/types.h>
#   include <sys/socket.h>
#   include <sys/ioctl.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <arpa/inet.h>
#   include <netdb.h>
#   include <unistd.h>

    typedef int      ds_socket_t;
    static const ds_socket_t DS_INVALID_SOCK = -1;

#   define sock_close(fd)   close(fd)
#   define SOCK_OPT(x)      (&(x))

#endif  /* _WIN32 */

#endif  /* _DS_COMPAT_H */
