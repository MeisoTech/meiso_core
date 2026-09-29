#include <winsock2.h>
#include <ws2tcpip.h>
#include <meiso/adapters/windows/display_ingress_media_udp.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return false; } } while (0)
#define UDP(name) meiso_display_ingress_media_windows_udp_##name
#define STATUS(name) MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_##name
#define MEDIA(name) meiso_display_ingress_media_transport_##name
#define MEDIA_OK MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK

int meiso_test_send_error;
int meiso_test_recv_error;
int meiso_test_short_send;

typedef union { MeisoMaxAlign alignment; uint8_t bytes[8192]; } Arena;
typedef struct {
    Arena transport_arena[2], adapter_arena[2];
    MeisoDisplayIngressMediaTransport *transport[2];
    MeisoDisplayIngressMediaWindowsUdp *adapter[2];
    MeisoDisplayIngressMediaWindowsUdpConfig adapter_config[2];
    SOCKET sockets[2];
} Pair;

static MeisoDisplayIngressMediaTransportConfig config(MeisoSessionRole role)
{
    MeisoDisplayIngressMediaTransportConfig c;
    memset(&c, 0, sizeof(c));
    c.local_role = role;
    c.input_width = 4; c.input_height = 2; c.max_fragment_payload_bytes = 8;
    c.deadline_policy = MEISO_STREAM_DEADLINE_UNSUPPORTED;
    c.binding.runtime.session_id = (MeisoSessionId){{11, 12}, {13, 14}};
    c.binding.runtime.route.catalog_generation = 17;
    c.binding.runtime.route.endpoint_id = *meiso_display_ingress_endpoint();
    c.binding.runtime.route.endpoint_incarnation = 19;
    c.binding.runtime.route.resource_id = *meiso_display_ingress_resource_id(MEISO_DISPLAY_INGRESS_ROUTE_MEDIA);
    c.binding.runtime.route.facet = MEISO_RUNTIME_FACET_DATA;
    c.binding.runtime.resource.id = c.binding.runtime.route.resource_id;
    c.binding.runtime.resource.kind = MEISO_RESOURCE_KIND_STREAM;
    c.binding.runtime.resource.schema = *meiso_display_ingress_resource_schema(MEISO_DISPLAY_INGRESS_ROUTE_MEDIA);
    c.binding.runtime.resource.owner = MEISO_OWNER_ROLE_HOST;
    c.binding.runtime.resource.max_payload_size = 32;
    c.binding.runtime.production_epoch = 23;
    c.binding.media_stream_epoch = 23;
    c.binding.channel_nonce = (MeisoSessionNonce){31, 37};
    c.binding.producer = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 41};
    return c;
}

static bool make_pair(Pair *p, int family)
{
    struct sockaddr_storage address[2];
    int sizes[2];
    memset(p, 0, sizeof(*p));
    memset(address, 0, sizeof(address));
    for (unsigned i = 0; i < 2; ++i) {
        p->sockets[i] = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(p->sockets[i] != INVALID_SOCKET);
        if (family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&address[i];
            a->sin_family = AF_INET;
            a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sizes[i] = sizeof(*a);
        } else {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&address[i];
            a->sin6_family = AF_INET6;
            a->sin6_addr = in6addr_loopback;
            sizes[i] = sizeof(*a);
        }
        CHECK(bind(p->sockets[i], (struct sockaddr *)&address[i], sizes[i]) == 0);
        CHECK(getsockname(p->sockets[i], (struct sockaddr *)&address[i], &sizes[i]) == 0);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const MeisoDisplayIngressMediaTransportConfig c = config(i == 0 ? MEISO_SESSION_ROLE_HOST : MEISO_SESSION_ROLE_EDGE);
        CHECK(connect(p->sockets[i], (struct sockaddr *)&address[1-i], sizes[1-i]) == 0);
        CHECK(MEDIA(init)(&c, (MeisoMutableBuffer){p->transport_arena[i].bytes, sizeof(Arena)}, &p->transport[i]) == MEDIA_OK);
        p->adapter_config[i] = (MeisoDisplayIngressMediaWindowsUdpConfig){
            (uintptr_t)p->sockets[i], p->transport[i], c.binding};
    }
    return true;
}

static bool initialize(Pair *p)
{
    for (unsigned i = 0; i < 2; ++i)
        CHECK(UDP(init)(p->adapter_arena[i].bytes, sizeof(Arena), &p->adapter_config[i], &p->adapter[i]) == STATUS(INIT_OK));
    return true;
}

static bool close_pair(Pair *p)
{
    CHECK(closesocket(p->sockets[0]) == 0);
    CHECK(closesocket(p->sockets[1]) == 0);
    return true;
}

static bool wait_readable(SOCKET s)
{
    WSAPOLLFD item = {s, POLLRDNORM, 0};
    return WSAPoll(&item, 1, 1000) == 1 && (item.revents & POLLRDNORM) != 0;
}

static bool submit(Pair *p, uint64_t sequence)
{
    uint8_t pixels[32];
    memset(pixels, 0x5a, sizeof(pixels));
    for (size_t i = 3; i < sizeof(pixels); i += 4) pixels[i] = 0xff;
    MeisoDisplayIngressMediaFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.frame_sequence = sequence;
    frame.source_time = (MeisoTimePoint){43, sequence};
    frame.rgba_frame = (MeisoByteView){pixels, sizeof(pixels)};
    CHECK(MEDIA(submit)(p->transport[0], &frame, NULL) == MEDIA_OK);
    return true;
}

static bool test_roundtrip(int family)
{
    Pair p;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaWindowsUdpSnapshot snapshot;
    uint8_t pixels[32];
    memset(pixels, 0x5a, sizeof(pixels));
    for (size_t i = 3; i < sizeof(pixels); i += 4) pixels[i] = 0xff;
    CHECK(make_pair(&p, family) && initialize(&p));
    /* The sockets started blocking; successful init must make empty reads bounded. */
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_WOULD_BLOCK));
    CHECK(!UDP(wants_read)(p.adapter[0]) && UDP(wants_read)(p.adapter[1]));
    CHECK(UDP(prepare_outbound)(p.adapter[1]) == STATUS(PREPARE_INVALID_STATE));
    CHECK(UDP(on_readable)(p.adapter[0], NULL) == STATUS(READ_INVALID_STATE));
    CHECK(submit(&p, 1));
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(UDP(prepare_outbound)(p.adapter[0]) == STATUS(PREPARE_READY));
        CHECK(UDP(prepare_outbound)(p.adapter[0]) == STATUS(PREPARE_READY));
        CHECK(UDP(wants_write)(p.adapter[0]));
        CHECK(UDP(on_writable)(p.adapter[0]) == STATUS(WRITE_PACKET_SENT));
        CHECK(wait_readable(p.sockets[1]));
        CHECK(UDP(on_readable)(p.adapter[1], NULL) ==
              (i == 3 ? STATUS(READ_FRAME_PUBLISHED) : STATUS(READ_FRAGMENT_ACCEPTED)));
    }
    CHECK(UDP(prepare_outbound)(p.adapter[0]) == STATUS(PREPARE_IDLE));
    CHECK(MEDIA(latest)(p.transport[1], &latest) == MEDIA_OK);
    CHECK(latest.frame.frame_sequence == 1 && latest.frame.rgba_frame.size == sizeof(pixels));
    CHECK(memcmp(latest.frame.rgba_frame.data, pixels, sizeof(pixels)) == 0);
    CHECK(MEDIA(release_latest)(p.transport[1], &latest.ref) == MEDIA_OK);
    CHECK(UDP(snapshot)(p.adapter[1], &snapshot) && snapshot.datagrams_received == 4);
    CHECK(UDP(terminate)(p.adapter[1], MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE, 0) == STATUS(TERMINATE_OK));
    CHECK(!UDP(wants_read)(p.adapter[1]));
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_TRANSPORT_LOST));
    /* Termination borrows the socket; the owner can still query and close it. */
    int type = 0, type_size = sizeof(type);
    CHECK(getsockopt(p.sockets[1], SOL_SOCKET, SO_TYPE, (char *)&type, &type_size) == 0 && type == SOCK_DGRAM);
    return close_pair(&p);
}

static bool test_init_and_truncation(void)
{
    Pair p;
    Arena before;
    MeisoDisplayIngressMediaWindowsUdp *out = (MeisoDisplayIngressMediaWindowsUdp *)(uintptr_t)1;
    MeisoDisplayIngressMediaWindowsUdpConfig bad;
    MeisoDisplayIngressMediaWindowsUdpSnapshot snapshot;
    char oversized[1201] = {0};
    CHECK(make_pair(&p, AF_INET));
    memset(&p.adapter_arena[0], 0xa5, sizeof(Arena)); before = p.adapter_arena[0];
    CHECK(UDP(init)(p.adapter_arena[0].bytes, 1, &p.adapter_config[0], &out) == STATUS(INIT_ARENA_TOO_SMALL));
    CHECK(out == (MeisoDisplayIngressMediaWindowsUdp *)(uintptr_t)1 && memcmp(&before, &p.adapter_arena[0], sizeof(Arena)) == 0);
    CHECK(UDP(init)(p.adapter_arena[0].bytes + 1, sizeof(Arena) - 1, &p.adapter_config[0], &out) == STATUS(INIT_MISALIGNED_ARENA));
    bad = p.adapter_config[0]; bad.binding.channel_nonce.low++;
    CHECK(UDP(init)(p.adapter_arena[0].bytes, sizeof(Arena), &bad, &out) == STATUS(INIT_BINDING_MISMATCH));
    bad = p.adapter_config[0]; bad.connected_socket = (uintptr_t)INVALID_SOCKET;
    CHECK(UDP(memory_required)(&bad) == 0);
    bad = p.adapter_config[0]; bad.connected_socket = (uintptr_t)socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(UDP(init)(p.adapter_arena[0].bytes, sizeof(Arena), &bad, &out) == STATUS(INIT_SOCKET_ERROR));
    CHECK(closesocket((SOCKET)bad.connected_socket) == 0);
    CHECK(initialize(&p));
    CHECK(send(p.sockets[0], oversized, sizeof(oversized), 0) == sizeof(oversized));
    CHECK(wait_readable(p.sockets[1]));
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_TRUNCATED));
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_WOULD_BLOCK));
    CHECK(send(p.sockets[0], oversized, 1200, 0) == 1200);
    CHECK(wait_readable(p.sockets[1]));
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_PACKET_REJECTED));
    CHECK(send(p.sockets[0], oversized, 0, 0) == 0);
    CHECK(wait_readable(p.sockets[1]));
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_PACKET_REJECTED));
    CHECK(UDP(snapshot)(p.adapter[1], &snapshot));
    CHECK(snapshot.datagrams_received == 3 && snapshot.datagrams_truncated == 1 && snapshot.datagrams_rejected == 2 && !snapshot.terminal);
    return close_pair(&p);
}

static bool test_io_failures(void)
{
    Pair p;
    Arena adapter_before, transport_before;
    int error = 0;
    MeisoDisplayIngressMediaTransportSnapshot state;
    CHECK(make_pair(&p, AF_INET) && initialize(&p) && submit(&p, 1));
    CHECK(UDP(prepare_outbound)(p.adapter[0]) == STATUS(PREPARE_READY));
    adapter_before = p.adapter_arena[0]; transport_before = p.transport_arena[0];
    meiso_test_send_error = WSAEWOULDBLOCK;
    CHECK(UDP(on_writable)(p.adapter[0]) == STATUS(WRITE_WOULD_BLOCK));
    meiso_test_send_error = WSAEINTR;
    CHECK(UDP(on_writable)(p.adapter[0]) == STATUS(WRITE_INTERRUPTED));
    CHECK(memcmp(&adapter_before, &p.adapter_arena[0], sizeof(Arena)) == 0);
    CHECK(memcmp(&transport_before, &p.transport_arena[0], sizeof(Arena)) == 0);
    adapter_before = p.adapter_arena[1]; transport_before = p.transport_arena[1];
    meiso_test_recv_error = WSAEINTR;
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_INTERRUPTED));
    meiso_test_recv_error = WSAEWOULDBLOCK;
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_WOULD_BLOCK));
    CHECK(memcmp(&adapter_before, &p.adapter_arena[1], sizeof(Arena)) == 0);
    CHECK(memcmp(&transport_before, &p.transport_arena[1], sizeof(Arena)) == 0);
    meiso_test_short_send = 1;
    CHECK(UDP(on_writable)(p.adapter[0]) == STATUS(WRITE_TRANSPORT_LOST));
    CHECK(UDP(last_error)(p.adapter[0], &error) && error == WSAECONNABORTED);
    CHECK(MEDIA(snapshot)(p.transport[0], &state) == MEDIA_OK && state.phase == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL);
    meiso_test_recv_error = WSAECONNRESET;
    CHECK(UDP(on_readable)(p.adapter[1], NULL) == STATUS(READ_TRANSPORT_LOST));
    CHECK(UDP(last_error)(p.adapter[1], &error) && error == WSAECONNRESET);
    return close_pair(&p);
}

int main(void)
{
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    const bool success = test_roundtrip(AF_INET) && test_roundtrip(AF_INET6) &&
        test_init_and_truncation() && test_io_failures();
    if (WSACleanup() != 0 || !success) return 1;
    puts("Windows DIFR UDP tests passed");
    return 0;
}
