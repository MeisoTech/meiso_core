#define _POSIX_C_SOURCE 200809L

#include <meiso/adapters/posix/display_ingress_media_udp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef send
#undef send
#endif
#ifdef recvmsg
#undef recvmsg
#endif

extern ssize_t send(int, const void *, size_t, int);
extern ssize_t recvmsg(int, struct msghdr *, int);

#define TEST_ARENA_CAPACITY 8192U
#define TEST_FRAME_BYTES 32U
#define TEST_FRAGMENT_BYTES 8U

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_ARENA_CAPACITY];
} TestArena;

typedef struct {
    MeisoDisplayIngressMediaTransportConfig host_transport_config;
    MeisoDisplayIngressMediaTransportConfig edge_transport_config;
    MeisoDisplayIngressMediaPosixUdpConfig host_adapter_config;
    MeisoDisplayIngressMediaPosixUdpConfig edge_adapter_config;
    TestArena host_transport_arena;
    TestArena edge_transport_arena;
    TestArena host_adapter_arena;
    TestArena edge_adapter_arena;
    size_t host_transport_bytes;
    size_t edge_transport_bytes;
    size_t host_adapter_bytes;
    size_t edge_adapter_bytes;
    MeisoDisplayIngressMediaTransport *host_transport;
    MeisoDisplayIngressMediaTransport *edge_transport;
    MeisoDisplayIngressMediaPosixUdp *host_adapter;
    MeisoDisplayIngressMediaPosixUdp *edge_adapter;
    int host_fd;
    int edge_fd;
} UdpPair;

static int injected_send_errno;
static int injected_recv_errno;
static bool injected_short_send;

/* Test-only compile redirection for deterministic syscall outcomes. */
ssize_t meiso_test_send(int fd, const void *bytes, size_t size, int flags)
{
    if (injected_send_errno != 0) {
        errno = injected_send_errno;
        injected_send_errno = 0;
        return -1;
    }
    if (injected_short_send) {
        injected_short_send = false;
        return size == 0U ? 0 : (ssize_t)(size - 1U);
    }
    return send(fd, bytes, size, flags);
}

ssize_t meiso_test_recvmsg(int fd, struct msghdr *message, int flags)
{
    if (injected_recv_errno != 0) {
        errno = injected_recv_errno;
        injected_recv_errno = 0;
        return -1;
    }
    return recvmsg(fd, message, flags);
}

static bool binding_equal(
    const MeisoDisplayIngressMediaBinding *lhs,
    const MeisoDisplayIngressMediaBinding *rhs
)
{
    return meiso_session_id_equal(
               lhs->runtime.session_id, rhs->runtime.session_id
           ) &&
        lhs->runtime.route.catalog_generation ==
            rhs->runtime.route.catalog_generation &&
        meiso_endpoint_id_equal(
            lhs->runtime.route.endpoint_id,
            rhs->runtime.route.endpoint_id
        ) &&
        lhs->runtime.route.endpoint_incarnation ==
            rhs->runtime.route.endpoint_incarnation &&
        meiso_resource_id_equal(
            lhs->runtime.route.resource_id,
            rhs->runtime.route.resource_id
        ) &&
        lhs->runtime.route.facet == rhs->runtime.route.facet &&
        meiso_resource_descriptor_equal(
            &lhs->runtime.resource, &rhs->runtime.resource
        ) &&
        lhs->runtime.production_epoch == rhs->runtime.production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low &&
        meiso_owner_scope_equal(lhs->producer, rhs->producer);
}

static MeisoDisplayIngressMediaTransportConfig transport_config(
    MeisoSessionRole role,
    uint32_t width,
    uint32_t height,
    uint32_t fragment_bytes
)
{
    MeisoDisplayIngressMediaTransportConfig value;
    const uint32_t frame_bytes = width * height * 4U;

    memset(&value, 0, sizeof(value));
    value.local_role = role;
    value.input_width = width;
    value.input_height = height;
    value.max_fragment_payload_bytes = fragment_bytes;
    value.deadline_policy = MEISO_STREAM_DEADLINE_UNSUPPORTED;
    value.binding.runtime.session_id.host_nonce =
        (MeisoSessionNonce){11U, 12U};
    value.binding.runtime.session_id.edge_nonce =
        (MeisoSessionNonce){13U, 14U};
    value.binding.runtime.route.catalog_generation = 17U;
    value.binding.runtime.route.endpoint_id = *meiso_display_ingress_endpoint();
    value.binding.runtime.route.endpoint_incarnation = 19U;
    value.binding.runtime.route.resource_id =
        *meiso_display_ingress_resource_id(
            MEISO_DISPLAY_INGRESS_ROUTE_MEDIA
        );
    value.binding.runtime.route.facet = MEISO_RUNTIME_FACET_DATA;
    value.binding.runtime.resource.id =
        value.binding.runtime.route.resource_id;
    value.binding.runtime.resource.kind = MEISO_RESOURCE_KIND_STREAM;
    value.binding.runtime.resource.schema =
        *meiso_display_ingress_resource_schema(
            MEISO_DISPLAY_INGRESS_ROUTE_MEDIA
        );
    value.binding.runtime.resource.owner = MEISO_OWNER_ROLE_HOST;
    value.binding.runtime.resource.max_payload_size = frame_bytes;
    value.binding.runtime.production_epoch = 23U;
    value.binding.media_stream_epoch = 23U;
    value.binding.channel_nonce = (MeisoSessionNonce){31U, 37U};
    value.binding.producer =
        (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 41U};
    return value;
}

static bool make_loopback_pair(int *out_host, int *out_edge)
{
    struct sockaddr_in host_address;
    struct sockaddr_in edge_address;
    socklen_t address_size = (socklen_t)sizeof(struct sockaddr_in);
    int host = -1;
    int edge = -1;
    int flags;

    host = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(host >= 0);
    edge = socket(AF_INET, SOCK_DGRAM, 0);
    if (edge < 0) {
        (void)close(host);
        CHECK(false);
    }
    memset(&host_address, 0, sizeof(host_address));
    host_address.sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &host_address.sin_addr) == 1);
    host_address.sin_port = 0U;
    edge_address = host_address;
    CHECK(bind(
              host, (const struct sockaddr *)(const void *)&host_address,
              address_size
          ) == 0);
    CHECK(bind(
              edge, (const struct sockaddr *)(const void *)&edge_address,
              address_size
          ) == 0);
    CHECK(getsockname(
              host, (struct sockaddr *)(void *)&host_address, &address_size
          ) == 0);
    address_size = (socklen_t)sizeof(edge_address);
    CHECK(getsockname(
              edge, (struct sockaddr *)(void *)&edge_address, &address_size
          ) == 0);
    CHECK(connect(
              host, (const struct sockaddr *)(const void *)&edge_address,
              address_size
          ) == 0);
    CHECK(connect(
              edge, (const struct sockaddr *)(const void *)&host_address,
              (socklen_t)sizeof(host_address)
          ) == 0);
    flags = fcntl(host, F_GETFL);
    CHECK(flags >= 0 && fcntl(host, F_SETFL, flags | O_NONBLOCK) == 0);
    flags = fcntl(edge, F_GETFL);
    CHECK(flags >= 0 && fcntl(edge, F_SETFL, flags | O_NONBLOCK) == 0);
    *out_host = host;
    *out_edge = edge;
    return true;
}

static bool make_ipv6_loopback_pair(int *out_host, int *out_edge)
{
    struct sockaddr_in6 host_address;
    struct sockaddr_in6 edge_address;
    socklen_t address_size = (socklen_t)sizeof(struct sockaddr_in6);
    int host = -1;
    int edge = -1;
    int flags;

    host = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(host >= 0);
    edge = socket(AF_INET6, SOCK_DGRAM, 0);
    if (edge < 0) {
        (void)close(host);
        CHECK(false);
    }
    memset(&host_address, 0, sizeof(host_address));
    host_address.sin6_family = AF_INET6;
    host_address.sin6_addr = in6addr_loopback;
    host_address.sin6_port = 0U;
    edge_address = host_address;
    CHECK(bind(
              host, (const struct sockaddr *)(const void *)&host_address,
              address_size
          ) == 0);
    CHECK(bind(
              edge, (const struct sockaddr *)(const void *)&edge_address,
              address_size
          ) == 0);
    CHECK(getsockname(
              host, (struct sockaddr *)(void *)&host_address, &address_size
          ) == 0);
    address_size = (socklen_t)sizeof(edge_address);
    CHECK(getsockname(
              edge, (struct sockaddr *)(void *)&edge_address, &address_size
          ) == 0);
    CHECK(connect(
              host, (const struct sockaddr *)(const void *)&edge_address,
              address_size
          ) == 0);
    CHECK(connect(
              edge, (const struct sockaddr *)(const void *)&host_address,
              (socklen_t)sizeof(host_address)
          ) == 0);
    flags = fcntl(host, F_GETFL);
    CHECK(flags >= 0 && fcntl(host, F_SETFL, flags | O_NONBLOCK) == 0);
    flags = fcntl(edge, F_GETFL);
    CHECK(flags >= 0 && fcntl(edge, F_SETFL, flags | O_NONBLOCK) == 0);
    *out_host = host;
    *out_edge = edge;
    return true;
}

static bool initialize_scoped_sized_pair(
    UdpPair *pair,
    uint32_t width,
    uint32_t height,
    uint32_t fragment_bytes,
    uint64_t scope_tag
)
{
    size_t alignment = 0U;

    memset(pair, 0, sizeof(*pair));
    pair->host_fd = -1;
    pair->edge_fd = -1;
    pair->host_transport_config = transport_config(
        MEISO_SESSION_ROLE_HOST, width, height, fragment_bytes
    );
    pair->edge_transport_config = transport_config(
        MEISO_SESSION_ROLE_EDGE, width, height, fragment_bytes
    );
    pair->host_transport_config.binding.runtime.session_id.host_nonce.low +=
        scope_tag;
    pair->host_transport_config.binding.runtime.session_id.edge_nonce.low +=
        scope_tag;
    pair->edge_transport_config.binding =
        pair->host_transport_config.binding;
    CHECK(meiso_display_ingress_media_transport_memory_required(
              &pair->host_transport_config, &pair->host_transport_bytes,
              &alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(alignment == alignof(max_align_t) &&
          pair->host_transport_bytes <= TEST_ARENA_CAPACITY);
    CHECK(meiso_display_ingress_media_transport_memory_required(
              &pair->edge_transport_config, &pair->edge_transport_bytes,
              &alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(pair->edge_transport_bytes == pair->host_transport_bytes);
    CHECK(meiso_display_ingress_media_transport_init(
              &pair->host_transport_config,
              (MeisoMutableBuffer){
                  pair->host_transport_arena.bytes,
                  pair->host_transport_bytes
              },
              &pair->host_transport
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_init(
              &pair->edge_transport_config,
              (MeisoMutableBuffer){
                  pair->edge_transport_arena.bytes,
                  pair->edge_transport_bytes
              },
              &pair->edge_transport
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(make_loopback_pair(&pair->host_fd, &pair->edge_fd));
    pair->host_adapter_config = (MeisoDisplayIngressMediaPosixUdpConfig){
        pair->host_fd,
        pair->host_transport,
        pair->host_transport_config.binding
    };
    pair->edge_adapter_config = (MeisoDisplayIngressMediaPosixUdpConfig){
        pair->edge_fd,
        pair->edge_transport,
        pair->edge_transport_config.binding
    };
    pair->host_adapter_bytes =
        meiso_display_ingress_media_posix_udp_memory_required(
            &pair->host_adapter_config
        );
    pair->edge_adapter_bytes =
        meiso_display_ingress_media_posix_udp_memory_required(
            &pair->edge_adapter_config
        );
    if (fragment_bytes <=
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES) {
        CHECK(pair->host_adapter_bytes != 0U &&
              pair->host_adapter_bytes <= TEST_ARENA_CAPACITY &&
              pair->edge_adapter_bytes == pair->host_adapter_bytes);
    } else {
        CHECK(pair->host_adapter_bytes == 0U &&
              pair->edge_adapter_bytes == 0U);
    }
    return true;
}

static bool initialize_sized_pair(
    UdpPair *pair,
    uint32_t width,
    uint32_t height,
    uint32_t fragment_bytes
)
{
    return initialize_scoped_sized_pair(
        pair, width, height, fragment_bytes, 0U
    );
}

static bool initialize_pair(UdpPair *pair, uint32_t fragment_bytes)
{
    return initialize_sized_pair(pair, 4U, 2U, fragment_bytes);
}

static bool initialize_adapters(UdpPair *pair)
{
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair->host_adapter_arena.bytes, pair->host_adapter_bytes,
              &pair->host_adapter_config, &pair->host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_OK);
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair->edge_adapter_arena.bytes, pair->edge_adapter_bytes,
              &pair->edge_adapter_config, &pair->edge_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_OK);
    return true;
}

static void close_pair(UdpPair *pair)
{
    if (pair->host_fd >= 0) {
        (void)close(pair->host_fd);
        pair->host_fd = -1;
    }
    if (pair->edge_fd >= 0) {
        (void)close(pair->edge_fd);
        pair->edge_fd = -1;
    }
}

static void fill_frame_sized(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t offset;

    for (offset = 0U; offset < size; offset += 4U) {
        bytes[offset] = seed++;
        bytes[offset + 1U] = seed++;
        bytes[offset + 2U] = seed++;
        bytes[offset + 3U] = UINT8_MAX;
    }
}

static void fill_frame(uint8_t *bytes, uint8_t seed)
{
    fill_frame_sized(bytes, TEST_FRAME_BYTES, seed);
}

static MeisoDisplayIngressMediaFrame frame_value_sized(
    const uint8_t *bytes,
    size_t size,
    uint64_t sequence,
    uint64_t drops
)
{
    MeisoDisplayIngressMediaFrame frame;

    memset(&frame, 0, sizeof(frame));
    frame.frame_sequence = sequence;
    frame.source_time = (MeisoTimePoint){43U, 100U + sequence};
    frame.producer_dropped_frames = drops;
    frame.rgba_frame = (MeisoByteView){bytes, size};
    return frame;
}

static MeisoDisplayIngressMediaFrame frame_value(
    const uint8_t *bytes,
    uint64_t sequence,
    uint64_t drops
)
{
    return frame_value_sized(bytes, TEST_FRAME_BYTES, sequence, drops);
}

static bool wait_readable(int fd)
{
    struct pollfd descriptor = {fd, POLLIN, 0};
    return poll(&descriptor, 1U, 1000) > 0 &&
        (descriptor.revents & POLLIN) != 0;
}

static bool send_one(UdpPair *pair)
{
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair->host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair->host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT);
    return true;
}

static bool test_init_fd_alias_roles_and_datagram_limit(void)
{
    UdpPair pair;
    TestArena before;
    TestArena transport_before;
    MeisoDisplayIngressMediaPosixUdp *sentinel =
        (MeisoDisplayIngressMediaPosixUdp *)(uintptr_t)0x1234U;
    MeisoDisplayIngressMediaPosixUdp *out = sentinel;
    MeisoDisplayIngressMediaPosixUdpConfig invalid;
    MeisoDisplayIngressMediaPosixUdpSnapshot snapshot;
    int flags;
    int descriptor_flags;
    int unconnected = -1;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(meiso_display_ingress_media_posix_udp_alignment_required() ==
          alignof(max_align_t));
    flags = fcntl(pair.host_fd, F_GETFL);
    descriptor_flags = fcntl(pair.host_fd, F_GETFD);
    CHECK(flags >= 0 && descriptor_flags >= 0);

    memset(&pair.host_adapter_arena, 0xa5, sizeof(pair.host_adapter_arena));
    before = pair.host_adapter_arena;
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes,
              pair.host_adapter_bytes - 1U,
              &pair.host_adapter_config, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_ARENA_TOO_SMALL);
    CHECK(out == sentinel &&
          memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes + 1U,
              pair.host_adapter_bytes,
              &pair.host_adapter_config, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_MISALIGNED_ARENA);
    CHECK(out == sentinel &&
          memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &pair.host_adapter_config,
              (MeisoDisplayIngressMediaPosixUdp **)(void *)
                  pair.host_adapter_arena.bytes
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_transport_arena.bytes,
              pair.host_transport_bytes,
              &pair.host_adapter_config, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_ARGUMENT);

    invalid = pair.host_adapter_config;
    ++invalid.binding.runtime.production_epoch;
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &invalid, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_BINDING_MISMATCH);
    CHECK(memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);

    CHECK(fcntl(pair.host_fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &pair.host_adapter_config, &out
          ) ==
          MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_NONBLOCKING_REQUIRED);
    CHECK(fcntl(pair.host_fd, F_SETFL, flags) == 0);
    unconnected = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(unconnected >= 0);
    invalid = pair.host_adapter_config;
    invalid.connected_fd = unconnected;
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &invalid, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_FD_ERROR);
    CHECK(close(unconnected) == 0);

    CHECK(initialize_adapters(&pair));
    CHECK(fcntl(pair.host_fd, F_GETFL) == flags &&
          fcntl(pair.host_fd, F_GETFD) == descriptor_flags);
    CHECK(!meiso_display_ingress_media_posix_udp_wants_read(
        pair.host_adapter
    ));
    CHECK(!meiso_display_ingress_media_posix_udp_wants_write(
        pair.host_adapter
    ));
    CHECK(meiso_display_ingress_media_posix_udp_wants_read(
        pair.edge_adapter
    ));
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.edge_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_INVALID_STATE);
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.host_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INVALID_STATE);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter, &snapshot
    ));
    CHECK(snapshot.arena_bytes == pair.host_adapter_bytes &&
          binding_equal(&snapshot.binding,
                        &pair.host_transport_config.binding));
    CHECK(!meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter,
        (MeisoDisplayIngressMediaPosixUdpSnapshot *)(void *)
            pair.host_adapter_arena.bytes
    ));
    transport_before = pair.host_transport_arena;
    CHECK(!meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter,
        (MeisoDisplayIngressMediaPosixUdpSnapshot *)(void *)
            pair.host_transport_arena.bytes
    ));
    CHECK(memcmp(&transport_before, &pair.host_transport_arena,
                 sizeof(transport_before)) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_terminate(
              pair.host_adapter,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE,
              0
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_OK);
    CHECK(fcntl(pair.host_fd, F_GETFL) == flags &&
          fcntl(pair.host_fd, F_GETFD) == descriptor_flags);

    close_pair(&pair);

    CHECK(initialize_pair(
        &pair,
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES + 1U
    ));
    CHECK(meiso_display_ingress_media_posix_udp_memory_required(
              &pair.host_adapter_config
          ) == 0U);
    out = sentinel;
    CHECK(meiso_display_ingress_media_posix_udp_init(
              pair.host_adapter_arena.bytes,
              sizeof(pair.host_adapter_arena.bytes),
              &pair.host_adapter_config, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_CONFIG);
    CHECK(out == sentinel);
    close_pair(&pair);
    return true;
}

static bool test_exact_1200_byte_datagram_success(void)
{
    UdpPair pair;
    uint8_t frame_bytes[
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES
    ];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaPosixUdpSnapshot snapshot;

    CHECK(initialize_sized_pair(
        &pair, 244U, 1U,
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES
    ));
    CHECK(initialize_adapters(&pair));
    fill_frame_sized(frame_bytes, sizeof(frame_bytes), 1U);
    frame = frame_value_sized(frame_bytes, sizeof(frame_bytes), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter, &snapshot
    ));
    CHECK(snapshot.tx_packet_borrowed &&
          snapshot.tx_packet.packet_bytes ==
              MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES);
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED);
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge_transport, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.rgba_frame.size == sizeof(frame_bytes) &&
          memcmp(latest.frame.rgba_frame.data, frame_bytes,
                 sizeof(frame_bytes)) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter, &snapshot
    ));
    CHECK(snapshot.datagrams_sent == 1U);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &snapshot
    ));
    CHECK(snapshot.datagrams_received == 1U &&
          snapshot.datagrams_rejected == 0U &&
          snapshot.datagrams_truncated == 0U);
    close_pair(&pair);
    return true;
}

static bool test_ipv6_loopback_success(void)
{
    UdpPair pair;
    uint8_t frame_bytes[TEST_FRAME_BYTES];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    close_pair(&pair);
    CHECK(make_ipv6_loopback_pair(&pair.host_fd, &pair.edge_fd));
    pair.host_adapter_config.connected_fd = pair.host_fd;
    pair.edge_adapter_config.connected_fd = pair.edge_fd;
    pair.host_adapter_bytes =
        meiso_display_ingress_media_posix_udp_memory_required(
            &pair.host_adapter_config
        );
    pair.edge_adapter_bytes =
        meiso_display_ingress_media_posix_udp_memory_required(
            &pair.edge_adapter_config
        );
    CHECK(pair.host_adapter_bytes != 0U &&
          pair.edge_adapter_bytes == pair.host_adapter_bytes);
    CHECK(initialize_adapters(&pair));
    fill_frame(frame_bytes, 7U);
    frame = frame_value(frame_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(send_one(&pair));
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED);
    CHECK(send_one(&pair));
    CHECK(send_one(&pair));
    CHECK(send_one(&pair));
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED);
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge_transport, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.rgba_frame.size == TEST_FRAME_BYTES &&
          memcmp(latest.frame.rgba_frame.data, frame_bytes,
                 TEST_FRAME_BYTES) == 0);
    close_pair(&pair);
    return true;
}

static bool test_fresh_transport_and_udp_tuple_contract(void)
{
    UdpPair first;
    UdpPair second;
    TestArena before;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaTransportSnapshot transport_snapshot;
    MeisoDisplayIngressMediaPosixUdp *sentinel =
        (MeisoDisplayIngressMediaPosixUdp *)(uintptr_t)0x1234U;
    MeisoDisplayIngressMediaPosixUdp *out = sentinel;
    uint8_t frame_bytes[TEST_FRAME_BYTES];
    struct sockaddr_in first_local;
    struct sockaddr_in first_peer;
    struct sockaddr_in second_local;
    struct sockaddr_in second_peer;
    socklen_t address_size;

    CHECK(initialize_scoped_sized_pair(
        &first, 4U, 2U, TEST_FRAGMENT_BYTES, 0U
    ));
    CHECK(initialize_scoped_sized_pair(
        &second, 4U, 2U, TEST_FRAGMENT_BYTES, 100U
    ));
    CHECK(!meiso_session_id_equal(
        first.host_transport_config.binding.runtime.session_id,
        second.host_transport_config.binding.runtime.session_id
    ));
    address_size = (socklen_t)sizeof(first_local);
    CHECK(getsockname(
              first.host_fd, (struct sockaddr *)(void *)&first_local,
              &address_size
          ) == 0);
    address_size = (socklen_t)sizeof(first_peer);
    CHECK(getpeername(
              first.host_fd, (struct sockaddr *)(void *)&first_peer,
              &address_size
          ) == 0);
    address_size = (socklen_t)sizeof(second_local);
    CHECK(getsockname(
              second.host_fd, (struct sockaddr *)(void *)&second_local,
              &address_size
          ) == 0);
    address_size = (socklen_t)sizeof(second_peer);
    CHECK(getpeername(
              second.host_fd, (struct sockaddr *)(void *)&second_peer,
              &address_size
          ) == 0);
    CHECK(first_local.sin_family == AF_INET &&
          first_peer.sin_family == AF_INET &&
          second_local.sin_family == AF_INET &&
          second_peer.sin_family == AF_INET);
    CHECK(first_local.sin_port != second_local.sin_port &&
          first_peer.sin_port != second_peer.sin_port);

    CHECK(initialize_adapters(&first));
    fill_frame(frame_bytes, 1U);
    frame = frame_value(frame_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              second.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_snapshot(
              second.host_transport, &transport_snapshot
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(transport_snapshot.send_in_flight &&
          transport_snapshot.submitted_sequence_high_water == 1U);
    CHECK(meiso_display_ingress_media_posix_udp_memory_required(
              &second.host_adapter_config
          ) == 0U);
    memset(&second.host_adapter_arena, 0xa5,
           sizeof(second.host_adapter_arena));
    before = second.host_adapter_arena;
    CHECK(meiso_display_ingress_media_posix_udp_init(
              second.host_adapter_arena.bytes,
              sizeof(second.host_adapter_arena.bytes),
              &second.host_adapter_config, &out
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_CONFIG);
    CHECK(out == sentinel &&
          memcmp(&before, &second.host_adapter_arena, sizeof(before)) == 0);

    close_pair(&second);
    close_pair(&first);
    return true;
}

static bool test_no_ack_full_frame_loopback_and_syscall_retry(void)
{
    UdpPair pair;
    uint8_t frame_bytes[TEST_FRAME_BYTES];
    uint8_t adapter_before[TEST_ARENA_CAPACITY];
    uint8_t transport_before[TEST_ARENA_CAPACITY];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaPosixUdpSnapshot snapshot;
    MeisoDisplayIngressMediaTransportSnapshot transport_snapshot;
    size_t packet_index;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    fill_frame(frame_bytes, 1U);
    frame = frame_value(frame_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
    CHECK(meiso_display_ingress_media_posix_udp_wants_write(
        pair.host_adapter
    ));
    memcpy(adapter_before, pair.host_adapter_arena.bytes,
           pair.host_adapter_bytes);
    memcpy(transport_before, pair.host_transport_arena.bytes,
           pair.host_transport_bytes);
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);
    injected_send_errno = EAGAIN;
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_WOULD_BLOCK);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);
    injected_send_errno = EINTR;
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INTERRUPTED);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);

    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT);
    for (packet_index = 1U; packet_index < 4U; ++packet_index) {
        CHECK(send_one(&pair));
    }
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_IDLE);
    CHECK(meiso_display_ingress_media_transport_snapshot(
              pair.host_transport, &transport_snapshot
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(!transport_snapshot.send_in_flight &&
          !transport_snapshot.send_latest);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter, &snapshot
    ));
    CHECK(snapshot.datagrams_sent == 4U &&
          snapshot.datagrams_received == 0U);

    injected_recv_errno = EAGAIN;
    memcpy(adapter_before, pair.edge_adapter_arena.bytes,
           pair.edge_adapter_bytes);
    memcpy(transport_before, pair.edge_transport_arena.bytes,
           pair.edge_transport_bytes);
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_WOULD_BLOCK);
    CHECK(memcmp(adapter_before, pair.edge_adapter_arena.bytes,
                 pair.edge_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.edge_transport_arena.bytes,
                 pair.edge_transport_bytes) == 0);
    injected_recv_errno = EINTR;
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INTERRUPTED);
    CHECK(memcmp(adapter_before, pair.edge_adapter_arena.bytes,
                 pair.edge_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.edge_transport_arena.bytes,
                 pair.edge_transport_bytes) == 0);

    for (packet_index = 0U; packet_index < 4U; ++packet_index) {
        CHECK(wait_readable(pair.edge_fd));
        CHECK(meiso_display_ingress_media_posix_udp_on_readable(
                  pair.edge_adapter, NULL
              ) == (packet_index == 3U
                      ? MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED
                      : MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED));
    }
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge_transport, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.frame_sequence == 1U &&
          latest.frame.rgba_frame.size == TEST_FRAME_BYTES &&
          memcmp(latest.frame.rgba_frame.data, frame_bytes,
                 TEST_FRAME_BYTES) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &snapshot
    ));
    CHECK(snapshot.datagrams_received == 4U &&
          snapshot.datagrams_rejected == 0U &&
          snapshot.datagrams_truncated == 0U);

    close_pair(&pair);
    return true;
}

static bool test_dropped_old_fragment_does_not_block_recovery_frame(void)
{
    UdpPair pair;
    uint8_t first_bytes[TEST_FRAME_BYTES];
    uint8_t latest_bytes[TEST_FRAME_BYTES];
    uint8_t discarded[256U];
    MeisoDisplayIngressMediaFrame first;
    MeisoDisplayIngressMediaFrame latest_frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaTransportSnapshot transport_snapshot;
    ssize_t discarded_size;
    size_t packet_index;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    fill_frame(first_bytes, 1U);
    fill_frame(latest_bytes, 101U);
    first = frame_value(first_bytes, 1U, 0U);
    latest_frame = frame_value(latest_bytes, 2U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &first, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(send_one(&pair));
    CHECK(wait_readable(pair.edge_fd));
    discarded_size = recv(
        pair.edge_fd, discarded, sizeof(discarded), 0
    );
    CHECK(discarded_size ==
          (ssize_t)(MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE +
                    TEST_FRAGMENT_BYTES));

    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &latest_frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    for (packet_index = 0U; packet_index < 4U; ++packet_index) {
        CHECK(send_one(&pair));
    }
    CHECK(meiso_display_ingress_media_transport_snapshot(
              pair.host_transport, &transport_snapshot
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(transport_snapshot.transport_whole_frames_dropped == 1U &&
          !transport_snapshot.send_in_flight);

    for (packet_index = 0U; packet_index < 4U; ++packet_index) {
        CHECK(wait_readable(pair.edge_fd));
        CHECK(meiso_display_ingress_media_posix_udp_on_readable(
                  pair.edge_adapter, NULL
              ) == (packet_index == 3U
                      ? MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED
                      : MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED));
    }
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge_transport, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.frame_sequence == 2U &&
          memcmp(latest.frame.rgba_frame.data, latest_bytes,
                 TEST_FRAME_BYTES) == 0);
    CHECK(meiso_display_ingress_media_transport_snapshot(
              pair.edge_transport, &transport_snapshot
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(transport_snapshot.sequence_gap_frames == 1U &&
          transport_snapshot.published_sequence_high_water == 2U);

    close_pair(&pair);
    return true;
}

static bool test_reordered_and_duplicate_datagrams_recover(void)
{
    UdpPair pair;
    uint8_t first_bytes[TEST_FRAME_BYTES];
    uint8_t recovery_bytes[TEST_FRAME_BYTES];
    uint8_t packets[4U][256U];
    size_t packet_sizes[4U];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaPosixUdpSnapshot adapter_snapshot;
    MeisoDisplayIngressMediaTransportSnapshot transport_snapshot;
    ssize_t transferred;
    size_t packet_index;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    fill_frame(first_bytes, 1U);
    frame = frame_value(first_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    for (packet_index = 0U; packet_index < 4U; ++packet_index) {
        CHECK(send_one(&pair));
        CHECK(wait_readable(pair.edge_fd));
        transferred = recv(
            pair.edge_fd, packets[packet_index],
            sizeof(packets[packet_index]), 0
        );
        CHECK(transferred ==
              (ssize_t)(MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE +
                        TEST_FRAGMENT_BYTES));
        packet_sizes[packet_index] = (size_t)transferred;
    }

    transferred = send(
        pair.host_fd, packets[2U], packet_sizes[2U], 0
    );
    CHECK(transferred == (ssize_t)packet_sizes[2U]);
    transferred = send(
        pair.host_fd, packets[0U], packet_sizes[0U], 0
    );
    CHECK(transferred == (ssize_t)packet_sizes[0U]);
    transferred = send(
        pair.host_fd, packets[0U], packet_sizes[0U], 0
    );
    CHECK(transferred == (ssize_t)packet_sizes[0U]);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_REJECTED);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_REJECTED);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &adapter_snapshot
    ));
    CHECK(adapter_snapshot.datagrams_received == 3U &&
          adapter_snapshot.datagrams_rejected == 2U &&
          adapter_snapshot.datagrams_truncated == 0U &&
          !adapter_snapshot.terminal);
    CHECK(meiso_display_ingress_media_transport_snapshot(
              pair.edge_transport, &transport_snapshot
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(!transport_snapshot.receive_reassembly &&
          transport_snapshot.reassembly_frames_dropped == 1U &&
          transport_snapshot.published_sequence_high_water == 0U);

    fill_frame(recovery_bytes, 101U);
    frame = frame_value(recovery_bytes, 2U, 1U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    for (packet_index = 0U; packet_index < 4U; ++packet_index) {
        CHECK(send_one(&pair));
    }
    for (packet_index = 0U; packet_index < 4U; ++packet_index) {
        CHECK(wait_readable(pair.edge_fd));
        CHECK(meiso_display_ingress_media_posix_udp_on_readable(
                  pair.edge_adapter, NULL
              ) == (packet_index == 3U
                      ? MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED
                      : MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED));
    }
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge_transport, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.frame_sequence == 2U &&
          memcmp(latest.frame.rgba_frame.data, recovery_bytes,
                 TEST_FRAME_BYTES) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &adapter_snapshot
    ));
    CHECK(adapter_snapshot.datagrams_received == 7U &&
          adapter_snapshot.datagrams_rejected == 2U &&
          !adapter_snapshot.terminal);
    close_pair(&pair);
    return true;
}

static bool test_truncation_rejection_and_stale_scope_fail_closed(void)
{
    UdpPair pair;
    uint8_t oversized[
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES + 1U
    ];
    uint8_t frame_bytes[TEST_FRAME_BYTES];
    uint8_t stale_packet[256U];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaPosixUdpSnapshot adapter_snapshot;
    MeisoDisplayIngressMediaTransportSnapshot transport_snapshot;
    ssize_t packet_size;
    ssize_t sent;
    int descriptor_flags;
    int error_number = -1;
    int flags;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    flags = fcntl(pair.edge_fd, F_GETFL);
    descriptor_flags = fcntl(pair.edge_fd, F_GETFD);
    CHECK(flags >= 0 && descriptor_flags >= 0);
    memset(oversized, 0x5a, sizeof(oversized));
    sent = send(pair.host_fd, oversized, sizeof(oversized), 0);
    CHECK(sent == (ssize_t)sizeof(oversized));
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRUNCATED);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &adapter_snapshot
    ));
    CHECK(adapter_snapshot.datagrams_received == 1U &&
          adapter_snapshot.datagrams_truncated == 1U &&
          adapter_snapshot.datagrams_rejected == 0U &&
          !adapter_snapshot.receive_status_present);
    CHECK(meiso_display_ingress_media_transport_snapshot(
              pair.edge_transport, &transport_snapshot
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(!transport_snapshot.receive_reassembly &&
          !transport_snapshot.receive_latest);

    fill_frame(frame_bytes, 1U);
    frame = frame_value(frame_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(send_one(&pair));
    CHECK(wait_readable(pair.edge_fd));
    packet_size = recv(
        pair.edge_fd, stale_packet, sizeof(stale_packet), 0
    );
    CHECK(packet_size ==
          (ssize_t)(MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE +
                    TEST_FRAGMENT_BYTES));
    stale_packet[16U] ^= UINT8_C(0x01);
    sent = send(pair.host_fd, stale_packet, (size_t)packet_size, 0);
    CHECK(sent == packet_size);
    CHECK(wait_readable(pair.edge_fd));
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &adapter_snapshot
    ));
    CHECK(adapter_snapshot.terminal &&
          adapter_snapshot.terminal_reason ==
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING &&
          adapter_snapshot.datagrams_received == 2U &&
          adapter_snapshot.datagrams_rejected == 1U &&
          adapter_snapshot.datagrams_truncated == 1U &&
          adapter_snapshot.last_receive_status ==
              MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH);
    CHECK(meiso_display_ingress_media_posix_udp_last_error(
        pair.edge_adapter, &error_number
    ));
    CHECK(error_number == 0);
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_terminate(
              pair.edge_adapter,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE,
              EIO
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_ALREADY);
    CHECK(meiso_display_ingress_media_posix_udp_last_error(
        pair.edge_adapter, &error_number
    ));
    CHECK(error_number == 0);
    CHECK(fcntl(pair.edge_fd, F_GETFL) == flags &&
          fcntl(pair.edge_fd, F_GETFD) == descriptor_flags);

    close_pair(&pair);
    return true;
}

static bool test_short_send_and_io_error_are_sticky_terminal(void)
{
    UdpPair pair;
    uint8_t frame_bytes[TEST_FRAME_BYTES];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaPosixUdpSnapshot before;
    MeisoDisplayIngressMediaPosixUdpSnapshot after;
    int descriptor_flags;
    int error_number = 0;
    int flags;

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    flags = fcntl(pair.host_fd, F_GETFL);
    descriptor_flags = fcntl(pair.host_fd, F_GETFD);
    CHECK(flags >= 0 && descriptor_flags >= 0);
    fill_frame(frame_bytes, 1U);
    frame = frame_value(frame_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
    injected_short_send = true;
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter, &before
    ));
    CHECK(before.terminal && before.last_error == EIO &&
          before.datagrams_sent == 0U && !before.tx_packet_borrowed);
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TERMINAL);
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_terminate(
              pair.host_adapter,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE,
              ENETDOWN
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_ALREADY);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.host_adapter, &after
    ));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_posix_udp_last_error(
        pair.host_adapter, &error_number
    ));
    CHECK(error_number == EIO);
    CHECK(fcntl(pair.host_fd, F_GETFL) == flags &&
          fcntl(pair.host_fd, F_GETFD) == descriptor_flags);
    close_pair(&pair);

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    frame = frame_value(frame_bytes, 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
    injected_send_errno = EMSGSIZE;
    CHECK(meiso_display_ingress_media_posix_udp_on_writable(
              pair.host_adapter
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_last_error(
        pair.host_adapter, &error_number
    ));
    CHECK(error_number == EMSGSIZE);
    CHECK(strcmp(meiso_display_ingress_media_posix_udp_error_name(
                     error_number
                 ), "EMSGSIZE") == 0);
    close_pair(&pair);

    CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
    CHECK(initialize_adapters(&pair));
    flags = fcntl(pair.edge_fd, F_GETFL);
    descriptor_flags = fcntl(pair.edge_fd, F_GETFD);
    CHECK(flags >= 0 && descriptor_flags >= 0);
    injected_recv_errno = ENOTCONN;
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &before
    ));
    CHECK(before.terminal && before.last_error == ENOTCONN &&
          before.receive_errors_dropped == 0U &&
          before.datagrams_received == 0U &&
          before.datagrams_rejected == 0U &&
          before.datagrams_truncated == 0U);
    CHECK(meiso_display_ingress_media_posix_udp_on_readable(
              pair.edge_adapter, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST);
    CHECK(meiso_display_ingress_media_posix_udp_terminate(
              pair.edge_adapter,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE,
              EIO
          ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_ALREADY);
    CHECK(meiso_display_ingress_media_posix_udp_snapshot(
        pair.edge_adapter, &after
    ));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(fcntl(pair.edge_fd, F_GETFL) == flags &&
          fcntl(pair.edge_fd, F_GETFD) == descriptor_flags);
    close_pair(&pair);
    return true;
}

/*
 * meiso_core #45: the network's errors lose one datagram, never the
 * transport. Every class, both directions; the next frame still arrives.
 */
static bool test_network_errors_drop_one_datagram(void)
{
    static const struct {
        int error_number;
        const char *name;
    } classes[MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_CLASS_COUNT] = {
        { EHOSTUNREACH, "EHOSTUNREACH" }, { ENETUNREACH, "ENETUNREACH" },
        { EHOSTDOWN, "EHOSTDOWN" }, { ENETDOWN, "ENETDOWN" },
        { ECONNREFUSED, "ECONNREFUSED" }, { ENOBUFS, "ENOBUFS" },
        { EPERM, "EPERM" }
    };
    UdpPair pair;
    uint8_t frame_bytes[TEST_FRAME_BYTES];
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaPosixUdpSnapshot snapshot;
    MeisoDisplayIngressMediaPosixUdpDropClass drop_class;
    MeisoDisplayIngressMediaPosixUdpReadStatus read;
    size_t index;
    size_t packet_index;
    size_t reads;
    int error_number = 0;

    CHECK(!meiso_display_ingress_media_posix_udp_drop_class(0, &drop_class));
    CHECK(!meiso_display_ingress_media_posix_udp_drop_class(
        EMSGSIZE, &drop_class
    ));
    CHECK(meiso_display_ingress_media_posix_udp_error_name(-1) == NULL);
    CHECK(meiso_display_ingress_media_posix_udp_drop_class_error(
              MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_CLASS_COUNT
          ) == 0);
    for (index = 0U;
         index < MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_CLASS_COUNT;
         ++index) {
        CHECK(meiso_display_ingress_media_posix_udp_drop_class(
            classes[index].error_number, &drop_class
        ));
        CHECK(drop_class == index);
        CHECK(meiso_display_ingress_media_posix_udp_drop_class_error(
                  drop_class
              ) == classes[index].error_number);
        CHECK(strcmp(meiso_display_ingress_media_posix_udp_error_name(
                         classes[index].error_number
                     ), classes[index].name) == 0);

        CHECK(initialize_pair(&pair, TEST_FRAGMENT_BYTES));
        CHECK(initialize_adapters(&pair));

        /* Sending: the first fragment is lost, the other three go. */
        fill_frame(frame_bytes, 1U);
        frame = frame_value(frame_bytes, 1U, 0U);
        CHECK(meiso_display_ingress_media_transport_submit(
                  pair.host_transport, &frame, NULL
              ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
                  pair.host_adapter
              ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY);
        injected_send_errno = classes[index].error_number;
        CHECK(meiso_display_ingress_media_posix_udp_on_writable(
                  pair.host_adapter
              ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_DROPPED);
        CHECK(meiso_display_ingress_media_posix_udp_snapshot(
            pair.host_adapter, &snapshot
        ));
        CHECK(!snapshot.terminal && !snapshot.tx_packet_borrowed &&
              snapshot.datagrams_sent == 0U &&
              snapshot.datagrams_dropped == 1U &&
              snapshot.drops_by_class[index] == 1U &&
              snapshot.last_drop_error == classes[index].error_number &&
              snapshot.last_error == 0);
        CHECK(!meiso_display_ingress_media_posix_udp_last_error(
            pair.host_adapter, &error_number
        ));
        for (packet_index = 1U; packet_index < 4U; ++packet_index) {
            CHECK(send_one(&pair));
        }
        CHECK(meiso_display_ingress_media_posix_udp_prepare_outbound(
                  pair.host_adapter
              ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_IDLE);

        /* Receiving: the error comes instead of a datagram. */
        injected_recv_errno = classes[index].error_number;
        CHECK(meiso_display_ingress_media_posix_udp_on_readable(
                  pair.edge_adapter, NULL
              ) == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_DROPPED);
        CHECK(meiso_display_ingress_media_posix_udp_snapshot(
            pair.edge_adapter, &snapshot
        ));
        CHECK(!snapshot.terminal && snapshot.datagrams_received == 0U &&
              snapshot.receive_errors_dropped == 1U &&
              snapshot.drops_by_class[index] == 1U &&
              snapshot.last_drop_error == classes[index].error_number);

        /* The next frame arrives whole behind what is left of the first. */
        fill_frame(frame_bytes, 2U);
        frame = frame_value(frame_bytes, 2U, 0U);
        CHECK(meiso_display_ingress_media_transport_submit(
                  pair.host_transport, &frame, NULL
              ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        for (packet_index = 0U; packet_index < 4U; ++packet_index) {
            CHECK(send_one(&pair));
        }
        read = MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_WOULD_BLOCK;
        for (reads = 0U; reads < 7U &&
             read != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED;
             ++reads) {
            CHECK(wait_readable(pair.edge_fd));
            read = meiso_display_ingress_media_posix_udp_on_readable(
                pair.edge_adapter, NULL
            );
            CHECK(read !=
                      MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST &&
                  read !=
                      MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INVALID_STATE);
        }
        CHECK(read == MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED);
        CHECK(meiso_display_ingress_media_transport_latest(
                  pair.edge_transport, &latest
              ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        CHECK(latest.frame.frame_sequence == 2U &&
              memcmp(latest.frame.rgba_frame.data, frame_bytes,
                     TEST_FRAME_BYTES) == 0);
        close_pair(&pair);
    }
    return true;
}

int main(void)
{
    if (!test_init_fd_alias_roles_and_datagram_limit() ||
        !test_exact_1200_byte_datagram_success() ||
        !test_ipv6_loopback_success() ||
        !test_fresh_transport_and_udp_tuple_contract() ||
        !test_no_ack_full_frame_loopback_and_syscall_retry() ||
        !test_dropped_old_fragment_does_not_block_recovery_frame() ||
        !test_reordered_and_duplicate_datagrams_recover() ||
        !test_truncation_rejection_and_stale_scope_fail_closed() ||
        !test_short_send_and_io_error_are_sticky_terminal() ||
        !test_network_errors_drop_one_datagram()) {
        return 1;
    }
    puts("Display Ingress connected UDP behavior tests passed");
    return 0;
}
