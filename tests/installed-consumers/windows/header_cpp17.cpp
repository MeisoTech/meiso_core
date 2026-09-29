#include <meiso/adapters/windows/tcp.h>
#include <meiso/adapters/windows/display_ingress_media_udp.h>
#include <cstddef>
#include <type_traits>

static_assert(alignof(MeisoMaxAlign) == alignof(std::max_align_t));
static_assert(std::is_standard_layout_v<MeisoWindowsTcpConfig>);
static_assert(std::is_standard_layout_v<MeisoDisplayIngressMediaWindowsUdpConfig>);
extern "C" int meiso_windows_cpp_headers(void)
{
    return meiso_windows_tcp_alignment_required() == alignof(MeisoMaxAlign) ? 0 : 1;
}
