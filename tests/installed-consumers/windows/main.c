#include <winsock2.h>
#include <meiso/adapters/windows/tcp.h>
#include <meiso/adapters/windows/display_ingress_media_udp.h>

extern int meiso_windows_cpp_headers(void);

int main(void)
{
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    const bool valid = meiso_windows_tcp_alignment_required() == _Alignof(MeisoMaxAlign) &&
        meiso_display_ingress_media_windows_udp_alignment_required() == _Alignof(MeisoMaxAlign) &&
        meiso_windows_tcp_memory_required(NULL) == 0 &&
        meiso_display_ingress_media_windows_udp_memory_required(NULL) == 0 &&
        meiso_windows_cpp_headers() == 0;
    return WSACleanup() == 0 && valid ? 0 : 1;
}
