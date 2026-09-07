#define _POSIX_C_SOURCE 200809L

#include "display_ingress_simulated_service_private.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const MeisoPeerId HOST_PEER = {
    UINT64_C(0x484f53545f53494d), UINT64_C(0x554c415445445f31)
};
static const MeisoPeerId EDGE_PEER = {
    UINT64_C(0x454447455f53494d), UINT64_C(0x554c415445445f31)
};

bool service_fail(Service *service, const char *plane, const char *stage)
{
    if (service != NULL && service->failure_plane == NULL) {
        service->failure_plane = plane;
        service->failure_stage = stage;
    }
    return false;
}

void service_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

uint64_t service_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

uint64_t service_deadline_after_ms(uint32_t milliseconds)
{
    const uint64_t now = service_now_ns();
    const uint64_t delta = (uint64_t)milliseconds * UINT64_C(1000000);

    if (now == 0U) {
        return 0U;
    }
    return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
}

bool service_deadline_expired(uint64_t deadline_ns)
{
    const uint64_t now = service_now_ns();
    return now == 0U || deadline_ns == 0U || now >= deadline_ns;
}

bool service_random_bytes(void *output, size_t output_size)
{
    uint8_t *bytes = (uint8_t *)output;
    size_t offset = 0U;
    int fd;
    bool ok = false;

    if (output == NULL || output_size == 0U) {
        return false;
    }
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    while (offset < output_size) {
        const ssize_t count = read(fd, bytes + offset, output_size - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            goto done;
        }
    }
    ok = true;

done:
    (void)close(fd);
    return ok;
}

static bool connected_nonblocking_socket_is(int fd, int expected_type)
{
    struct sockaddr_storage peer;
    socklen_t peer_size = (socklen_t)sizeof(peer);
    socklen_t type_size = (socklen_t)sizeof(expected_type);
    int actual_type = 0;
    int flags;

    memset(&peer, 0, sizeof(peer));
    flags = fcntl(fd, F_GETFL);
    return fd >= 3 && flags >= 0 && (flags & O_NONBLOCK) != 0 &&
        getsockopt(
            fd, SOL_SOCKET, SO_TYPE, &actual_type, &type_size
        ) == 0 && actual_type == expected_type &&
        getpeername(fd, (struct sockaddr *)(void *)&peer, &peer_size) == 0 &&
        (peer.ss_family == AF_INET || peer.ss_family == AF_INET6);
}

static bool build_catalog(Service *service)
{
    const MeisoDisplayIngressCatalogConfig config = {
        1U,
        {
            SERVICE_FRAME_WIDTH,
            SERVICE_FRAME_HEIGHT,
            60000U,
            SERVICE_FRAME_BYTES,
            MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES,
            SERVICE_EVIDENCE_MASK,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            SERVICE_FRAME_FORMAT
        }
    };

    service->capability = config.capability;
    if (service->overlay_enabled) return meiso_display_overlay_build_catalog(&config,
        &service->overlay_capability, (MeisoMutableBuffer){service->capability_wire,64},
        service->endpoints,2,service->routes,7);
    return meiso_display_ingress_build_catalog(
        &config,
        (MeisoMutableBuffer){
            service->capability_wire, sizeof(service->capability_wire)
        },
        service->endpoints,
        MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT,
        service->routes,
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ) == MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

static bool initialize_session_and_tcp(Service *service)
{
    MeisoSessionConfig session_config;
    MeisoPosixTcpConfig tcp_config;
    struct {
        uint64_t incarnation;
        MeisoSessionNonce nonce;
    } identity;
    size_t required;
    unsigned int attempt;

    memset(&identity, 0, sizeof(identity));
    for (attempt = 0U; attempt < 4U; ++attempt) {
        if (!service_random_bytes(&identity, sizeof(identity))) {
            return service_fail(service, "application", "bootstrap");
        }
        if (identity.incarnation != 0U &&
            meiso_session_nonce_valid(identity.nonce)) {
            break;
        }
    }
    if (identity.incarnation == 0U ||
        !meiso_session_nonce_valid(identity.nonce)) {
        return service_fail(service, "application", "bootstrap");
    }
    memset(&session_config, 0, sizeof(session_config));
    session_config.local_role = service->role;
    session_config.local_peer_id = service->role == MEISO_SESSION_ROLE_HOST ?
        HOST_PEER : EDGE_PEER;
    session_config.expected_peer_id = service->role == MEISO_SESSION_ROLE_HOST ?
        EDGE_PEER : HOST_PEER;
    session_config.local_incarnation = identity.incarnation;
    session_config.local_nonce = identity.nonce;
    session_config.protocol = (MeisoProtocolVersion){1U, 0U};
    if (service->overlay_enabled ? !meiso_display_overlay_configure_session(&session_config) :
        meiso_display_ingress_configure_session_for_format(&session_config, SERVICE_FRAME_FORMAT) != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        return service_fail(service, "control", "session");
    }
    required = meiso_session_memory_required(&session_config);
    if (required == 0U || required > sizeof(service->session_arena.bytes) ||
        meiso_session_init(
            service->session_arena.bytes,
            required,
            &session_config,
            &service->session
        ) != MEISO_SESSION_INIT_OK ||
        meiso_session_start(service->session) != MEISO_SESSION_START_STARTED) {
        return service_fail(service, "control", "session");
    }
    tcp_config = (MeisoPosixTcpConfig){
        service->control_fd,
        service->session,
        MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES,
        SERVICE_CONTROL_IO_BYTES
    };
    required = meiso_posix_tcp_memory_required(&tcp_config);
    if (required == 0U || required > sizeof(service->tcp_arena.bytes) ||
        meiso_posix_tcp_init(
            service->tcp_arena.bytes,
            required,
            &tcp_config,
            &service->tcp
        ) != MEISO_POSIX_TCP_INIT_OK) {
        return service_fail(service, "control", "session");
    }
    return true;
}

bool service_bootstrap(Service *service)
{
    if (service == NULL || service->role_name == NULL ||
        (service->role != MEISO_SESSION_ROLE_HOST &&
         service->role != MEISO_SESSION_ROLE_EDGE)) {
        return service_fail(service, "application", "bootstrap");
    }
    if (!connected_nonblocking_socket_is(service->control_fd, SOCK_STREAM)) {
        return service_fail(service, "control", "bootstrap");
    }
    if (!connected_nonblocking_socket_is(service->media_fd, SOCK_DGRAM)) {
        return service_fail(service, "media", "bootstrap");
    }
    if (!build_catalog(service)) {
        return service_fail(service, "application", "catalog");
    }
    return initialize_session_and_tcp(service);
}

bool service_initialize_runtime(Service *service)
{
    MeisoRuntimeV4Config runtime_config;
    MeisoSessionSnapshot snapshot;
    const MeisoResourceDescriptor *state_resource;
    MeisoOwnerScope state_owner;
    size_t required;

    if (service == NULL || service->session == NULL || service->runtime_ready ||
        !meiso_session_snapshot(service->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known || !snapshot.remote_identity_known ||
        (service->overlay_enabled ? !meiso_display_overlay_configure_runtime(&runtime_config,service->session) :
        meiso_display_ingress_configure_runtime_v4(&runtime_config, service->session) != MEISO_DISPLAY_INGRESS_PROFILE_OK)) {
        return service_fail(service, "control", "session");
    }
    required = meiso_runtime_v4_memory_required(&runtime_config);
    if (required == 0U || required > sizeof(service->runtime_arena.bytes) ||
        meiso_runtime_v4_init(
            service->runtime_arena.bytes,
            required,
            &runtime_config,
            &service->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return service_fail(service, "control", "session");
    }
    service->session_id = snapshot.session_id;
    if (service->role == MEISO_SESSION_ROLE_HOST) {
        service->host_owner = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_HOST, snapshot.local_incarnation
        };
        service->edge_owner = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_EDGE, snapshot.remote_incarnation
        };
        state_resource = &service->routes[
            MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL
        ].route.resource;
        state_owner = service->edge_owner;
    } else {
        service->host_owner = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_HOST, snapshot.remote_incarnation
        };
        service->edge_owner = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_EDGE, snapshot.local_incarnation
        };
        state_resource = &service->routes[
            MEISO_DISPLAY_INGRESS_ROUTE_DESIRED
        ].route.resource;
        state_owner = service->host_owner;
    }
    required = meiso_state_replica_memory_required(state_resource);
    if (required == 0U || required > sizeof(service->state_arena.bytes) ||
        meiso_state_replica_init(
            service->state_arena.bytes,
            required,
            state_resource,
            &service->state_replica
        ) != MEISO_STATE_INIT_OK ||
        meiso_state_replica_begin_owner(service->state_replica, state_owner) !=
            MEISO_STATE_OWNER_STARTED) {
        return service_fail(service, "application", "state");
    }
    service->runtime_ready = true;
    return true;
}

bool service_arm_media(Service *service)
{
    MeisoDisplayIngressMediaTransportConfig media_config;
    MeisoDisplayIngressMediaPosixUdpConfig udp_config;
    size_t media_required = 0U;
    size_t alignment = 0U;
    size_t udp_required;

    if (service == NULL || !service->binding_ready || service->media != NULL ||
        service->udp != NULL) {
        return service_fail(service, "media", "binding");
    }
    memset(&media_config, 0, sizeof(media_config));
    media_config.local_role = service->role;
    media_config.binding = service->binding;
    media_config.input_width = SERVICE_FRAME_WIDTH;
    media_config.input_height = SERVICE_FRAME_HEIGHT;
    media_config.max_fragment_payload_bytes =
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES;
    media_config.frame_format = SERVICE_FRAME_FORMAT;
    media_config.deadline_policy = MEISO_STREAM_DEADLINE_UNSUPPORTED;
    if (meiso_display_ingress_media_transport_memory_required(
            &media_config, &media_required, &alignment
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        media_required == 0U ||
        media_required > sizeof(service->media_arena.bytes) ||
        meiso_display_ingress_media_transport_init(
            &media_config,
            (MeisoMutableBuffer){
                service->media_arena.bytes, media_required
            },
            &service->media
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return service_fail(service, "media", "binding");
    }
    udp_config = (MeisoDisplayIngressMediaPosixUdpConfig){
        service->media_fd, service->media, service->binding
    };
    udp_required = meiso_display_ingress_media_posix_udp_memory_required(
        &udp_config
    );
    if (udp_required == 0U ||
        udp_required > sizeof(service->udp_arena.bytes) ||
        meiso_display_ingress_media_posix_udp_init(
            service->udp_arena.bytes,
            udp_required,
            &udp_config,
            &service->udp
        ) != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_OK) {
        return service_fail(service, "media", "binding");
    }
    if (service->role == MEISO_SESSION_ROLE_EDGE) {
        service->udp_armed_before_actual_write = true;
    }
    return true;
}
