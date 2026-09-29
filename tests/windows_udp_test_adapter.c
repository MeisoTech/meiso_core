#include <winsock2.h>

extern int meiso_test_send_error;
extern int meiso_test_recv_error;
extern int meiso_test_short_send;

static int test_send(SOCKET s, const char *bytes, int size, int flags)
{
    if (meiso_test_send_error != 0) {
        WSASetLastError(meiso_test_send_error);
        meiso_test_send_error = 0;
        return SOCKET_ERROR;
    }
    if (meiso_test_short_send) {
        meiso_test_short_send = 0;
        return size - 1;
    }
    return send(s, bytes, size, flags);
}

static int test_recv(SOCKET s, char *bytes, int size, int flags)
{
    if (meiso_test_recv_error != 0) {
        WSASetLastError(meiso_test_recv_error);
        meiso_test_recv_error = 0;
        return SOCKET_ERROR;
    }
    return recv(s, bytes, size, flags);
}

/* Test-only syscall injection; the production target has no hooks. */
#define send test_send
#define recv test_recv
#include "../adapters/windows/display_ingress_media_windows_udp.c"
