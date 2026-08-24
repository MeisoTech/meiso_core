#define _POSIX_C_SOURCE 200809L

#include "composition_media_posix_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_ARENA_CAPACITY 8192U

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
    MeisoCompositionMediaTransportConfig host_transport_config;
    MeisoCompositionMediaTransportConfig edge_transport_config;
    MeisoCompositionMediaPosixTcpConfig host_adapter_config;
    MeisoCompositionMediaPosixTcpConfig edge_adapter_config;
    TestArena host_transport_arena;
    TestArena edge_transport_arena;
    TestArena host_adapter_arena;
    TestArena edge_adapter_arena;
    size_t host_transport_bytes;
    size_t edge_transport_bytes;
    size_t host_adapter_bytes;
    size_t edge_adapter_bytes;
    MeisoCompositionMediaTransport *host_transport;
    MeisoCompositionMediaTransport *edge_transport;
    MeisoCompositionMediaPosixTcp *host_adapter;
    MeisoCompositionMediaPosixTcp *edge_adapter;
    int host_fd;
    int edge_fd;
} TcpPair;

static int injected_send_errno;
static int injected_recv_errno;

/* Test-only compile redirection for deterministic EINTR/EAGAIN branches. */
ssize_t meiso_test_send(int fd, const void *bytes, size_t size, int flags)
{
    if (injected_send_errno != 0) {
        errno = injected_send_errno;
        injected_send_errno = 0;
        return -1;
    }
    (void)flags;
    return write(fd, bytes, size);
}

ssize_t meiso_test_recv(int fd, void *bytes, size_t size, int flags)
{
    if (injected_recv_errno != 0) {
        errno = injected_recv_errno;
        injected_recv_errno = 0;
        return -1;
    }
    (void)flags;
    return read(fd, bytes, size);
}

static MeisoCompositionMediaTransportConfig transport_config(
    MeisoSessionRole role
)
{
    MeisoCompositionMediaTransportConfig value;
    size_t render_count = 0U;
    size_t camera_count = 0U;
    const MeisoResourceDescriptor *render_resources =
        meiso_composition_render_media_resources(&render_count);
    const MeisoResourceDescriptor *camera_resources =
        meiso_composition_camera_media_resources(&camera_count);
    memset(&value, 0, sizeof(value));
    value.local_role = role;
    value.scope.session_id.host_nonce = (MeisoSessionNonce){11U, 12U};
    value.scope.session_id.edge_nonce = (MeisoSessionNonce){13U, 14U};
    value.scope.catalog_generation = 17U;

    value.routes[0].binding.session_id = value.scope.session_id;
    value.routes[0].binding.catalog_generation = 17U;
    value.routes[0].binding.endpoint_id =
        *meiso_composition_render_media_endpoint();
    value.routes[0].binding.endpoint_incarnation = 21U;
    if (render_resources != NULL && render_count == 3U) {
        value.routes[0].binding.resource_id = render_resources[2].id;
    }
    value.routes[0].binding.source =
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    value.routes[0].binding.production_epoch = 23U;
    value.routes[0].binding.media_stream_epoch = 29U;
    value.routes[0].binding.channel_nonce = (MeisoSessionNonce){31U, 37U};
    value.routes[0].capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        2U, 1U, 2U, 1U, 1000U, 8U, 3U, 0U
    };
    value.routes[0].effective = (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        2U, 1U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };

    value.routes[1].binding.session_id = value.scope.session_id;
    value.routes[1].binding.catalog_generation = 17U;
    value.routes[1].binding.endpoint_id =
        *meiso_composition_camera_media_endpoint();
    value.routes[1].binding.endpoint_incarnation = 22U;
    if (camera_resources != NULL && camera_count == 3U) {
        value.routes[1].binding.resource_id = camera_resources[2].id;
    }
    value.routes[1].binding.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    value.routes[1].binding.production_epoch = 24U;
    value.routes[1].binding.media_stream_epoch = 30U;
    value.routes[1].binding.channel_nonce = (MeisoSessionNonce){41U, 43U};
    value.routes[1].capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        2U, 2U, 2U, 2U, 1000U, 6U, 2U, 0U
    };
    value.routes[1].effective = (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        2U, 2U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    return value;
}

static bool make_loopback_pair(int *out_client, int *out_server)
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int flags;
    int enabled = 1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    CHECK(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     (socklen_t)sizeof(enabled)) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    address.sin_port = 0U;
    CHECK(bind(listener, (const struct sockaddr *)(const void *)&address,
               (socklen_t)sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)(void *)&address,
                      &address_size) == 0);
    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0);
    CHECK(connect(client, (const struct sockaddr *)(const void *)&address,
                  address_size) == 0);
    server = accept(listener, NULL, NULL);
    CHECK(server >= 0);
    CHECK(close(listener) == 0);
    flags = fcntl(client, F_GETFL);
    CHECK(flags >= 0 && fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0);
    flags = fcntl(server, F_GETFL);
    CHECK(flags >= 0 && fcntl(server, F_SETFL, flags | O_NONBLOCK) == 0);
    *out_client = client;
    *out_server = server;
    return true;
}

static bool init_transports_and_sockets(TcpPair *pair)
{
    size_t alignment = 0U;
    memset(pair, 0, sizeof(*pair));
    pair->host_fd = -1;
    pair->edge_fd = -1;
    pair->host_transport_config = transport_config(MEISO_SESSION_ROLE_HOST);
    pair->edge_transport_config = transport_config(MEISO_SESSION_ROLE_EDGE);
    CHECK(meiso_composition_media_transport_memory_required(
              &pair->host_transport_config, &pair->host_transport_bytes,
              &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_memory_required(
              &pair->edge_transport_config, &pair->edge_transport_bytes,
              &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_init(
              &pair->host_transport_config,
              (MeisoMutableBuffer){pair->host_transport_arena.bytes,
                                   pair->host_transport_bytes},
              &pair->host_transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_init(
              &pair->edge_transport_config,
              (MeisoMutableBuffer){pair->edge_transport_arena.bytes,
                                   pair->edge_transport_bytes},
              &pair->edge_transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(make_loopback_pair(&pair->host_fd, &pair->edge_fd));
    pair->host_adapter_config = (MeisoCompositionMediaPosixTcpConfig){
        pair->host_fd, pair->host_transport,
        pair->host_transport_config.scope, 17U
    };
    pair->edge_adapter_config = (MeisoCompositionMediaPosixTcpConfig){
        pair->edge_fd, pair->edge_transport,
        pair->edge_transport_config.scope, 17U
    };
    pair->host_adapter_bytes =
        meiso_composition_media_posix_tcp_memory_required(
            &pair->host_adapter_config
        );
    pair->edge_adapter_bytes =
        meiso_composition_media_posix_tcp_memory_required(
            &pair->edge_adapter_config
        );
    CHECK(pair->host_adapter_bytes != 0U &&
          pair->host_adapter_bytes <= TEST_ARENA_CAPACITY);
    CHECK(pair->edge_adapter_bytes == pair->host_adapter_bytes);
    return true;
}

static bool init_adapters(TcpPair *pair)
{
    CHECK(meiso_composition_media_posix_tcp_init(
              pair->host_adapter_arena.bytes, pair->host_adapter_bytes,
              &pair->host_adapter_config, &pair->host_adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK);
    CHECK(meiso_composition_media_posix_tcp_init(
              pair->edge_adapter_arena.bytes, pair->edge_adapter_bytes,
              &pair->edge_adapter_config, &pair->edge_adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK);
    return true;
}

static void close_pair(TcpPair *pair)
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

static bool write_one_packet(
    MeisoCompositionMediaPosixTcp *sender,
    size_t *out_progress_count
)
{
    size_t progress = 0U;
    CHECK(meiso_composition_media_posix_tcp_prepare_outbound(sender) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY);
    while (true) {
        const MeisoCompositionMediaPosixTcpWriteStatus status =
            meiso_composition_media_posix_tcp_on_writable(sender);
        if (status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT) {
            break;
        }
        CHECK(status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS);
        ++progress;
    }
    if (out_progress_count != NULL) {
        *out_progress_count = progress;
    }
    return true;
}

static bool wait_readable(int fd)
{
    struct pollfd descriptor = {fd, POLLIN, 0};
    return poll(&descriptor, 1U, 1000) > 0;
}

static bool read_one_packet(
    MeisoCompositionMediaPosixTcp *receiver,
    int receiver_fd,
    MeisoCompositionMediaPosixTcpReadStatus expected,
    size_t *out_progress_count
)
{
    size_t progress = 0U;
    while (true) {
        const MeisoCompositionMediaPosixTcpReadStatus status =
            meiso_composition_media_posix_tcp_on_readable(receiver);
        if (status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK) {
            CHECK(wait_readable(receiver_fd));
            continue;
        }
        if (status != MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS) {
            if (status != expected) {
                MeisoCompositionMediaPosixTcpSnapshot debug_snapshot;
                memset(&debug_snapshot, 0, sizeof(debug_snapshot));
                (void)meiso_composition_media_posix_tcp_snapshot(
                    receiver, &debug_snapshot
                );
                fprintf(stderr, "read status %u, expected %u\n",
                        (unsigned)status, (unsigned)expected);
                fprintf(stderr, "rx %zu/%zu terminal=%u error=%d\n",
                        debug_snapshot.rx_bytes,
                        debug_snapshot.rx_expected_bytes,
                        debug_snapshot.terminal ? 1U : 0U,
                        debug_snapshot.last_error);
            }
            CHECK(status == expected);
            break;
        }
        ++progress;
    }
    if (out_progress_count != NULL) {
        *out_progress_count = progress;
    }
    return true;
}

static bool handshake(TcpPair *pair, bool prove_no_overread)
{
    MeisoCompositionMediaTransportSnapshot transport_snapshot;
    MeisoCompositionMediaPosixTcpSnapshot adapter_snapshot;
    size_t write_progress = 0U;
    size_t read_progress = 0U;
    CHECK(meiso_composition_media_posix_tcp_prepare_outbound(
              pair->host_adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY);
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair->host_adapter, &adapter_snapshot
          ));
    CHECK(adapter_snapshot.tx_packet_borrowed &&
          adapter_snapshot.tx_packet_bytes ==
              MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE &&
          adapter_snapshot.tx_bytes_sent == 0U);
    CHECK(meiso_composition_media_posix_tcp_on_writable(pair->host_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS);
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair->host_adapter, &adapter_snapshot
          ));
    CHECK(adapter_snapshot.tx_bytes_sent == 17U);
    CHECK(write_one_packet(pair->host_adapter, &write_progress));
    CHECK(write_progress >= 2U);
    CHECK(wait_readable(pair->edge_fd));
    CHECK(meiso_composition_media_posix_tcp_on_readable(pair->edge_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS);
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair->edge_adapter, &adapter_snapshot
          ));
    CHECK(adapter_snapshot.rx_bytes == 4U &&
          adapter_snapshot.rx_expected_bytes ==
              MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE);
    CHECK(wait_readable(pair->edge_fd));
    CHECK(meiso_composition_media_posix_tcp_on_readable(pair->edge_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS);
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair->edge_adapter, &adapter_snapshot
          ));
    CHECK(adapter_snapshot.rx_bytes > 4U &&
          adapter_snapshot.rx_bytes <= 21U &&
          adapter_snapshot.rx_expected_bytes ==
              MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE);
    CHECK(read_one_packet(
        pair->edge_adapter, pair->edge_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_ACCEPTED,
        &read_progress
    ));
    CHECK(read_progress >= 2U);

    CHECK(write_one_packet(pair->edge_adapter, NULL));
    if (prove_no_overread) {
        CHECK(write_one_packet(pair->edge_adapter, NULL));
    }
    CHECK(read_one_packet(
        pair->host_adapter, pair->host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_ACCEPTED, NULL
    ));
    CHECK(meiso_composition_media_transport_snapshot(
              pair->host_transport, &transport_snapshot
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(transport_snapshot.bound_route_mask == UINT8_C(0x01));
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair->host_adapter, &adapter_snapshot
          ));
    CHECK(adapter_snapshot.rx_bytes == 0U &&
          adapter_snapshot.rx_expected_bytes == 0U);
    if (!prove_no_overread) {
        CHECK(write_one_packet(pair->edge_adapter, NULL));
    }
    CHECK(read_one_packet(
        pair->host_adapter, pair->host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_ACCEPTED, NULL
    ));
    CHECK(meiso_composition_media_transport_snapshot(
              pair->host_transport, &transport_snapshot
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(transport_snapshot.phase ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE);
    CHECK(transport_snapshot.bound_route_mask == UINT8_C(0x03));
    return true;
}

static MeisoCompositionMediaUnit render_unit(
    const TcpPair *pair,
    uint64_t sequence,
    const uint8_t *bytes
)
{
    MeisoCompositionMediaUnit value;
    memset(&value, 0, sizeof(value));
    value.binding = pair->edge_transport_config.routes[0].binding;
    value.frame_sequence = sequence;
    value.source_time = (MeisoTimePoint){7U, sequence * 100U};
    value.applied_scene_owner =
        (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 47U};
    value.applied_scene_state_version = 53U;
    value.active_plan_generation = 59U;
    value.world_generation = 61U;
    value.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    value.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    value.width = 2U;
    value.height = 1U;
    value.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    value.access_unit = (MeisoByteView){bytes, 8U};
    return value;
}

static bool raw_write_all(int fd, const uint8_t *bytes, size_t size)
{
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t sent = sendto(
            fd, bytes + offset, size - offset, 0, NULL, 0U
        );
        if (sent > 0) {
            offset += (size_t)sent;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool test_init_fd_ownership_alias_and_no_resume(void)
{
    TcpPair pair;
    TestArena before;
    MeisoCompositionMediaPosixTcp *adapter =
        (MeisoCompositionMediaPosixTcp *)(uintptr_t)0x1234U;
    MeisoCompositionMediaPosixTcp *adapter_before = adapter;
    MeisoCompositionMediaPosixTcpConfig invalid;
    MeisoCompositionMediaPosixTcpSnapshot snapshot_value;
    int flags_before;
    int flags_after;
    int descriptor_flags_before;
    CHECK(init_transports_and_sockets(&pair));
    CHECK(meiso_composition_media_posix_tcp_alignment_required() ==
          alignof(max_align_t));
    flags_before = fcntl(pair.host_fd, F_GETFL);
    CHECK(flags_before >= 0);
    descriptor_flags_before = fcntl(pair.host_fd, F_GETFD);
    CHECK(descriptor_flags_before >= 0);

    memset(&pair.host_adapter_arena, 0xa5,
           sizeof(pair.host_adapter_arena));
    before = pair.host_adapter_arena;
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes - 1U,
              &pair.host_adapter_config, &adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_ARENA_TOO_SMALL);
    CHECK(adapter == adapter_before &&
          memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_adapter_arena.bytes + 1U,
              pair.host_adapter_bytes,
              &pair.host_adapter_config, &adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_MISALIGNED_ARENA);
    CHECK(adapter == adapter_before &&
          memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &pair.host_adapter_config,
              (MeisoCompositionMediaPosixTcp **)(void *)
                  pair.host_adapter_arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_transport_arena.bytes, pair.host_transport_bytes,
              &pair.host_adapter_config, &adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_ARGUMENT);
    invalid = pair.host_adapter_config;
    ++invalid.scope.catalog_generation;
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &invalid, &adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_SCOPE_MISMATCH);
    CHECK(memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);

    CHECK(fcntl(pair.host_fd, F_SETFL, flags_before & ~O_NONBLOCK) == 0);
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &pair.host_adapter_config, &adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_NONBLOCKING_REQUIRED);
    CHECK(adapter == adapter_before &&
          memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    CHECK((fcntl(pair.host_fd, F_GETFL) & O_NONBLOCK) == 0);
    CHECK(fcntl(pair.host_fd, F_SETFL, flags_before) == 0);

    CHECK(init_adapters(&pair));
    flags_after = fcntl(pair.host_fd, F_GETFL);
    CHECK(flags_after == flags_before);
    CHECK(meiso_composition_media_posix_tcp_wants_read(pair.host_adapter));
    CHECK(!meiso_composition_media_posix_tcp_wants_write(pair.host_adapter));
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair.host_adapter, &snapshot_value
          ));
    CHECK(snapshot_value.arena_bytes == pair.host_adapter_bytes);
    CHECK(!meiso_composition_media_posix_tcp_snapshot(
        pair.host_adapter,
        (MeisoCompositionMediaPosixTcpSnapshot *)(void *)
            pair.host_adapter_arena.bytes
    ));
    CHECK(!meiso_composition_media_posix_tcp_snapshot(
        pair.host_adapter,
        (MeisoCompositionMediaPosixTcpSnapshot *)(void *)
            pair.host_transport_arena.bytes
    ));

    CHECK(handshake(&pair, true));
    CHECK(meiso_composition_media_posix_tcp_memory_required(
              &pair.host_adapter_config
          ) == 0U);
    memset(&pair.host_adapter_arena, 0xa5,
           sizeof(pair.host_adapter_arena));
    before = pair.host_adapter_arena;
    adapter = adapter_before;
    CHECK(meiso_composition_media_posix_tcp_init(
              pair.host_adapter_arena.bytes, pair.host_adapter_bytes,
              &pair.host_adapter_config, &adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_CONFIG);
    CHECK(adapter == adapter_before &&
          memcmp(&before, &pair.host_adapter_arena, sizeof(before)) == 0);
    flags_after = fcntl(pair.host_fd, F_GETFL);
    CHECK(flags_after >= 0);
    CHECK((flags_after & (O_ACCMODE | O_NONBLOCK)) ==
          (flags_before & (O_ACCMODE | O_NONBLOCK)));
    CHECK(fcntl(pair.host_fd, F_GETFD) == descriptor_flags_before);
    close_pair(&pair);
    return true;
}

static bool test_would_block_and_interrupted_raw_zero(void)
{
    TcpPair pair;
    uint8_t adapter_before[TEST_ARENA_CAPACITY];
    uint8_t transport_before[TEST_ARENA_CAPACITY];
    CHECK(init_transports_and_sockets(&pair));
    CHECK(init_adapters(&pair));

    memcpy(adapter_before, pair.edge_adapter_arena.bytes,
           pair.edge_adapter_bytes);
    memcpy(transport_before, pair.edge_transport_arena.bytes,
           pair.edge_transport_bytes);
    CHECK(meiso_composition_media_posix_tcp_on_readable(pair.edge_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK);
    CHECK(memcmp(adapter_before, pair.edge_adapter_arena.bytes,
                 pair.edge_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.edge_transport_arena.bytes,
                 pair.edge_transport_bytes) == 0);
    injected_recv_errno = EINTR;
    CHECK(meiso_composition_media_posix_tcp_on_readable(pair.edge_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED);
    CHECK(memcmp(adapter_before, pair.edge_adapter_arena.bytes,
                 pair.edge_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.edge_transport_arena.bytes,
                 pair.edge_transport_bytes) == 0);

    CHECK(meiso_composition_media_posix_tcp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY);
    memcpy(adapter_before, pair.host_adapter_arena.bytes,
           pair.host_adapter_bytes);
    memcpy(transport_before, pair.host_transport_arena.bytes,
           pair.host_transport_bytes);
    CHECK(meiso_composition_media_posix_tcp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);
    injected_send_errno = EAGAIN;
    CHECK(meiso_composition_media_posix_tcp_on_writable(pair.host_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);
    injected_send_errno = EINTR;
    CHECK(meiso_composition_media_posix_tcp_on_writable(pair.host_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);
    close_pair(&pair);
    return true;
}

static bool test_fragment_reject_is_nonterminal(void)
{
    static const uint8_t payload[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    TcpPair pair;
    MeisoCompositionMediaUnit unit;
    MeisoCompositionMediaFrame overlap;
    uint8_t wire[MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES];
    size_t wire_size = 0U;
    MeisoCompositionMediaPosixTcpSnapshot adapter_snapshot;
    CHECK(init_transports_and_sockets(&pair));
    CHECK(init_adapters(&pair));
    CHECK(handshake(&pair, false));
    unit = render_unit(&pair, 1U, payload);
    CHECK(meiso_composition_media_transport_submit(
              pair.edge_transport, &unit
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(write_one_packet(pair.edge_adapter, NULL));
    CHECK(read_one_packet(
        pair.host_adapter, pair.host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAGMENT_ACCEPTED, NULL
    ));

    memset(&overlap, 0, sizeof(overlap));
    overlap.binding = unit.binding;
    overlap.frame_sequence = unit.frame_sequence;
    overlap.source_time = unit.source_time;
    overlap.applied_scene_owner = unit.applied_scene_owner;
    overlap.applied_scene_state_version = unit.applied_scene_state_version;
    overlap.active_plan_generation = unit.active_plan_generation;
    overlap.world_generation = unit.world_generation;
    overlap.pixel_format = unit.pixel_format;
    overlap.codec = unit.codec;
    overlap.width = unit.width;
    overlap.height = unit.height;
    overlap.access_unit_bytes = 8U;
    overlap.fragment_offset = 1U;
    overlap.fragment = (MeisoByteView){payload + 1U, 3U};
    overlap.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    CHECK(meiso_composition_media_frame_encode(
              &overlap,
              (MeisoMutableBuffer){wire, sizeof(wire)}, &wire_size
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(raw_write_all(pair.edge_fd, wire, wire_size));
    CHECK(read_one_packet(
        pair.host_adapter, pair.host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_REJECTED, NULL
    ));
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair.host_adapter, &adapter_snapshot
          ));
    CHECK(!adapter_snapshot.terminal);
    CHECK(adapter_snapshot.receive_status_present);
    CHECK(adapter_snapshot.last_receive_status ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY);
    CHECK(meiso_composition_media_posix_tcp_wants_read(pair.host_adapter));
    close_pair(&pair);
    return true;
}

static bool test_malformed_eof_and_reset_are_terminal(void)
{
    static const uint8_t payload[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    TcpPair pair;
    MeisoCompositionMediaUnit unit;
    MeisoCompositionMediaFrame frame;
    uint8_t wire[MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES];
    size_t wire_size = 0U;
    MeisoCompositionMediaPosixTcpSnapshot snapshot_value;
    int error_number = -1;
    struct linger reset_linger = {1, 0};

    CHECK(init_transports_and_sockets(&pair));
    CHECK(init_adapters(&pair));
    CHECK(handshake(&pair, false));
    unit = render_unit(&pair, 1U, payload);
    memset(&frame, 0, sizeof(frame));
    frame.binding = unit.binding;
    frame.frame_sequence = 1U;
    frame.source_time = unit.source_time;
    frame.applied_scene_owner = unit.applied_scene_owner;
    frame.applied_scene_state_version = unit.applied_scene_state_version;
    frame.active_plan_generation = unit.active_plan_generation;
    frame.world_generation = unit.world_generation;
    frame.pixel_format = unit.pixel_format;
    frame.codec = unit.codec;
    frame.width = unit.width;
    frame.height = unit.height;
    frame.access_unit_bytes = 8U;
    frame.fragment_offset = 0U;
    frame.fragment = (MeisoByteView){payload, 3U};
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_BEGIN |
        MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    CHECK(meiso_composition_media_frame_encode(
              &frame, (MeisoMutableBuffer){wire, sizeof(wire)}, &wire_size
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    memset(wire + 200U, 0, 4U);
    CHECK(raw_write_all(
        pair.edge_fd, wire, MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE
    ));
    CHECK(read_one_packet(
        pair.host_adapter, pair.host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST, NULL
    ));
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair.host_adapter, &snapshot_value
          ));
    CHECK(snapshot_value.terminal &&
          snapshot_value.terminal_reason ==
              MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST);
    CHECK(meiso_composition_media_posix_tcp_last_error(
              pair.host_adapter, &error_number
          ));
    CHECK(error_number == EPROTO);
    close_pair(&pair);

    CHECK(init_transports_and_sockets(&pair));
    CHECK(init_adapters(&pair));
    CHECK(close(pair.edge_fd) == 0);
    pair.edge_fd = -1;
    CHECK(read_one_packet(
        pair.host_adapter, pair.host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST, NULL
    ));
    CHECK(meiso_composition_media_posix_tcp_last_error(
              pair.host_adapter, &error_number
          ));
    CHECK(error_number == 0);
    CHECK(fcntl(pair.host_fd, F_GETFL) >= 0);
    close_pair(&pair);

    CHECK(init_transports_and_sockets(&pair));
    CHECK(init_adapters(&pair));
    CHECK(setsockopt(pair.edge_fd, SOL_SOCKET, SO_LINGER, &reset_linger,
                     (socklen_t)sizeof(reset_linger)) == 0);
    CHECK(close(pair.edge_fd) == 0);
    pair.edge_fd = -1;
    CHECK(read_one_packet(
        pair.host_adapter, pair.host_fd,
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST, NULL
    ));
    CHECK(meiso_composition_media_posix_tcp_last_error(
              pair.host_adapter, &error_number
          ));
    CHECK(error_number == 0 || error_number == ECONNRESET);
    CHECK(fcntl(pair.host_fd, F_GETFL) >= 0);
    close_pair(&pair);
    return true;
}

static bool test_explicit_termination_does_not_close_fd(void)
{
    TcpPair pair;
    MeisoCompositionMediaPosixTcpSnapshot snapshot_value;
    int flags;
    int error_number = -1;
    uint8_t adapter_before[TEST_ARENA_CAPACITY];
    uint8_t transport_before[TEST_ARENA_CAPACITY];
    CHECK(init_transports_and_sockets(&pair));
    CHECK(init_adapters(&pair));
    flags = fcntl(pair.host_fd, F_GETFL);
    CHECK(flags >= 0);
    CHECK(meiso_composition_media_posix_tcp_terminate(
              pair.host_adapter,
              MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE, 71
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_OK);
    CHECK(meiso_composition_media_posix_tcp_terminate(
              pair.host_adapter,
              MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE, 71
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_ALREADY);
    CHECK(meiso_composition_media_posix_tcp_snapshot(
              pair.host_adapter, &snapshot_value
          ));
    CHECK(snapshot_value.terminal);
    CHECK(!meiso_composition_media_posix_tcp_wants_read(pair.host_adapter));
    CHECK(!meiso_composition_media_posix_tcp_wants_write(pair.host_adapter));
    CHECK(meiso_composition_media_posix_tcp_last_error(
              pair.host_adapter, &error_number
          ));
    CHECK(error_number == 71);
    CHECK(fcntl(pair.host_fd, F_GETFL) == flags);
    memcpy(adapter_before, pair.host_adapter_arena.bytes,
           pair.host_adapter_bytes);
    memcpy(transport_before, pair.host_transport_arena.bytes,
           pair.host_transport_bytes);
    CHECK(meiso_composition_media_posix_tcp_prepare_outbound(
              pair.host_adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL);
    CHECK(meiso_composition_media_posix_tcp_on_readable(pair.host_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST);
    CHECK(meiso_composition_media_posix_tcp_on_writable(pair.host_adapter) ==
          MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST);
    CHECK(memcmp(adapter_before, pair.host_adapter_arena.bytes,
                 pair.host_adapter_bytes) == 0);
    CHECK(memcmp(transport_before, pair.host_transport_arena.bytes,
                 pair.host_transport_bytes) == 0);
    CHECK(!meiso_composition_media_posix_tcp_last_error(
        pair.host_adapter,
        (int *)(void *)pair.host_adapter_arena.bytes
    ));
    CHECK(!meiso_composition_media_posix_tcp_last_error(
        pair.host_adapter,
        (int *)(void *)pair.host_transport_arena.bytes
    ));
    close_pair(&pair);
    return true;
}

int main(void)
{
    if (!test_init_fd_ownership_alias_and_no_resume() ||
        !test_would_block_and_interrupted_raw_zero() ||
        !test_fragment_reject_is_nonterminal() ||
        !test_malformed_eof_and_reset_are_terminal() ||
        !test_explicit_termination_does_not_close_fd()) {
        return 1;
    }
    puts("composition media POSIX TCP tests passed");
    return 0;
}
