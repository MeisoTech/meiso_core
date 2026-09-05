#ifndef MEISO_DISPLAY_INGRESS_SIMULATED_SERVICE_PRIVATE_H
#define MEISO_DISPLAY_INGRESS_SIMULATED_SERVICE_PRIVATE_H

#include <meiso/adapters/posix/display_ingress_media_udp.h>
#include <meiso/adapters/posix/tcp.h>
#include <meiso/profiles/display_ingress/profile.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define SERVICE_PRIVATE_API __attribute__((visibility("hidden")))
#else
#define SERVICE_PRIVATE_API
#endif

enum {
    SERVICE_SESSION_ARENA_BYTES = 65536,
    SERVICE_TCP_ARENA_BYTES = 65536,
    SERVICE_RUNTIME_ARENA_BYTES = 65536,
    SERVICE_STATE_ARENA_BYTES = 2048,
    SERVICE_MEDIA_ARENA_BYTES = 32768,
    SERVICE_UDP_ARENA_BYTES = 4096,
    SERVICE_CONTROL_IO_BYTES = 4096,
    SERVICE_FRAME_WIDTH = 64,
    SERVICE_FRAME_HEIGHT = 32,
    SERVICE_FRAME_BYTES = SERVICE_FRAME_WIDTH * SERVICE_FRAME_HEIGHT * 4,
    SERVICE_POLL_SLICE_MS = 20,
    SERVICE_TIMEOUT_MS = 5000,
    SERVICE_CATALOG_GENERATION = 1,
    SERVICE_STATE_VERSION = 1,
    SERVICE_DESIRED_DELIVERY_GENERATION = 7,
    SERVICE_ACTUAL_DELIVERY_GENERATION = 8,
    SERVICE_PRODUCTION_EPOCH = 1,
    SERVICE_FRAME_SEQUENCE = 1,
    SERVICE_EVIDENCE_STREAM_EPOCH = 3,
    SERVICE_EVIDENCE_STREAM_SEQUENCE = 1,
    SERVICE_SOURCE_CLOCK_EPOCH = 4
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[SERVICE_SESSION_ARENA_BYTES];
} ServiceSessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[SERVICE_TCP_ARENA_BYTES];
} ServiceTcpArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[SERVICE_RUNTIME_ARENA_BYTES];
} ServiceRuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[SERVICE_STATE_ARENA_BYTES];
} ServiceStateArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[SERVICE_MEDIA_ARENA_BYTES];
} ServiceMediaArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[SERVICE_UDP_ARENA_BYTES];
} ServiceUdpArena;

typedef uint8_t ServiceControlStep;
enum {
    SERVICE_CONTROL_IDLE = 1,
    SERVICE_CONTROL_EVENT = 2,
    SERVICE_CONTROL_PEER_CLOSED = 3,
    SERVICE_CONTROL_FAILED = 4
};

typedef struct {
    int control_fd;
    int media_fd;
    MeisoSessionRole role;
    const char *role_name;
    const char *failure_plane;
    const char *failure_stage;
    ServiceSessionArena session_arena;
    ServiceTcpArena tcp_arena;
    ServiceRuntimeArena runtime_arena;
    ServiceStateArena state_arena;
    ServiceMediaArena media_arena;
    ServiceUdpArena udp_arena;
    MeisoSession *session;
    MeisoPosixTcp *tcp;
    MeisoRuntime *runtime;
    MeisoStateReplica *state_replica;
    MeisoDisplayIngressMediaTransport *media;
    MeisoDisplayIngressMediaPosixUdp *udp;
    MeisoOwnerScope host_owner;
    MeisoOwnerScope edge_owner;
    MeisoDisplayIngressCapability capability;
    uint8_t capability_wire[MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE];
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeV4RouteDescriptor routes[
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ];
    MeisoRuntimeDesiredState applied_desired;
    uint8_t applied_desired_profile[
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE
    ];
    MeisoDisplayIngressMediaBinding binding;
    MeisoSessionId session_id;
    uint8_t frame[SERVICE_FRAME_BYTES];
    uint8_t input_sha256[32U];
    uint8_t received_sha256[32U];
    uint8_t evidence_sha256[32U];
    uint64_t catalog_generation;
    uint64_t applied_desired_version;
    uint64_t actual_version;
    uint64_t production_epoch;
    uint64_t media_stream_epoch;
    uint64_t frame_sequence;
    bool runtime_ready;
    bool catalog_active;
    bool binding_ready;
    bool frame_complete;
    bool evidence_queued;
    bool evidence_complete;
    bool running_actual_queued;
    bool udp_armed_before_actual_write;
    bool media_terminated;
    bool descriptors_closed;
    bool close_clean;
} Service;

SERVICE_PRIVATE_API bool service_fail(
    Service *service,
    const char *plane,
    const char *stage
);
SERVICE_PRIVATE_API bool service_bootstrap(Service *service);
SERVICE_PRIVATE_API bool service_initialize_runtime(Service *service);
SERVICE_PRIVATE_API bool service_arm_media(Service *service);
SERVICE_PRIVATE_API void service_close_fd(int *fd);
SERVICE_PRIVATE_API uint64_t service_now_ns(void);
SERVICE_PRIVATE_API uint64_t service_deadline_after_ms(uint32_t milliseconds);
SERVICE_PRIVATE_API bool service_deadline_expired(uint64_t deadline_ns);
SERVICE_PRIVATE_API bool service_random_bytes(void *output, size_t output_size);

SERVICE_PRIVATE_API bool service_session_tx_idle(const Service *service);
SERVICE_PRIVATE_API bool service_wait_session_ready(Service *service);
SERVICE_PRIVATE_API ServiceControlStep service_control_step(
    Service *service,
    bool allow_write,
    int timeout_ms,
    MeisoRuntimeEvent *out_event
);
SERVICE_PRIVATE_API bool service_flush_control(Service *service);
SERVICE_PRIVATE_API MeisoRuntimeRouteKey service_route_key(
    const Service *service,
    MeisoDisplayIngressProfileRoute route
);
SERVICE_PRIVATE_API bool service_validate_staged_catalog(Service *service);

SERVICE_PRIVATE_API bool service_run_host(Service *service);
SERVICE_PRIVATE_API bool service_run_edge(Service *service);
SERVICE_PRIVATE_API bool service_run_host_media(Service *service);
SERVICE_PRIVATE_API bool service_run_edge_media(Service *service);
SERVICE_PRIVATE_API bool service_fill_and_verify_frame(Service *service);
SERVICE_PRIVATE_API bool service_verify_rgba_sha256(
    MeisoByteView rgba,
    uint8_t out_digest[32U]
);
SERVICE_PRIVATE_API bool service_terminate_media(
    Service *service,
    MeisoDisplayIngressMediaTerminalReason reason
);
SERVICE_PRIVATE_API bool service_finish_local_close(Service *service);
SERVICE_PRIVATE_API bool service_finish_peer_close(Service *service);
SERVICE_PRIVATE_API bool service_consume_optional_accept(
    Service *service,
    const MeisoRuntimeEvent *event
);
SERVICE_PRIVATE_API void service_cleanup_failure(Service *service);
SERVICE_PRIVATE_API bool service_run(Service *service);

SERVICE_PRIVATE_API bool service_emit_failure(const Service *service);
SERVICE_PRIVATE_API bool service_success_ready(const Service *service);
SERVICE_PRIVATE_API bool service_emit_success(const Service *service);

#endif
