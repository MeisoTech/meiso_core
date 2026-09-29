#ifndef MEISO_WINDOWS_SOCKET_PRIVATE_H
#define MEISO_WINDOWS_SOCKET_PRIVATE_H
#include <winsock2.h>
#include <stdbool.h>

/* Borrow an established IP socket. Winsock has no query for FIONBIO;
 * setting it last makes a successful init bounded regardless of caller mode. */
static bool meiso_windows_socket_prepare(SOCKET socket_handle, int expected_type)
{
    struct sockaddr_storage peer;
    int peer_size = (int)sizeof(peer);
    int socket_type = 0;
    int type_size = (int)sizeof(socket_type);
    u_long nonblocking = 1;
    return socket_handle != INVALID_SOCKET &&
        getsockopt(socket_handle, SOL_SOCKET, SO_TYPE, (char *)&socket_type,
                   &type_size) == 0 && socket_type == expected_type &&
        getpeername(socket_handle, (struct sockaddr *)&peer, &peer_size) == 0 &&
        (peer.ss_family == AF_INET || peer.ss_family == AF_INET6) &&
        ioctlsocket(socket_handle, FIONBIO, &nonblocking) == 0;
}
#endif
