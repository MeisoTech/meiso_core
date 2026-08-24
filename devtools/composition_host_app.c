#define _POSIX_C_SOURCE 200809L

#include "composition_host_app.h"

#include "composition_coordinator.h"
#include "scene_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>

enum {
    HOST_APP_MAGIC = 0x48415050U, /* HAPP */
    HOST_MEDIA_ADAPTER_RESERVE_BYTES = 16384U,
    HOST_DESIRED_SCRATCH_BYTES =
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE,

    ROUTE_WIFI_DESIRED = 0,
    ROUTE_WIFI_ACTUAL = 1,
    ROUTE_WIFI_INLINE = 2,
    ROUTE_PRESENTATION_DESIRED = 3,
    ROUTE_PRESENTATION_ACTUAL = 4,
    ROUTE_SCENE_DESIRED = 5,
    ROUTE_SCENE_ACTUAL = 6,
    ROUTE_MESH_BLOB = 7,
    ROUTE_RENDER_INLINE = 8,
    ROUTE_DEVICE_DESIRED = 9,
    ROUTE_DEVICE_ACTUAL = 10,
    ROUTE_DEVICE_INLINE = 11,
    ROUTE_RENDER_MEDIA_DESIRED = 12,
    ROUTE_RENDER_MEDIA_ACTUAL = 13,
    ROUTE_RENDER_MEDIA_DATA = 14,
    ROUTE_CAMERA_MEDIA_DESIRED = 15,
    ROUTE_CAMERA_MEDIA_ACTUAL = 16,
    ROUTE_CAMERA_MEDIA_DATA = 17,

    SOURCE_CATALOG = 0,
    SOURCE_WIFI_DESIRED = 1,
    SOURCE_WIFI_ACTUAL_ACCEPT = 2,
    SOURCE_BLOB_BEGIN = 3,
    SOURCE_BLOB_CHUNK = 4,
    SOURCE_BLOB_COMMIT = 5,
    SOURCE_BLOB_ABORT = 6,
    SOURCE_PRESENTATION_DESIRED = 7,
    SOURCE_PRESENTATION_ACTUAL_ACCEPT = 8,
    SOURCE_SCENE_DESIRED = 9,
    SOURCE_SCENE_ACTUAL_ACCEPT = 10,
    SOURCE_DEVICE_DESIRED = 11,
    SOURCE_DEVICE_ACTUAL_ACCEPT = 12,
    SOURCE_RENDER_MEDIA_DESIRED = 13,
    SOURCE_RENDER_MEDIA_ACTUAL_ACCEPT = 14,
    SOURCE_CAMERA_MEDIA_DESIRED = 15,
    SOURCE_CAMERA_MEDIA_ACTUAL_ACCEPT = 16
};

static const uint8_t DESIRED_ROUTE_INDEX[
    MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT
] = {
    ROUTE_WIFI_DESIRED,
    ROUTE_PRESENTATION_DESIRED,
    ROUTE_SCENE_DESIRED,
    ROUTE_DEVICE_DESIRED,
    ROUTE_RENDER_MEDIA_DESIRED,
    ROUTE_CAMERA_MEDIA_DESIRED
};

static const uint8_t DESIRED_SOURCE_INDEX[
    MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT
] = {
    SOURCE_WIFI_DESIRED,
    SOURCE_PRESENTATION_DESIRED,
    SOURCE_SCENE_DESIRED,
    SOURCE_DEVICE_DESIRED,
    SOURCE_RENDER_MEDIA_DESIRED,
    SOURCE_CAMERA_MEDIA_DESIRED
};

static const uint8_t ACTUAL_ROUTE_INDEX[
    MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT
] = {
    ROUTE_WIFI_ACTUAL,
    ROUTE_PRESENTATION_ACTUAL,
    ROUTE_SCENE_ACTUAL,
    ROUTE_DEVICE_ACTUAL,
    ROUTE_RENDER_MEDIA_ACTUAL,
    ROUTE_CAMERA_MEDIA_ACTUAL
};

static const uint8_t ACTUAL_SOURCE_INDEX[
    MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT
] = {
    SOURCE_WIFI_ACTUAL_ACCEPT,
    SOURCE_PRESENTATION_ACTUAL_ACCEPT,
    SOURCE_SCENE_ACTUAL_ACCEPT,
    SOURCE_DEVICE_ACTUAL_ACCEPT,
    SOURCE_RENDER_MEDIA_ACTUAL_ACCEPT,
    SOURCE_CAMERA_MEDIA_ACTUAL_ACCEPT
};

static const uint8_t INLINE_ROUTE_INDEX[
    MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT
] = {
    ROUTE_WIFI_INLINE, ROUTE_RENDER_INLINE, ROUTE_DEVICE_INLINE
};

typedef struct {
    MeisoStatePublisher *publisher;
    MeisoCompositionHostAppDesiredSnapshot snapshot;
    uint8_t inflight_payload[HOST_DESIRED_SCRATCH_BYTES];
    size_t inflight_payload_size;
    uint64_t inflight_command_id;
    MeisoOwnerScope inflight_owner;
    uint64_t inflight_version;
    MeisoDeliveryGeneration inflight_generation;
    bool inflight_present;
    bool inflight_source_completed;
    bool inflight_offer_accepted;
    bool inflight_actual_observed;
    bool offer_queued;
    bool offer_sent;
    bool plan_present;
    bool plan_echoed;
    MeisoCompositionScenePresentationPlan plan;
} HostDesiredChannel;

typedef struct {
    MeisoStateReplica *replica;
    MeisoCompositionHostAppActualSnapshot snapshot;
    bool owner_started;
    bool accept_pending;
    bool accept_work_queued;
    MeisoDeliveryGeneration accept_generation;
    MeisoOwnerScope accept_owner;
    uint64_t accept_version;
} HostActualChannel;

typedef struct {
    MeisoStream *stream;
    MeisoCompositionHostAppInlineSnapshot snapshot;
    bool producer_present;
    MeisoOwnerScope producer;
    uint64_t epoch;
} HostInlineChannel;

typedef struct {
    size_t offset;
    bool consumed;
    int fd;
    uint8_t active_route_mask;
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaPosixTcp *adapter;
    MeisoCompositionMediaTransportConfig transport_config;
    uint64_t published_high_water[
        MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT
    ];
} HostMediaSlot;

typedef struct {
    size_t total;
    size_t peer_offset;
    size_t peer_bytes;
    size_t publisher_offset[MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT];
    size_t publisher_bytes[MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT];
    size_t replica_offset[MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT];
    size_t replica_bytes[MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT];
    size_t stream_offset[MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT];
    size_t stream_bytes[MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT];
    size_t mesh_offset;
    size_t mesh_bytes;
    size_t media_slots_offset;
    size_t media_transport_bytes;
    size_t media_slot_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
} HostAppLayout;

struct MeisoCompositionHostApp {
    uint32_t magic;
    uint8_t *arena_begin;
    size_t arena_bytes;
    MeisoCompositionHostAppConfig config;
    MeisoCompositionDebugPeer *peer;
    MeisoCompositionSessionV3RouteDescriptor catalog_routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    HostDesiredChannel desired[MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT];
    HostActualChannel actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT];
    HostInlineChannel inline_streams[
        MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT
    ];
    HostMediaSlot media_slots[MEISO_COMPOSITION_HOST_APP_MAX_MEDIA_CONNECTIONS];
    HostMediaSlot *active_media;
    uint8_t media_slots_consumed;
    uint8_t fairness_cursor;
    bool deliveries_started;
    bool mutable_active;
    MeisoCompositionHostAppPhase phase;
    MeisoCompositionHostAppTerminalReason terminal_reason;
    uint64_t next_command_id;
    uint64_t plan_generation_high_water;
    uint8_t *mesh_bytes;
    size_t mesh_capacity;
    size_t mesh_size;
    MeisoCompositionHostAppMeshSnapshot mesh;
    bool blob_work_queued[4];
    uint32_t blob_chunk_queued_bytes;
    MeisoRuntimeBlobTransferRef blob_transfer;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_effective_present;
    MeisoWifiTelemetrySample wifi_sample;
    bool wifi_sample_present;
    MeisoCompositionScenePresentationStatus presentation_status;
    bool presentation_status_present;
    MeisoCompositionSceneStatus scene_status;
    bool scene_status_present;
    MeisoCompositionSceneRenderResult render_result;
    bool render_result_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_effective_present;
    MeisoDeviceTelemetrySample device_sample;
    bool device_sample_present;
    MeisoCompositionMediaActual render_media_actual;
    bool render_media_actual_present;
    MeisoCompositionMediaActual camera_media_actual;
    bool camera_media_actual_present;
    uint8_t desired_scratch[HOST_DESIRED_SCRATCH_BYTES];
    uint8_t profile_scratch[HOST_DESIRED_SCRATCH_BYTES];
    size_t control_peer_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t mesh_staging_bytes;
    size_t media_slot_bytes;
    size_t media_transport_bytes;
};

_Static_assert(
    MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT == 6U &&
        MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT == 6U &&
        MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT == 3U,
    "host channel tables"
);
_Static_assert(
    MEISO_COMPOSITION_HOST_APP_DESIRED_WIFI ==
            MEISO_COMPOSITION_HOST_APP_ACTUAL_WIFI &&
        MEISO_COMPOSITION_HOST_APP_DESIRED_PRESENTATION ==
            MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION &&
        MEISO_COMPOSITION_HOST_APP_DESIRED_SCENE ==
            MEISO_COMPOSITION_HOST_APP_ACTUAL_SCENE &&
        MEISO_COMPOSITION_HOST_APP_DESIRED_DEVICE ==
            MEISO_COMPOSITION_HOST_APP_ACTUAL_DEVICE &&
        MEISO_COMPOSITION_HOST_APP_DESIRED_RENDER_MEDIA ==
            MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA &&
        MEISO_COMPOSITION_HOST_APP_DESIRED_CAMERA_MEDIA ==
            MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA,
    "desired and actual barrier channel ordering"
);
_Static_assert(
    HOST_MEDIA_ADAPTER_RESERVE_BYTES >=
        MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES +
        MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES,
    "media adapter reserve covers bounded metadata and one RX packet"
);

static bool checked_add(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

static bool align_up(size_t value, size_t alignment, size_t *out_value)
{
    size_t adjusted;
    if (out_value == NULL || alignment == 0U ||
        value > SIZE_MAX - (alignment - 1U)) {
        return false;
    }
    adjusted = value + alignment - 1U;
    *out_value = adjusted - adjusted % alignment;
    return true;
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_start > UINTPTR_MAX - lhs_size ||
        rhs_start > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_start < rhs_start + rhs_size &&
        rhs_start < lhs_start + lhs_size;
}

static void peer_source(
    MeisoCompositionDebugPeerConfig *config,
    uint8_t index,
    MeisoRuntimeSchedulerWorkKind kind,
    uint8_t route_index
)
{
    config->sources[index].work_kind = kind;
    config->sources[index].route_index = route_index;
}

static void configure_peer_sources(MeisoCompositionDebugPeerConfig *config)
{
    size_t index;
    peer_source(config, SOURCE_CATALOG,
                MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT,
                MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE);
    peer_source(config, SOURCE_WIFI_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_WIFI_DESIRED);
    peer_source(config, SOURCE_WIFI_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_WIFI_ACTUAL);
    peer_source(config, SOURCE_BLOB_BEGIN,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN, ROUTE_MESH_BLOB);
    peer_source(config, SOURCE_BLOB_CHUNK,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK, ROUTE_MESH_BLOB);
    peer_source(config, SOURCE_BLOB_COMMIT,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT, ROUTE_MESH_BLOB);
    peer_source(config, SOURCE_BLOB_ABORT,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT, ROUTE_MESH_BLOB);
    peer_source(config, SOURCE_PRESENTATION_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_PRESENTATION_DESIRED);
    peer_source(config, SOURCE_PRESENTATION_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_PRESENTATION_ACTUAL);
    peer_source(config, SOURCE_SCENE_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_SCENE_DESIRED);
    peer_source(config, SOURCE_SCENE_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_SCENE_ACTUAL);
    peer_source(config, SOURCE_DEVICE_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_DEVICE_DESIRED);
    peer_source(config, SOURCE_DEVICE_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_DEVICE_ACTUAL);
    peer_source(config, SOURCE_RENDER_MEDIA_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_RENDER_MEDIA_DESIRED);
    peer_source(config, SOURCE_RENDER_MEDIA_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_RENDER_MEDIA_ACTUAL);
    peer_source(config, SOURCE_CAMERA_MEDIA_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_CAMERA_MEDIA_DESIRED);
    peer_source(config, SOURCE_CAMERA_MEDIA_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_CAMERA_MEDIA_ACTUAL);
    for (index = 17U; index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT;
         ++index) {
        peer_source(config, (uint8_t)index,
                    MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                    ROUTE_WIFI_DESIRED);
    }
}

static bool peer_config_from_host(
    const MeisoCompositionHostAppConfig *config,
    MeisoCompositionDebugPeerConfig *out_config
)
{
    MeisoCompositionDebugPeerConfig candidate;
    if (config == NULL || out_config == NULL) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.connected_fd = config->control_connected_fd;
    candidate.local_role = MEISO_SESSION_ROLE_HOST;
    candidate.local_peer_id = config->local_peer_id;
    candidate.local_incarnation = config->local_incarnation;
    candidate.local_nonce = config->local_nonce;
    candidate.expected_peer_id = config->expected_edge_peer_id;
    candidate.protocol = config->protocol;
    candidate.catalog_generation = config->catalog_generation;
    candidate.catalog = config->catalog;
    candidate.max_io_bytes_per_step = config->max_control_io_bytes_per_step;
    candidate.adapter_reserve_bytes = config->control_adapter_reserve_bytes;
    candidate.runtime_reserve_bytes = config->control_runtime_reserve_bytes;
    candidate.scheduler_reserve_bytes = config->control_scheduler_reserve_bytes;
    configure_peer_sources(&candidate);
    *out_config = candidate;
    return true;
}

static bool build_catalog_for_config(
    const MeisoCompositionHostAppConfig *config,
    MeisoCompositionSessionV3RouteDescriptor *out_routes
)
{
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    uint8_t capability[MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES];
    return config != NULL && out_routes != NULL &&
        meiso_composition_session_v3_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){capability, sizeof(capability)},
            endpoints, MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            out_routes, MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) == MEISO_COMPOSITION_SESSION_V3_OK;
}

static bool config_shape_valid(const MeisoCompositionHostAppConfig *config)
{
    MeisoCompositionDebugPeerConfig peer_config;
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    return config != NULL && config->control_connected_fd >= 0 &&
        config->local_incarnation != 0U &&
        meiso_session_nonce_valid(config->local_nonce) &&
        config->catalog_generation != 0U &&
        config->max_control_io_bytes_per_step != 0U &&
        config->control_adapter_reserve_bytes >=
            MEISO_COMPOSITION_HOST_APP_CONTROL_ADAPTER_MIN_RESERVE_BYTES &&
        config->control_runtime_reserve_bytes != 0U &&
        config->control_scheduler_reserve_bytes != 0U &&
        meiso_delivery_generation_valid(
            config->initial_desired_delivery_generation
        ) && config->media_connection_slots != 0U &&
        config->media_connection_slots <=
            MEISO_COMPOSITION_HOST_APP_MAX_MEDIA_CONNECTIONS &&
        config->max_media_io_bytes_per_step != 0U &&
        peer_config_from_host(config, &peer_config) &&
        meiso_composition_debug_peer_memory_required(&peer_config) != 0U &&
        build_catalog_for_config(config, routes);
}

static bool add_aligned_region(
    size_t *cursor,
    size_t bytes,
    size_t alignment,
    size_t *out_offset
)
{
    size_t offset;
    size_t end;
    if (cursor == NULL || out_offset == NULL || bytes == 0U ||
        !align_up(*cursor, alignment, &offset) ||
        !checked_add(offset, bytes, &end)) {
        return false;
    }
    *out_offset = offset;
    *cursor = end;
    return true;
}

static bool calculate_layout(
    const MeisoCompositionHostAppConfig *config,
    HostAppLayout *out_layout,
    MeisoCompositionSessionV3RouteDescriptor *out_routes
)
{
    const size_t alignment = alignof(max_align_t);
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_COMPOSITION_HOST_APP_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    MeisoCompositionDebugPeerConfig peer_config;
    HostAppLayout layout;
    size_t cursor;
    size_t index;
    size_t slots_bytes;
    uint32_t max_fragment;

    if (out_layout == NULL || out_routes == NULL ||
        !config_shape_valid(config) ||
        !peer_config_from_host(config, &peer_config) ||
        !build_catalog_for_config(config, out_routes)) {
        return false;
    }
    memset(&layout, 0, sizeof(layout));
    if (!align_up(sizeof(MeisoCompositionHostApp), alignment, &cursor)) {
        return false;
    }
    layout.peer_bytes =
        meiso_composition_debug_peer_memory_required(&peer_config);
    if (!add_aligned_region(
            &cursor, layout.peer_bytes, alignment, &layout.peer_offset
        )) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT;
         ++index) {
        layout.publisher_bytes[index] =
            meiso_state_publisher_memory_required(
                &out_routes[DESIRED_ROUTE_INDEX[index]].route.resource,
                &state_config
            );
        if (!add_aligned_region(
                &cursor, layout.publisher_bytes[index], alignment,
                &layout.publisher_offset[index]
            ) || !checked_add(
                layout.core_state_bytes, layout.publisher_bytes[index],
                &layout.core_state_bytes
            )) {
            return false;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT;
         ++index) {
        layout.replica_bytes[index] = meiso_state_replica_memory_required(
            &out_routes[ACTUAL_ROUTE_INDEX[index]].route.resource
        );
        if (!add_aligned_region(
                &cursor, layout.replica_bytes[index], alignment,
                &layout.replica_offset[index]
            ) || !checked_add(
                layout.core_state_bytes, layout.replica_bytes[index],
                &layout.core_state_bytes
            )) {
            return false;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT;
         ++index) {
        layout.stream_bytes[index] = meiso_stream_memory_required(
            &out_routes[INLINE_ROUTE_INDEX[index]].route.resource,
            &stream_config
        );
        if (!add_aligned_region(
                &cursor, layout.stream_bytes[index], alignment,
                &layout.stream_offset[index]
            ) || !checked_add(
                layout.core_stream_bytes, layout.stream_bytes[index],
                &layout.core_stream_bytes
            )) {
            return false;
        }
    }
    layout.mesh_bytes = out_routes[ROUTE_MESH_BLOB].route.resource
        .max_payload_size;
    if (!add_aligned_region(
            &cursor, layout.mesh_bytes, alignment, &layout.mesh_offset
        )) {
        return false;
    }
    max_fragment = config->catalog.render_media_capability
            .max_fragment_payload_bytes >
            config->catalog.camera_media_capability
                .max_fragment_payload_bytes ?
        config->catalog.render_media_capability.max_fragment_payload_bytes :
        config->catalog.camera_media_capability.max_fragment_payload_bytes;
    layout.media_transport_bytes =
        MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES +
        2U * ((size_t)config->catalog.render_media_capability
                    .max_access_unit_bytes +
              (size_t)config->catalog.camera_media_capability
                    .max_access_unit_bytes) +
        MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + max_fragment;
    if (!align_up(
            layout.media_transport_bytes, alignment,
            &layout.media_transport_bytes
        ) || !checked_add(
            layout.media_transport_bytes,
            HOST_MEDIA_ADAPTER_RESERVE_BYTES,
            &layout.media_slot_bytes
        ) || !checked_multiply(
            layout.media_slot_bytes, config->media_connection_slots,
            &slots_bytes
        ) || !add_aligned_region(
            &cursor, slots_bytes, alignment, &layout.media_slots_offset
        ) || !align_up(cursor, alignment, &layout.total)) {
        return false;
    }
    *out_layout = layout;
    return true;
}

size_t meiso_composition_host_app_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_composition_host_app_memory_required(
    const MeisoCompositionHostAppConfig *config
)
{
    HostAppLayout layout;
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    return calculate_layout(config, &layout, routes) ? layout.total : 0U;
}

static bool fd_preflight(int fd)
{
    int flags;
    int socket_type;
    struct sockaddr_storage peer_address;
    socklen_t peer_address_size = (socklen_t)sizeof(peer_address);
    socklen_t socket_type_size = (socklen_t)sizeof(socket_type);
    flags = fcntl(fd, F_GETFL);
    return flags >= 0 && (flags & O_NONBLOCK) != 0 &&
        getsockopt(
            fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_size
        ) == 0 && socket_type == SOCK_STREAM &&
        getpeername(
            fd, (struct sockaddr *)&peer_address, &peer_address_size
        ) == 0 &&
        (peer_address.ss_family == AF_INET ||
         peer_address.ss_family == AF_INET6);
}

static bool fd_prepare_send_safety(int fd)
{
#if defined(MSG_NOSIGNAL)
    (void)fd;
    return true;
#elif defined(SO_NOSIGPIPE)
    const int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                      (socklen_t)sizeof(enabled)) == 0;
#else
    (void)fd;
    return false;
#endif
}

MeisoCompositionHostAppInitStatus meiso_composition_host_app_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionHostAppConfig *config,
    MeisoCompositionHostApp **out_app
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_COMPOSITION_HOST_APP_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    MeisoCompositionDebugPeerConfig peer_config;
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    HostAppLayout layout;
    MeisoCompositionHostApp *app;
    MeisoCompositionDebugPeer *initialized_peer;
    size_t index;
    if (arena == NULL || config == NULL || out_app == NULL ||
        ranges_overlap(arena, arena_size, config, sizeof(*config)) ||
        ranges_overlap(arena, arena_size, out_app, sizeof(*out_app)) ||
        ranges_overlap(config, sizeof(*config), out_app, sizeof(*out_app))) {
        return MEISO_COMPOSITION_HOST_APP_INIT_INVALID_ARGUMENT;
    }
    if (!calculate_layout(config, &layout, routes) ||
        !peer_config_from_host(config, &peer_config)) {
        return MEISO_COMPOSITION_HOST_APP_INIT_INVALID_CONFIG;
    }
    if (((uintptr_t)arena % alignof(max_align_t)) != 0U) {
        return MEISO_COMPOSITION_HOST_APP_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < layout.total) {
        return MEISO_COMPOSITION_HOST_APP_INIT_ARENA_TOO_SMALL;
    }
    if (!fd_preflight(config->control_connected_fd) ||
        !fd_prepare_send_safety(config->control_connected_fd)) {
        return MEISO_COMPOSITION_HOST_APP_INIT_FD_ERROR;
    }

    if (meiso_composition_debug_peer_init(
            (uint8_t *)arena + layout.peer_offset, layout.peer_bytes,
            &peer_config, &initialized_peer
        ) != MEISO_COMPOSITION_DEBUG_PEER_INIT_OK) {
        return MEISO_COMPOSITION_HOST_APP_INIT_INTERNAL_FAILURE;
    }
    /* Complete layout/config/fd preflight makes child failure unreachable for
     * the same config and a caller-stable fd in this single-threaded call.
     * The defensive status remains because a child failure may have written
     * its subarena; in that case out_app stays unchanged and the arena is
     * intentionally invalidated, as the public init contract states.
     * Remaining child layouts/descriptors are the exact values precomputed
     * above and therefore have no reachable failure result. */
    memset(arena, 0, sizeof(MeisoCompositionHostApp));
    app = (MeisoCompositionHostApp *)arena;
    app->arena_begin = (uint8_t *)arena;
    app->arena_bytes = layout.total;
    app->config = *config;
    memcpy(app->catalog_routes, routes, sizeof(routes));
    app->peer = initialized_peer;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT;
         ++index) {
        (void)meiso_state_publisher_init(
            app->arena_begin + layout.publisher_offset[index],
            layout.publisher_bytes[index],
            &routes[DESIRED_ROUTE_INDEX[index]].route.resource,
            &state_config, &app->desired[index].publisher
        );
        app->desired[index].snapshot.delivery_generation =
            config->initial_desired_delivery_generation;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT;
         ++index) {
        (void)meiso_state_replica_init(
            app->arena_begin + layout.replica_offset[index],
            layout.replica_bytes[index],
            &routes[ACTUAL_ROUTE_INDEX[index]].route.resource,
            &app->actual[index].replica
        );
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT;
         ++index) {
        (void)meiso_stream_init(
            app->arena_begin + layout.stream_offset[index],
            layout.stream_bytes[index],
            &routes[INLINE_ROUTE_INDEX[index]].route.resource,
            &stream_config, &app->inline_streams[index].stream
        );
    }
    app->mesh_bytes = app->arena_begin + layout.mesh_offset;
    app->mesh_capacity = layout.mesh_bytes;
    memset(app->mesh_bytes, 0, app->mesh_capacity);
    memset(app->arena_begin + layout.media_slots_offset, 0,
           layout.media_slot_bytes * config->media_connection_slots);
    for (index = 0U; index < config->media_connection_slots; ++index) {
        app->media_slots[index].offset =
            layout.media_slots_offset + index * layout.media_slot_bytes;
        app->media_slots[index].fd = -1;
    }
    app->phase = MEISO_COMPOSITION_HOST_APP_PHASE_HANDSHAKE;
    app->next_command_id = 1U;
    app->control_peer_bytes = layout.peer_bytes;
    app->core_state_bytes = layout.core_state_bytes;
    app->core_stream_bytes = layout.core_stream_bytes;
    app->mesh_staging_bytes = layout.mesh_bytes;
    app->media_slot_bytes = layout.media_slot_bytes;
    app->media_transport_bytes = layout.media_transport_bytes;
    app->magic = HOST_APP_MAGIC;
    *out_app = app;
    return MEISO_COMPOSITION_HOST_APP_INIT_OK;
}

static bool app_valid(const MeisoCompositionHostApp *app)
{
    return app != NULL && app->magic == HOST_APP_MAGIC &&
        app->arena_begin == (const uint8_t *)app &&
        app->arena_bytes >= sizeof(*app) && app->peer != NULL;
}

static bool app_range_overlap(
    const MeisoCompositionHostApp *app,
    const void *value,
    size_t size
)
{
    return app_valid(app) && ranges_overlap(
        app->arena_begin, app->arena_bytes, value, size
    );
}

static MeisoCompositionHostAppStatus mutation_status(
    MeisoCompositionHostApp *app
)
{
    if (!app_valid(app)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (app->mutable_active) {
        return MEISO_COMPOSITION_HOST_APP_BUSY;
    }
    if (app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_APP_TERMINAL;
    }
    return MEISO_COMPOSITION_HOST_APP_OK;
}

static bool receipt_valid(
    const MeisoCompositionHostApp *app,
    const void *input,
    size_t input_size,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    return out_receipt != NULL &&
        !app_range_overlap(app, out_receipt, sizeof(*out_receipt)) &&
        (input == NULL || !app_range_overlap(app, input, input_size)) &&
        (input == NULL || !ranges_overlap(
            input, input_size, out_receipt, sizeof(*out_receipt)
        ));
}

static MeisoOwnerScope host_owner(const MeisoCompositionHostApp *app)
{
    MeisoOwnerScope owner;
    owner.role = MEISO_OWNER_ROLE_HOST;
    owner.incarnation = app->config.local_incarnation;
    return owner;
}

static MeisoCompositionHostAppStatus publish_desired_profile(
    MeisoCompositionHostApp *app,
    uint8_t channel,
    MeisoCompositionHostAppCommandKind kind,
    bool enabled,
    MeisoByteView profile,
    const MeisoCompositionScenePresentationPlan *plan,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    MeisoRuntimeDesiredState desired;
    MeisoByteView payload;
    MeisoCompositionHostAppCommandReceipt receipt;
    HostDesiredChannel *target;
    MeisoStatePublishStatus publish_status;
    uint64_t version;
    uint64_t command_id;
    bool replaced_local;

    if (mutation_status(app) != MEISO_COMPOSITION_HOST_APP_OK ||
        out_receipt == NULL || channel >= MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT ||
        app_range_overlap(app, out_receipt, sizeof(*out_receipt)) ||
        (profile.size != 0U && profile.data == NULL)) {
        return mutation_status(app) == MEISO_COMPOSITION_HOST_APP_TERMINAL ?
            MEISO_COMPOSITION_HOST_APP_TERMINAL :
            MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    target = &app->desired[channel];
    if (app->next_command_id == UINT64_MAX ||
        target->snapshot.state_version == UINT64_MAX) {
        return MEISO_COMPOSITION_HOST_APP_COUNTER_EXHAUSTED;
    }
    command_id = app->next_command_id;
    version = target->snapshot.state_version + 1U;
    replaced_local = target->snapshot.requested &&
        target->snapshot.delivery_pending &&
        (!target->inflight_present ||
         target->snapshot.command_id != target->inflight_command_id);
    desired.enabled = enabled;
    desired.profile = profile;
    if (!meiso_runtime_desired_state_encode(
            &desired,
            (MeisoMutableBuffer){app->desired_scratch,
                                 sizeof(app->desired_scratch)},
            &payload
        )) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    app->mutable_active = true;
    publish_status = meiso_state_publisher_publish(
        target->publisher, host_owner(app), version, payload
    );
    app->mutable_active = false;
    if (publish_status != MEISO_STATE_PUBLISH_NEW &&
        publish_status != MEISO_STATE_PUBLISH_REPLACED_PENDING) {
        return publish_status == MEISO_STATE_PUBLISH_CAPACITY ?
            MEISO_COMPOSITION_HOST_APP_CAPACITY :
            MEISO_COMPOSITION_HOST_APP_INTERNAL_FAILURE;
    }
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = command_id;
    receipt.kind = kind;
    receipt.local_result = !replaced_local ?
        MEISO_COMPOSITION_HOST_APP_LOCAL_QUEUED :
        MEISO_COMPOSITION_HOST_APP_LOCAL_REPLACED_PENDING;
    if (replaced_local) {
        receipt.replaced_command_present = true;
        receipt.replaced_command_id = target->snapshot.command_id;
        ++target->snapshot.replaced_pending_count;
    }
    receipt.desired_state_present = true;
    receipt.desired_channel = channel;
    receipt.desired_state_version = version;
    if (plan != NULL) {
        receipt.plan_generation_present = true;
        receipt.plan_generation = plan->plan_generation;
        target->plan_present = true;
        target->plan = *plan;
        target->plan_echoed = false;
        app->plan_generation_high_water = plan->plan_generation;
    } else {
        target->plan_present = false;
        target->plan_echoed = false;
        memset(&target->plan, 0, sizeof(target->plan));
    }
    target->snapshot.requested = true;
    target->snapshot.enabled = enabled;
    target->snapshot.command_id = command_id;
    target->snapshot.owner = host_owner(app);
    target->snapshot.state_version = version;
    target->snapshot.delivery_pending = true;
    target->snapshot.delivery_accepted = false;
    target->snapshot.delivery_failed = false;
    app->next_command_id = command_id + 1U;
    *out_receipt = receipt;
    return MEISO_COMPOSITION_HOST_APP_OK;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_put_mesh(
    MeisoCompositionHostApp *app,
    MeisoByteView canonical_mesh,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoContentId content_id;
    uint64_t transfer_id_high_water;
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        return status;
    }
    if (!receipt_valid(app, canonical_mesh.data, canonical_mesh.size,
                       out_receipt) || canonical_mesh.data == NULL ||
        canonical_mesh.size == 0U || canonical_mesh.size > app->mesh_capacity) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (app->mesh.queued || app->mesh.transfer_active ||
        app->mesh.waiting_result) {
        return MEISO_COMPOSITION_HOST_APP_BUSY;
    }
    if (app->next_command_id == UINT64_MAX) {
        return MEISO_COMPOSITION_HOST_APP_COUNTER_EXHAUSTED;
    }
    if (!meiso_composition_scene_mesh_content_id(
            canonical_mesh, &content_id
        )) {
        return MEISO_COMPOSITION_HOST_APP_CONFLICT;
    }
    transfer_id_high_water = app->mesh.transfer_id_high_water;
    memcpy(app->mesh_bytes, canonical_mesh.data, canonical_mesh.size);
    app->mesh_size = canonical_mesh.size;
    memset(&app->mesh, 0, sizeof(app->mesh));
    app->mesh.transfer_id_high_water = transfer_id_high_water;
    app->mesh.queued = true;
    app->mesh.command_id = app->next_command_id;
    app->mesh.content_id = content_id;
    app->mesh.total_bytes = canonical_mesh.size;
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = app->next_command_id++;
    receipt.kind = MEISO_COMPOSITION_HOST_APP_COMMAND_PUT_MESH;
    receipt.local_result = MEISO_COMPOSITION_HOST_APP_LOCAL_QUEUED;
    receipt.mesh_content_id_present = true;
    receipt.mesh_content_id = content_id;
    *out_receipt = receipt;
    return MEISO_COMPOSITION_HOST_APP_OK;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_set_scene(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    size_t profile_size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        return status;
    }
    if (!receipt_valid(app, value, value == NULL ? 0U : sizeof(*value),
                       out_receipt) || enabled != (value != NULL)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (enabled) {
        if (meiso_composition_scene_desired_encode(
                value,
                (MeisoMutableBuffer){app->profile_scratch,
                                     sizeof(app->profile_scratch)},
                &profile_size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return MEISO_COMPOSITION_HOST_APP_CONFLICT;
        }
        profile.data = app->profile_scratch;
        profile.size = profile_size;
    }
    return publish_desired_profile(
        app, MEISO_COMPOSITION_HOST_APP_DESIRED_SCENE,
        MEISO_COMPOSITION_HOST_APP_COMMAND_SET_SCENE, enabled, profile, NULL,
        out_receipt
    );
}

MeisoCompositionHostAppStatus meiso_composition_host_app_set_presentation(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionHostAppPresentationSettings *settings,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];
    size_t profile_size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        return status;
    }
    if (!receipt_valid(app, settings,
                       settings == NULL ? 0U : sizeof(*settings), out_receipt) ||
        enabled != (settings != NULL)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    memset(&plan, 0, sizeof(plan));
    if (enabled) {
        if (app->plan_generation_high_water == UINT64_MAX) {
            return MEISO_COMPOSITION_HOST_APP_COUNTER_EXHAUSTED;
        }
        plan.flags = settings->flags;
        plan.plan_generation = app->plan_generation_high_water + 1U;
        plan.capability_generation = app->config.catalog_generation;
        plan.scene_binding = app->catalog_routes[ROUTE_SCENE_DESIRED]
            .route.endpoint_id;
        plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
        plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
        plan.target_width = settings->target_width;
        plan.target_height = settings->target_height;
        plan.color_format = settings->color_format;
        plan.depth_format = settings->depth_format;
        plan.max_result_hz = settings->max_result_hz;
        plan.offline_lease_ms = settings->offline_lease_ms;
        if (!meiso_composition_coordinator_plan_digest(&plan, digest)) {
            return MEISO_COMPOSITION_HOST_APP_CONFLICT;
        }
        memcpy(plan.candidate_digest, digest, sizeof(digest));
        if (meiso_composition_scene_presentation_plan_encode(
                &plan,
                (MeisoMutableBuffer){app->profile_scratch,
                                     sizeof(app->profile_scratch)},
                &profile_size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return MEISO_COMPOSITION_HOST_APP_CONFLICT;
        }
        profile.data = app->profile_scratch;
        profile.size = profile_size;
    }
    return publish_desired_profile(
        app, MEISO_COMPOSITION_HOST_APP_DESIRED_PRESENTATION,
        MEISO_COMPOSITION_HOST_APP_COMMAND_SET_PRESENTATION, enabled, profile,
        enabled ? &plan : NULL, out_receipt
    );
}

MeisoCompositionHostAppStatus meiso_composition_host_app_set_wifi(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    size_t size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) return status;
    if (!receipt_valid(app, parameters,
                       parameters == NULL ? 0U : sizeof(*parameters),
                       out_receipt) || enabled != (parameters != NULL)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (enabled && (!meiso_wifi_telemetry_parameters_supported(
            &app->config.catalog.wifi_capability, parameters) ||
        meiso_wifi_telemetry_desired_parameters_encode(
            parameters, (MeisoMutableBuffer){app->profile_scratch,
                                             sizeof(app->profile_scratch)},
            &size) != MEISO_WIFI_TELEMETRY_CODEC_OK)) {
        return MEISO_COMPOSITION_HOST_APP_CONFLICT;
    }
    if (enabled) profile = (MeisoByteView){app->profile_scratch, size};
    return publish_desired_profile(
        app, MEISO_COMPOSITION_HOST_APP_DESIRED_WIFI,
        MEISO_COMPOSITION_HOST_APP_COMMAND_SET_WIFI, enabled, profile, NULL,
        out_receipt
    );
}

MeisoCompositionHostAppStatus meiso_composition_host_app_set_device(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoDeviceTelemetryParameters *parameters,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    size_t size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) return status;
    if (!receipt_valid(app, parameters,
                       parameters == NULL ? 0U : sizeof(*parameters),
                       out_receipt) || enabled != (parameters != NULL)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (enabled && (!meiso_device_telemetry_parameters_supported(
            &app->config.catalog.device_telemetry_capability, parameters) ||
        meiso_device_telemetry_parameters_encode(
            parameters, (MeisoMutableBuffer){app->profile_scratch,
                                             sizeof(app->profile_scratch)},
            &size) != MEISO_DEVICE_TELEMETRY_CODEC_OK)) {
        return MEISO_COMPOSITION_HOST_APP_CONFLICT;
    }
    if (enabled) profile = (MeisoByteView){app->profile_scratch, size};
    return publish_desired_profile(
        app, MEISO_COMPOSITION_HOST_APP_DESIRED_DEVICE,
        MEISO_COMPOSITION_HOST_APP_COMMAND_SET_DEVICE, enabled, profile, NULL,
        out_receipt
    );
}

static MeisoCompositionHostAppStatus set_media_desired(
    MeisoCompositionHostApp *app,
    uint8_t channel,
    MeisoCompositionHostAppCommandKind kind,
    const MeisoCompositionMediaCapability *capability,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    size_t size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) return status;
    if (!receipt_valid(app, desired, desired == NULL ? 0U : sizeof(*desired),
                       out_receipt) || enabled != (desired != NULL)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (enabled && (!meiso_composition_media_desired_supported(
            capability, desired) || meiso_composition_media_desired_encode(
            desired, (MeisoMutableBuffer){app->profile_scratch,
                                         sizeof(app->profile_scratch)},
            &size) != MEISO_COMPOSITION_SESSION_V3_OK)) {
        return MEISO_COMPOSITION_HOST_APP_CONFLICT;
    }
    if (enabled) profile = (MeisoByteView){app->profile_scratch, size};
    return publish_desired_profile(
        app, channel, kind, enabled, profile, NULL, out_receipt
    );
}

MeisoCompositionHostAppStatus meiso_composition_host_app_set_render_media(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    return set_media_desired(
        app, MEISO_COMPOSITION_HOST_APP_DESIRED_RENDER_MEDIA,
        MEISO_COMPOSITION_HOST_APP_COMMAND_SET_RENDER_MEDIA,
        app_valid(app) ? &app->config.catalog.render_media_capability : NULL,
        enabled, desired, out_receipt
    );
}

MeisoCompositionHostAppStatus meiso_composition_host_app_set_camera_media(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostAppCommandReceipt *out_receipt
)
{
    return set_media_desired(
        app, MEISO_COMPOSITION_HOST_APP_DESIRED_CAMERA_MEDIA,
        MEISO_COMPOSITION_HOST_APP_COMMAND_SET_CAMERA_MEDIA,
        app_valid(app) ? &app->config.catalog.camera_media_capability : NULL,
        enabled, desired, out_receipt
    );
}

static bool media_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
)
{
    return lhs->pixel_format == rhs->pixel_format &&
        lhs->codec == rhs->codec && lhs->width == rhs->width &&
        lhs->height == rhs->height &&
        lhs->fps_millihz == rhs->fps_millihz &&
        lhs->bitrate_bps == rhs->bitrate_bps &&
        lhs->gop_frames == rhs->gop_frames &&
        lhs->recovery_flags == rhs->recovery_flags;
}

static bool media_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        lhs->endpoint_id.high == rhs->endpoint_id.high &&
        lhs->endpoint_id.low == rhs->endpoint_id.low &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        meiso_resource_id_equal(lhs->resource_id, rhs->resource_id) &&
        lhs->source == rhs->source &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

static bool media_desired_channels_converged(
    const MeisoCompositionHostApp *app
)
{
    static const size_t MEDIA_CHANNELS[
        MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT
    ] = {
        MEISO_COMPOSITION_HOST_APP_DESIRED_RENDER_MEDIA,
        MEISO_COMPOSITION_HOST_APP_DESIRED_CAMERA_MEDIA
    };
    size_t media_index;
    for (media_index = 0U;
         media_index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
         ++media_index) {
        const size_t index = MEDIA_CHANNELS[media_index];
        const MeisoCompositionHostAppDesiredSnapshot *desired =
            &app->desired[index].snapshot;
        const MeisoCompositionHostAppActualSnapshot *actual =
            &app->actual[index].snapshot;
        if (!desired->requested) {
            if (actual->present) {
                return false;
            }
            continue;
        }
        if (desired->inflight_present || desired->delivery_pending ||
            !desired->delivery_accepted || desired->delivery_failed ||
            !actual->present || !actual->observed_desired.present ||
            !actual->applied_desired.present ||
            actual->observed_desired.owner.role != desired->owner.role ||
            actual->observed_desired.owner.incarnation !=
                desired->owner.incarnation ||
            actual->observed_desired.version != desired->state_version ||
            actual->applied_desired.owner.role != desired->owner.role ||
            actual->applied_desired.owner.incarnation !=
                desired->owner.incarnation ||
            actual->applied_desired.version != desired->state_version) {
            return false;
        }
    }
    return true;
}

static bool media_route_config(
    const MeisoCompositionHostApp *app,
    const MeisoCompositionDebugPeerSnapshot *peer,
    size_t media_index,
    MeisoCompositionMediaRouteConfig *out_route
)
{
    const size_t actual_index = media_index == 0U ?
        MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA :
        MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA;
    const size_t route_index = media_index == 0U ?
        ROUTE_RENDER_MEDIA_DATA : ROUTE_CAMERA_MEDIA_DATA;
    const MeisoCompositionMediaActual *media = media_index == 0U ?
        &app->render_media_actual : &app->camera_media_actual;
    const MeisoCompositionMediaCapability *capability = media_index == 0U ?
        &app->config.catalog.render_media_capability :
        &app->config.catalog.camera_media_capability;
    const MeisoCompositionHostAppActualSnapshot *actual =
        &app->actual[actual_index].snapshot;
    const MeisoRuntimeRouteDescriptor *descriptor =
        &app->catalog_routes[route_index].route;
    MeisoCompositionMediaRouteConfig route;
    if (out_route == NULL || !actual->present || !actual->profile_decoded ||
        !actual->effective_enabled ||
        actual->availability != MEISO_RUNTIME_AVAILABILITY_PRESENT ||
        actual->production_status != MEISO_RUNTIME_PRODUCTION_RUNNING ||
        actual->production_epoch == 0U || media->media_stream_epoch == 0U ||
        !meiso_session_nonce_valid(media->channel_nonce) ||
        !meiso_composition_media_desired_supported(
            capability, &media->effective
        )) {
        return false;
    }
    memset(&route, 0, sizeof(route));
    route.binding.session_id = peer->session.session_id;
    route.binding.catalog_generation = peer->runtime.active_catalog_generation;
    route.binding.endpoint_id = descriptor->endpoint_id;
    route.binding.endpoint_incarnation = descriptor->endpoint_incarnation;
    route.binding.resource_id = descriptor->resource.id;
    route.binding.source = media->source;
    route.binding.production_epoch = actual->production_epoch;
    route.binding.media_stream_epoch = media->media_stream_epoch;
    route.binding.channel_nonce = media->channel_nonce;
    route.capability = *capability;
    route.effective = media->effective;
    *out_route = route;
    return true;
}

static bool build_media_config(
    const MeisoCompositionHostApp *app,
    MeisoCompositionMediaTransportConfig *out_config
)
{
    MeisoCompositionDebugPeerSnapshot peer;
    MeisoCompositionMediaTransportConfig config;
    size_t index;
    if (out_config == NULL ||
        !meiso_composition_debug_peer_snapshot(app->peer, &peer) ||
        peer.phase != MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY ||
        peer.runtime.active_catalog_generation != app->config.catalog_generation ||
        peer.session.state != MEISO_SESSION_STATE_READY ||
        !peer.session.session_id_known) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_HOST;
    config.scope.session_id = peer.session.session_id;
    config.scope.catalog_generation = app->config.catalog_generation;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if (media_route_config(app, &peer, index, &config.routes[index])) {
            config.active_route_mask =
                (uint8_t)(config.active_route_mask | bit);
        } else {
            config.routes[index].capability = index == 0U ?
                app->config.catalog.render_media_capability :
                app->config.catalog.camera_media_capability;
        }
    }
    if (config.active_route_mask == 0U) {
        return false;
    }
    *out_config = config;
    return true;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_bind_media_fd(
    MeisoCompositionHostApp *app,
    int connected_fd,
    MeisoCompositionHostAppMediaRouteMask active_routes
)
{
    MeisoCompositionHostAppStatus status = mutation_status(app);
    MeisoCompositionMediaTransportConfig transport_config;
    MeisoCompositionMediaPosixTcpConfig adapter_config;
    HostMediaSlot *slot;
    HostMediaSlot slot_before;
    size_t transport_bytes;
    size_t transport_alignment;
    size_t adapter_bytes;
    uint8_t *slot_bytes;
    MeisoCompositionMediaTransportStatus transport_status;
    MeisoCompositionMediaPosixTcpInitStatus adapter_status;
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        return status;
    }
    if (active_routes == 0U ||
        (active_routes & ~MEISO_COMPOSITION_HOST_APP_MEDIA_ALL) != 0U) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (connected_fd == app->config.control_connected_fd) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (!media_desired_channels_converged(app)) {
        return MEISO_COMPOSITION_HOST_APP_NOT_READY;
    }
    if (app->active_media != NULL) {
        return MEISO_COMPOSITION_HOST_APP_BUSY;
    }
    if (app->media_slots_consumed >= app->config.media_connection_slots) {
        return MEISO_COMPOSITION_HOST_APP_CAPACITY;
    }
    if (!build_media_config(app, &transport_config)) {
        return MEISO_COMPOSITION_HOST_APP_NOT_READY;
    }
    if (active_routes != transport_config.active_route_mask) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_STATE;
    }
    if (!fd_preflight(connected_fd) || !fd_prepare_send_safety(connected_fd)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (meiso_composition_media_transport_memory_required(
            &transport_config, &transport_bytes, &transport_alignment
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        transport_bytes > app->media_transport_bytes ||
        transport_alignment > alignof(max_align_t)) {
        return MEISO_COMPOSITION_HOST_APP_CAPACITY;
    }
    slot = &app->media_slots[app->media_slots_consumed];
    slot_before = *slot;
    slot_bytes = app->arena_begin + slot->offset;
    transport_status = meiso_composition_media_transport_init(
        &transport_config,
        (MeisoMutableBuffer){slot_bytes, app->media_transport_bytes},
        &slot->transport
    );
    if (transport_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        memset(slot_bytes, 0, app->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_HOST_APP_INTERNAL_FAILURE;
    }
    memset(&adapter_config, 0, sizeof(adapter_config));
    adapter_config.connected_fd = connected_fd;
    adapter_config.transport = slot->transport;
    adapter_config.scope = transport_config.scope;
    adapter_config.max_io_bytes_per_step =
        app->config.max_media_io_bytes_per_step;
    adapter_bytes = meiso_composition_media_posix_tcp_memory_required(
        &adapter_config
    );
    if (adapter_bytes == 0U ||
        adapter_bytes > HOST_MEDIA_ADAPTER_RESERVE_BYTES) {
        (void)meiso_composition_media_transport_terminate(
            slot->transport, &transport_config.scope,
            MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST
        );
        memset(slot_bytes, 0, app->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_HOST_APP_CAPACITY;
    }
    adapter_status = meiso_composition_media_posix_tcp_init(
        slot_bytes + app->media_transport_bytes,
        HOST_MEDIA_ADAPTER_RESERVE_BYTES, &adapter_config,
        &slot->adapter
    );
    if (adapter_status != MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK) {
        (void)meiso_composition_media_transport_terminate(
            slot->transport, &transport_config.scope,
            MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST
        );
        memset(slot_bytes, 0, app->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_HOST_APP_INTERNAL_FAILURE;
    }
    slot->consumed = true;
    slot->fd = connected_fd;
    slot->active_route_mask = active_routes;
    slot->transport_config = transport_config;
    ++app->media_slots_consumed;
    app->active_media = slot;
    return MEISO_COMPOSITION_HOST_APP_OK;
}

static bool active_media_scope_current(MeisoCompositionHostApp *app)
{
    MeisoCompositionMediaTransportConfig current;
    size_t index;
    if (app->active_media == NULL || !build_media_config(app, &current) ||
        current.active_route_mask != app->active_media->active_route_mask) {
        return false;
    }
    if (!meiso_session_id_equal(
            current.scope.session_id,
            app->active_media->transport_config.scope.session_id
        ) || current.scope.catalog_generation !=
            app->active_media->transport_config.scope.catalog_generation) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if ((current.active_route_mask & bit) != 0U &&
            (!media_binding_equal(
                &current.routes[index].binding,
                &app->active_media->transport_config.routes[index].binding
             ) || !media_desired_equal(
                &current.routes[index].effective,
                &app->active_media->transport_config.routes[index].effective
             ))) {
            return false;
        }
    }
    return true;
}

static void terminate_active_media(
    MeisoCompositionHostApp *app,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
)
{
    if (app->active_media != NULL) {
        (void)meiso_composition_media_posix_tcp_terminate(
            app->active_media->adapter, reason, error_number
        );
        app->active_media = NULL;
    }
}

static void clear_desired_inflight(HostDesiredChannel *channel)
{
    memset(channel->inflight_payload, 0, sizeof(channel->inflight_payload));
    channel->inflight_payload_size = 0U;
    channel->inflight_command_id = 0U;
    memset(&channel->inflight_owner, 0, sizeof(channel->inflight_owner));
    channel->inflight_version = 0U;
    memset(&channel->inflight_generation, 0,
           sizeof(channel->inflight_generation));
    channel->inflight_present = false;
    channel->inflight_source_completed = false;
    channel->inflight_offer_accepted = false;
    channel->inflight_actual_observed = false;
    channel->offer_queued = false;
    channel->offer_sent = false;
    channel->snapshot.inflight_present = false;
    channel->snapshot.inflight_command_id = 0U;
    memset(&channel->snapshot.inflight_owner, 0,
           sizeof(channel->snapshot.inflight_owner));
    channel->snapshot.inflight_state_version = 0U;
    memset(&channel->snapshot.inflight_delivery_generation, 0,
           sizeof(channel->snapshot.inflight_delivery_generation));
    channel->snapshot.inflight_source_completed = false;
    channel->snapshot.inflight_offer_accepted = false;
    channel->snapshot.inflight_actual_observed = false;
}

static void expose_desired_inflight(HostDesiredChannel *channel)
{
    channel->snapshot.inflight_present = channel->inflight_present;
    channel->snapshot.inflight_command_id = channel->inflight_command_id;
    channel->snapshot.inflight_owner = channel->inflight_owner;
    channel->snapshot.inflight_state_version = channel->inflight_version;
    channel->snapshot.inflight_delivery_generation =
        channel->inflight_generation;
    channel->snapshot.inflight_source_completed =
        channel->inflight_source_completed;
    channel->snapshot.inflight_offer_accepted =
        channel->inflight_offer_accepted;
    channel->snapshot.inflight_actual_observed =
        channel->inflight_actual_observed;
}

static void clear_terminal_views(MeisoCompositionHostApp *app)
{
    const uint64_t transfer_id_high_water =
        app->mesh.transfer_id_high_water;
    size_t index;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT;
         ++index) {
        memset(&app->desired[index].snapshot, 0,
               sizeof(app->desired[index].snapshot));
        clear_desired_inflight(&app->desired[index]);
        app->desired[index].plan_present = false;
        app->desired[index].plan_echoed = false;
        memset(&app->desired[index].plan, 0,
               sizeof(app->desired[index].plan));
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT;
         ++index) {
        memset(&app->actual[index].snapshot, 0,
               sizeof(app->actual[index].snapshot));
        app->actual[index].accept_pending = false;
        app->actual[index].accept_work_queued = false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT;
         ++index) {
        memset(&app->inline_streams[index].snapshot, 0,
               sizeof(app->inline_streams[index].snapshot));
        app->inline_streams[index].producer_present = false;
    }
    memset(&app->mesh, 0, sizeof(app->mesh));
    app->mesh.transfer_id_high_water = transfer_id_high_water;
    memset(app->blob_work_queued, 0, sizeof(app->blob_work_queued));
    app->blob_chunk_queued_bytes = 0U;
    memset(&app->blob_transfer, 0, sizeof(app->blob_transfer));
    app->mesh_size = 0U;
    memset(app->mesh_bytes, 0, app->mesh_capacity);
    app->wifi_effective_present = false;
    memset(&app->wifi_effective, 0, sizeof(app->wifi_effective));
    app->wifi_sample_present = false;
    memset(&app->wifi_sample, 0, sizeof(app->wifi_sample));
    app->presentation_status_present = false;
    memset(&app->presentation_status, 0, sizeof(app->presentation_status));
    app->scene_status_present = false;
    memset(&app->scene_status, 0, sizeof(app->scene_status));
    app->render_result_present = false;
    memset(&app->render_result, 0, sizeof(app->render_result));
    app->device_effective_present = false;
    memset(&app->device_effective, 0, sizeof(app->device_effective));
    app->device_sample_present = false;
    memset(&app->device_sample, 0, sizeof(app->device_sample));
    app->render_media_actual_present = false;
    memset(&app->render_media_actual, 0,
           sizeof(app->render_media_actual));
    app->camera_media_actual_present = false;
    memset(&app->camera_media_actual, 0,
           sizeof(app->camera_media_actual));
    memset(app->desired_scratch, 0, sizeof(app->desired_scratch));
    memset(app->profile_scratch, 0, sizeof(app->profile_scratch));
}

static void enter_terminal(
    MeisoCompositionHostApp *app,
    MeisoCompositionHostAppTerminalReason reason,
    MeisoCompositionMediaTerminalReason media_reason,
    int error_number
)
{
    terminate_active_media(app, media_reason, error_number);
    meiso_composition_debug_peer_abort(app->peer, error_number);
    clear_terminal_views(app);
    app->phase = MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL;
    app->terminal_reason = reason;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_unbind_media_fd(
    MeisoCompositionHostApp *app,
    int error_number
)
{
    MeisoCompositionHostAppStatus status = mutation_status(app);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        return status;
    }
    if (app->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_STATE;
    }
    terminate_active_media(
        app, MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE, error_number
    );
    return MEISO_COMPOSITION_HOST_APP_OK;
}

static bool route_key_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        lhs.endpoint_id.high == rhs.endpoint_id.high &&
        lhs.endpoint_id.low == rhs.endpoint_id.low &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static bool route_channel(
    MeisoCompositionHostApp *app,
    MeisoRuntimeRouteKey route,
    const uint8_t *routes,
    size_t count,
    size_t *out_index
)
{
    size_t index;
    MeisoRuntimeRouteKey expected;
    for (index = 0U; index < count; ++index) {
        if (meiso_composition_debug_peer_route_key(
                app->peer, routes[index], &expected
            ) && route_key_equal(route, expected)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

static bool owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

static bool desired_inflight_matches(
    const HostDesiredChannel *channel,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    return channel->inflight_present &&
        channel->inflight_generation.value == generation.value &&
        owner_equal(channel->inflight_owner, owner) &&
        channel->inflight_version == version;
}

static void retire_completed_desired_inflight(HostDesiredChannel *channel)
{
    if (channel->inflight_present && channel->inflight_source_completed &&
        channel->inflight_offer_accepted &&
        channel->inflight_actual_observed) {
        clear_desired_inflight(channel);
    }
}

static bool start_desired_deliveries(MeisoCompositionHostApp *app)
{
    size_t index;
    if (app->deliveries_started) {
        return true;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT;
         ++index) {
        MeisoStateDeliveryBeginStatus status =
            meiso_state_publisher_begin_delivery(
                app->desired[index].publisher,
                app->desired[index].snapshot.delivery_generation
            );
        if (status != MEISO_STATE_DELIVERY_STARTED) {
            return false;
        }
        app->desired[index].snapshot.delivery_active = true;
    }
    app->deliveries_started = true;
    return true;
}

static bool queue_desired_offer(
    MeisoCompositionHostApp *app,
    size_t index,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    HostDesiredChannel *channel = &app->desired[index];
    MeisoStateOffer offer;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeSchedulerWork work;
    MeisoCompositionDebugPeerStatus status;
    if (!channel->snapshot.requested || channel->inflight_present ||
        !meiso_state_publisher_next_offer(
            channel->publisher, &offer
        )) {
        return false;
    }
    if (offer.state.payload.size > sizeof(channel->inflight_payload) ||
        (offer.state.payload.size != 0U &&
         offer.state.payload.data == NULL)) {
        return false;
    }
    if (!meiso_composition_debug_peer_route_key(
            app->peer, DESIRED_ROUTE_INDEX[index], &route
        )) {
        return false;
    }
    if (offer.state.payload.size != 0U) {
        memcpy(channel->inflight_payload, offer.state.payload.data,
               offer.state.payload.size);
    }
    channel->inflight_payload_size = offer.state.payload.size;
    offer.state.payload.data = channel->inflight_payload;
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = route;
    work.value.state_offer.offer = offer;
    status = meiso_composition_debug_peer_queue_work(
        app->peer, DESIRED_SOURCE_INDEX[index], &work
    );
    if (status != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        memset(channel->inflight_payload, 0,
               sizeof(channel->inflight_payload));
        channel->inflight_payload_size = 0U;
        return false;
    }
    channel->inflight_present = true;
    channel->inflight_source_completed = false;
    channel->inflight_offer_accepted = false;
    channel->inflight_actual_observed = false;
    channel->inflight_command_id = channel->snapshot.command_id;
    channel->inflight_owner = offer.state.owner;
    channel->inflight_version = offer.state.version;
    channel->inflight_generation = offer.generation;
    channel->offer_queued = true;
    channel->offer_sent = false;
    expose_desired_inflight(channel);
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_DESIRED_DELIVERY;
    out_result->command_present = true;
    out_result->command_id = channel->inflight_command_id;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    return true;
}

static bool queue_actual_accept(
    MeisoCompositionHostApp *app,
    size_t index,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    HostActualChannel *channel = &app->actual[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    if (!channel->accept_pending || channel->accept_work_queued ||
        !meiso_composition_debug_peer_route_key(
            app->peer, ACTUAL_ROUTE_INDEX[index], &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
    work.value.state_accept.route = route;
    work.value.state_accept.generation = channel->accept_generation;
    work.value.state_accept.owner = channel->accept_owner;
    work.value.state_accept.version = channel->accept_version;
    if (meiso_composition_debug_peer_queue_work(
            app->peer, ACTUAL_SOURCE_INDEX[index], &work
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    channel->accept_work_queued = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_ACTUAL_UPDATED;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

static bool queue_blob_work(
    MeisoCompositionHostApp *app,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    uint8_t source;
    uint8_t operation_index;
    size_t remaining;
    size_t chunk;
    if (!app->mesh.queued || !meiso_composition_debug_peer_route_key(
            app->peer, ROUTE_MESH_BLOB, &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    if (!app->mesh.transfer_active) {
        operation_index = 0U;
        source = SOURCE_BLOB_BEGIN;
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN;
        work.value.blob_begin.route = route;
        work.value.blob_begin.content_id = app->mesh.content_id;
        work.value.blob_begin.total_size = app->mesh.total_bytes;
    } else if (!app->mesh.begin_ready || app->mesh.waiting_result) {
        return false;
    } else if (app->mesh.next_offset < app->mesh.total_bytes) {
        operation_index = 1U;
        source = SOURCE_BLOB_CHUNK;
        remaining = (size_t)(app->mesh.total_bytes - app->mesh.next_offset);
        chunk = remaining < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ?
            remaining : MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE;
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
        work.value.blob_chunk.transfer = app->blob_transfer;
        work.value.blob_chunk.offset = app->mesh.next_offset;
        work.value.blob_chunk.payload = (MeisoByteView){
            app->mesh_bytes + (size_t)app->mesh.next_offset, chunk
        };
        app->blob_chunk_queued_bytes = (uint32_t)chunk;
    } else {
        operation_index = 2U;
        source = SOURCE_BLOB_COMMIT;
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT;
        work.value.blob_commit.transfer = app->blob_transfer;
    }
    if (app->blob_work_queued[operation_index] ||
        meiso_composition_debug_peer_queue_work(
            app->peer, source, &work
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    app->blob_work_queued[operation_index] = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_MESH_PROGRESS;
    out_result->command_present = true;
    out_result->command_id = app->mesh.command_id;
    return true;
}

static bool handle_source_completion(
    MeisoCompositionHostApp *app,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    MeisoRuntimeSchedulerCompletion completion;
    size_t index;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT;
         ++index) {
        if (meiso_composition_debug_peer_take_completion(
                app->peer, DESIRED_SOURCE_INDEX[index], &completion
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
            HostDesiredChannel *channel = &app->desired[index];
            uint64_t completed_command_id;
            if (!channel->inflight_present || !channel->offer_queued) {
                enter_terminal(
                    app,
                    MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
                );
                return true;
            }
            completed_command_id = channel->inflight_command_id;
            channel->offer_queued = false;
            if (completion.code == MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                channel->offer_sent = true;
                channel->inflight_source_completed = true;
                expose_desired_inflight(channel);
                retire_completed_desired_inflight(channel);
            } else {
                (void)meiso_state_publisher_mark_delivery_failed(
                    channel->publisher, channel->inflight_generation,
                    channel->inflight_owner, channel->inflight_version
                );
                if (channel->snapshot.command_id ==
                        channel->inflight_command_id &&
                    channel->snapshot.state_version ==
                        channel->inflight_version) {
                    channel->snapshot.delivery_failed = true;
                    channel->snapshot.delivery_pending = false;
                }
                clear_desired_inflight(channel);
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_COMPOSITION_HOST_APP_PUMP_DESIRED_DELIVERY;
            out_result->command_present = true;
            out_result->command_id = completed_command_id;
            out_result->desired_channel_present = true;
            out_result->desired_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT;
         ++index) {
        if (meiso_composition_debug_peer_take_completion(
                app->peer, ACTUAL_SOURCE_INDEX[index], &completion
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
            app->actual[index].accept_work_queued = false;
            if (completion.code == MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                app->actual[index].accept_pending = false;
            } else {
                enter_terminal(
                    app,
                    MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
                );
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_ACTUAL_UPDATED;
            out_result->actual_channel_present = true;
            out_result->actual_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U; index < 3U; ++index) {
        const uint8_t source = index == 0U ? SOURCE_BLOB_BEGIN :
            index == 1U ? SOURCE_BLOB_CHUNK : SOURCE_BLOB_COMMIT;
        if (meiso_composition_debug_peer_take_completion(
                app->peer, source, &completion
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
            app->blob_work_queued[index] = false;
            if (completion.code != MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                app->mesh.queued = false;
                app->mesh.transfer_active = false;
                app->mesh.begin_ready = false;
                app->mesh.waiting_result = false;
                app->mesh.terminal_result_present = true;
                app->mesh.terminal_result =
                    MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
            } else if (index == 0U &&
                       completion.generated_blob_transfer_present) {
                app->blob_transfer = completion.generated_blob_transfer;
                app->mesh.transfer_active = true;
                if (completion.generated_blob_transfer.transfer_id >
                    app->mesh.transfer_id_high_water) {
                    app->mesh.transfer_id_high_water =
                        completion.generated_blob_transfer.transfer_id;
                }
            } else if (index == 0U) {
                enter_terminal(
                    app,
                    MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
                );
            } else if (index == 1U) {
                app->mesh.next_offset += app->blob_chunk_queued_bytes;
                app->blob_chunk_queued_bytes = 0U;
            } else if (index == 2U) {
                app->mesh.waiting_result = true;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_MESH_PROGRESS;
            out_result->command_present = true;
            out_result->command_id = app->mesh.command_id;
            return true;
        }
    }
    return false;
}

static bool maybe_echo_presentation_token(MeisoCompositionHostApp *app)
{
    HostDesiredChannel *desired =
        &app->desired[MEISO_COMPOSITION_HOST_APP_DESIRED_PRESENTATION];
    MeisoCompositionScenePresentationPlan plan;
    MeisoRuntimeDesiredState wrapper;
    MeisoByteView payload;
    size_t profile_size = 0U;
    uint64_t version;
    if (!desired->plan_present || desired->plan_echoed ||
        desired->inflight_present ||
        !app->presentation_status_present ||
        app->presentation_status.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
        !app->presentation_status.prepared_token_present ||
        !app->actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION]
             .snapshot.observed_desired.present ||
        !owner_equal(
            app->actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION]
                .snapshot.observed_desired.owner,
            desired->snapshot.owner
        ) || app->actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION]
                    .snapshot.observed_desired.version !=
                desired->snapshot.state_version ||
        app->presentation_status.candidate_plan_generation !=
            desired->plan.plan_generation ||
        memcmp(app->presentation_status.candidate_digest,
               desired->plan.candidate_digest,
               MEISO_SHA256_DIGEST_SIZE) != 0) {
        return true;
    }
    if (desired->snapshot.state_version == UINT64_MAX) {
        return false;
    }
    plan = desired->plan;
    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo,
           app->presentation_status.prepared_token,
           MEISO_SHA256_DIGEST_SIZE);
    if (meiso_composition_scene_presentation_plan_encode(
            &plan,
            (MeisoMutableBuffer){app->profile_scratch,
                                 sizeof(app->profile_scratch)},
            &profile_size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return false;
    }
    wrapper.enabled = true;
    wrapper.profile = (MeisoByteView){app->profile_scratch, profile_size};
    if (!meiso_runtime_desired_state_encode(
            &wrapper,
            (MeisoMutableBuffer){app->desired_scratch,
                                 sizeof(app->desired_scratch)},
            &payload
        )) {
        return false;
    }
    version = desired->snapshot.state_version + 1U;
    if (meiso_state_publisher_publish(
            desired->publisher, desired->snapshot.owner, version, payload
        ) != MEISO_STATE_PUBLISH_NEW) {
        return false;
    }
    desired->plan = plan;
    desired->plan_echoed = true;
    desired->snapshot.state_version = version;
    desired->snapshot.delivery_pending = true;
    desired->snapshot.delivery_accepted = false;
    desired->snapshot.delivery_failed = false;
    return true;
}

static bool decode_actual_profile(
    MeisoCompositionHostApp *app,
    size_t index,
    MeisoByteView payload
)
{
    HostActualChannel *channel = &app->actual[index];
    MeisoRuntimeActualState actual;
    if (!meiso_runtime_actual_state_decode(payload, &actual)) {
        return false;
    }
    channel->snapshot.observed_desired = actual.observed_desired;
    channel->snapshot.applied_desired = actual.applied_desired;
    channel->snapshot.apply_result = actual.apply_result;
    channel->snapshot.availability = actual.availability;
    channel->snapshot.effective_enabled = actual.effective_enabled;
    channel->snapshot.production_status = actual.production_status;
    channel->snapshot.reason = actual.reason;
    channel->snapshot.production_epoch = actual.production_epoch;
    channel->snapshot.profile_decoded = false;
    if (!actual.effective_enabled && actual.profile.size == 0U) {
        switch (index) {
        case MEISO_COMPOSITION_HOST_APP_ACTUAL_WIFI:
            app->wifi_effective_present = false;
            break;
        case MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION:
            app->presentation_status_present = false;
            break;
        case MEISO_COMPOSITION_HOST_APP_ACTUAL_SCENE:
            app->scene_status_present = false;
            break;
        case MEISO_COMPOSITION_HOST_APP_ACTUAL_DEVICE:
            app->device_effective_present = false;
            break;
        case MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA:
            app->render_media_actual_present = false;
            break;
        case MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA:
            app->camera_media_actual_present = false;
            break;
        default:
            return false;
        }
        channel->snapshot.profile_decoded = true;
        return true;
    }
    switch (index) {
    case MEISO_COMPOSITION_HOST_APP_ACTUAL_WIFI:
        if (meiso_wifi_telemetry_effective_parameters_decode(
                actual.profile, &app->wifi_effective
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) return false;
        app->wifi_effective_present = true;
        break;
    case MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION:
        if (meiso_composition_scene_presentation_status_decode(
                actual.profile, &app->presentation_status
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) return false;
        app->presentation_status_present = true;
        break;
    case MEISO_COMPOSITION_HOST_APP_ACTUAL_SCENE:
        if (meiso_composition_scene_status_decode(
                actual.profile, &app->scene_status
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) return false;
        app->scene_status_present = true;
        break;
    case MEISO_COMPOSITION_HOST_APP_ACTUAL_DEVICE:
        if (meiso_device_telemetry_parameters_decode(
                actual.profile, &app->device_effective
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK) return false;
        app->device_effective_present = true;
        break;
    case MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA:
        if (meiso_composition_media_actual_decode(
                actual.profile, &app->render_media_actual
            ) != MEISO_COMPOSITION_SESSION_V3_OK ||
            app->render_media_actual.source !=
                MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) return false;
        app->render_media_actual_present = true;
        break;
    case MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA:
        if (meiso_composition_media_actual_decode(
                actual.profile, &app->camera_media_actual
            ) != MEISO_COMPOSITION_SESSION_V3_OK ||
            app->camera_media_actual.source !=
                MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) return false;
        app->camera_media_actual_present = true;
        break;
    default:
        return false;
    }
    channel->snapshot.profile_decoded = true;
    return true;
}

static void note_desired_actual_observed(
    MeisoCompositionHostApp *app,
    size_t index
)
{
    HostDesiredChannel *desired;
    const MeisoRuntimeDesiredReference *observed;
    if (index >= MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT) {
        return;
    }
    desired = &app->desired[index];
    observed = &app->actual[index].snapshot.observed_desired;
    if (!desired->inflight_present || !observed->present ||
        !owner_equal(desired->inflight_owner, observed->owner) ||
        desired->inflight_version != observed->version) {
        return;
    }
    desired->inflight_actual_observed = true;
    expose_desired_inflight(desired);
    retire_completed_desired_inflight(desired);
}

static bool handle_actual_offer(
    MeisoCompositionHostApp *app,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    size_t index;
    HostActualChannel *channel;
    MeisoStateReceiveStatus receive;
    MeisoStateView current;
    if (!route_channel(app, event->route, ACTUAL_ROUTE_INDEX,
                       MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT, &index)) {
        return false;
    }
    channel = &app->actual[index];
    if (!channel->owner_started) {
        if (event->state_offer.state.owner.role != MEISO_OWNER_ROLE_EDGE ||
            meiso_state_replica_begin_owner(
                channel->replica, event->state_offer.state.owner
            ) != MEISO_STATE_OWNER_STARTED) {
            return false;
        }
        channel->owner_started = true;
        channel->snapshot.state_owner = event->state_offer.state.owner;
    } else if (!owner_equal(
                   channel->snapshot.state_owner,
                   event->state_offer.state.owner
               )) {
        return false;
    }
    receive = meiso_state_replica_receive(
        channel->replica, event->state_offer.state.owner,
        event->state_offer.state.version,
        event->state_offer.state.payload
    );
    if (meiso_composition_debug_peer_commit_state_receive(
            app->peer, event->route, &event->state_offer, receive
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    if (receive != MEISO_STATE_RECEIVE_NEW &&
        receive != MEISO_STATE_RECEIVE_DUPLICATE) {
        return false;
    }
    if (!meiso_state_replica_current(channel->replica, &current) ||
        !decode_actual_profile(app, index, current.payload)) {
        return false;
    }
    channel->snapshot.present = true;
    channel->snapshot.state_owner = current.owner;
    channel->snapshot.state_version = current.version;
    note_desired_actual_observed(app, index);
    channel->accept_pending = true;
    channel->accept_generation = event->state_offer.generation;
    channel->accept_owner = event->state_offer.state.owner;
    channel->accept_version = event->state_offer.state.version;
    if (app->active_media != NULL && !active_media_scope_current(app)) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_ACTUAL_UPDATED;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

static bool handle_desired_accept(
    MeisoCompositionHostApp *app,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    size_t index;
    HostDesiredChannel *channel;
    MeisoStateAcceptStatus accept_status;
    uint64_t accepted_command_id;
    if (!route_channel(app, event->route, DESIRED_ROUTE_INDEX,
                       MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT, &index)) {
        return false;
    }
    channel = &app->desired[index];
    if (!desired_inflight_matches(
            channel, event->state_accept_generation,
            event->state_accept_owner, event->state_accept_version
        )) {
        return false;
    }
    accept_status = meiso_state_publisher_mark_delivery_accepted(
        channel->publisher, event->state_accept_generation,
        event->state_accept_owner, event->state_accept_version
    );
    if (accept_status != MEISO_STATE_ACCEPT_CONFIRMED &&
        accept_status != MEISO_STATE_ACCEPT_DUPLICATE &&
        accept_status != MEISO_STATE_ACCEPT_STALE) {
        return false;
    }
    accepted_command_id = channel->inflight_command_id;
    channel->inflight_offer_accepted = true;
    if (channel->snapshot.command_id == channel->inflight_command_id &&
        channel->snapshot.state_version == channel->inflight_version) {
        channel->snapshot.delivery_pending = false;
        channel->snapshot.delivery_accepted = true;
        channel->snapshot.delivery_failed = false;
    }
    expose_desired_inflight(channel);
    retire_completed_desired_inflight(channel);
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_DESIRED_DELIVERY;
    out_result->command_present = true;
    out_result->command_id = accepted_command_id;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    return true;
}

static bool decode_inline(
    MeisoCompositionHostApp *app,
    size_t index,
    MeisoByteView payload
)
{
    switch (index) {
    case MEISO_COMPOSITION_HOST_APP_INLINE_WIFI:
        if (meiso_wifi_telemetry_sample_decode(
                payload, &app->wifi_sample
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) return false;
        app->wifi_sample_present = true;
        return true;
    case MEISO_COMPOSITION_HOST_APP_INLINE_RENDER_RESULT:
        if (meiso_composition_scene_render_result_decode(
                payload, &app->render_result
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) return false;
        app->render_result_present = true;
        return true;
    case MEISO_COMPOSITION_HOST_APP_INLINE_DEVICE:
        if (meiso_device_telemetry_sample_decode(
                payload, &app->device_sample
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK) return false;
        app->device_sample_present = true;
        return true;
    default:
        return false;
    }
}

static bool handle_inline_sample(
    MeisoCompositionHostApp *app,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    size_t index;
    HostInlineChannel *channel;
    MeisoStreamReceiveResult receive;
    MeisoStreamSample sample;
    if (!route_channel(app, event->route, INLINE_ROUTE_INDEX,
                       MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT,
                       &index)) {
        return false;
    }
    channel = &app->inline_streams[index];
    if (!channel->producer_present ||
        !owner_equal(channel->producer, event->stream_sample.key.producer) ||
        channel->epoch != event->production_epoch) {
        MeisoStreamProducerStatus producer_status = meiso_stream_begin_producer(
            channel->stream, event->stream_sample.key.producer,
            event->production_epoch
        );
        if (producer_status != MEISO_STREAM_PRODUCER_STARTED &&
            producer_status != MEISO_STREAM_PRODUCER_SWITCHED &&
            producer_status != MEISO_STREAM_PRODUCER_UNCHANGED) {
            return false;
        }
        channel->producer_present = true;
        channel->producer = event->stream_sample.key.producer;
        channel->epoch = event->production_epoch;
    }
    receive = meiso_stream_receive(channel->stream, &event->stream_sample, NULL);
    if (receive.code == MEISO_STREAM_RECEIVE_DUPLICATE ||
        receive.code == MEISO_STREAM_RECEIVE_STALE) {
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_INLINE_UPDATED;
        out_result->inline_channel_present = true;
        out_result->inline_channel = (uint8_t)index;
        return true;
    }
    if (receive.code != MEISO_STREAM_RECEIVE_ACCEPTED ||
        !meiso_stream_peek(channel->stream, &sample) ||
        !decode_inline(app, index, sample.payload)) {
        return false;
    }
    channel->snapshot.present = true;
    channel->snapshot.key = sample.key;
    channel->snapshot.source_time = sample.source_time;
    channel->snapshot.receive_flags = receive.flags;
    channel->snapshot.missing_before = receive.missing_before;
    ++channel->snapshot.received_count;
    if ((receive.flags & MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST) != 0U)
        ++channel->snapshot.replaced_count;
    if (!meiso_stream_pop(channel->stream)) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_INLINE_UPDATED;
    out_result->inline_channel_present = true;
    out_result->inline_channel = (uint8_t)index;
    return true;
}

static bool handle_blob_result(
    MeisoCompositionHostApp *app,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    if (!app->mesh.transfer_active ||
        event->blob_transfer.transfer_id != app->blob_transfer.transfer_id ||
        !route_key_equal(event->blob_transfer.route,
                         app->blob_transfer.route)) {
        return false;
    }
    if (event->blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
        event->blob_result == MEISO_RUNTIME_BLOB_RESULT_READY) {
        app->mesh.begin_ready = true;
    } else {
        app->mesh.terminal_result_present = true;
        app->mesh.terminal_result = event->blob_result;
        app->mesh.edge_core_published =
            event->blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
            event->blob_result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
        app->mesh.queued = false;
        app->mesh.transfer_active = false;
        app->mesh.waiting_result = false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_MESH_PROGRESS;
    out_result->command_present = true;
    out_result->command_id = app->mesh.command_id;
    return true;
}

static bool handle_runtime_event(
    MeisoCompositionHostApp *app,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    switch (event->type) {
    case MEISO_RUNTIME_EVENT_STATE_OFFER:
        return handle_actual_offer(app, event, out_result);
    case MEISO_RUNTIME_EVENT_STATE_ACCEPT:
        return handle_desired_accept(app, event, out_result);
    case MEISO_RUNTIME_EVENT_STREAM_SAMPLE:
        return handle_inline_sample(app, event, out_result);
    case MEISO_RUNTIME_EVENT_BLOB_RESULT:
        return handle_blob_result(app, event, out_result);
    case MEISO_RUNTIME_EVENT_CATALOG_OFFERED:
    case MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED:
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_CONTROL_PROGRESS;
        return true;
    default:
        return false;
    }
}

static void refresh_phase(MeisoCompositionHostApp *app)
{
    MeisoCompositionDebugPeerSnapshot peer;
    if (!meiso_composition_debug_peer_snapshot(app->peer, &peer)) {
        enter_terminal(
            app, MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return;
    }
    if (peer.runtime.active_catalog_generation != 0U &&
        peer.runtime.active_catalog_generation != app->config.catalog_generation) {
        enter_terminal(
            app, MEISO_COMPOSITION_HOST_APP_TERMINAL_CATALOG_CHANGED,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED, EPROTO
        );
        return;
    }
    if (peer.terminal) {
        enter_terminal(
            app,
            peer.session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST ?
                MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_TRANSPORT :
                MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_SESSION,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
    } else if (peer.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY) {
        app->phase = MEISO_COMPOSITION_HOST_APP_PHASE_READY;
    } else if (peer.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_REBIND) {
        app->phase = MEISO_COMPOSITION_HOST_APP_PHASE_REBIND;
    } else if (peer.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_CATALOG) {
        app->phase = MEISO_COMPOSITION_HOST_APP_PHASE_CATALOG;
    } else {
        app->phase = MEISO_COMPOSITION_HOST_APP_PHASE_HANDSHAKE;
    }
}

static MeisoCompositionHostAppPumpStatus pump_media(
    MeisoCompositionHostApp *app,
    const MeisoCompositionHostAppIoReady *ready,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    MeisoCompositionMediaPosixTcpReadStatus read_status;
    MeisoCompositionMediaPosixTcpWriteStatus write_status;
    MeisoCompositionMediaPosixTcpPrepareStatus prepare_status;
    MeisoCompositionMediaPosixTcpSnapshot before;
    MeisoCompositionMediaTransportSnapshot transport;
    size_t index;
    if (app->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
    }
    if (!active_media_scope_current(app)) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, EPROTO
        );
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY;
        return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    if (ready->media_readable) {
        read_status = meiso_composition_media_posix_tcp_on_readable(
            app->active_media->adapter
        );
        if (read_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK ||
            read_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED) {
            return MEISO_COMPOSITION_HOST_APP_PUMP_WOULD_BLOCK;
        }
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST ||
            read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INVALID_ARGUMENT) {
            terminate_active_media(
                app, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
                EPROTO
            );
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY;
            return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_PROGRESS;
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAME_PUBLISHED &&
            meiso_composition_media_transport_snapshot(
                app->active_media->transport, &transport
            ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            for (index = 0U;
                 index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
                 ++index) {
                if (transport.routes[index].published_sequence_high_water !=
                    app->active_media->published_high_water[index]) {
                    app->active_media->published_high_water[index] =
                        transport.routes[index].published_sequence_high_water;
                    out_result->event =
                        MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_FRAME;
                    out_result->media_source_present = true;
                    out_result->media_source = transport.routes[index].source;
                    break;
                }
            }
        }
        return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    if (!meiso_composition_media_posix_tcp_snapshot(
            app->active_media->adapter, &before
        )) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
    }
    prepare_status = meiso_composition_media_posix_tcp_prepare_outbound(
        app->active_media->adapter
    );
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TRANSPORT_INVALID ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_INVALID_ARGUMENT) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
    }
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY) {
        if (!ready->media_writable) {
            if (!before.tx_packet_borrowed) {
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_PROGRESS;
                return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
            }
            return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
        }
        write_status = meiso_composition_media_posix_tcp_on_writable(
            app->active_media->adapter
        );
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED) {
            return MEISO_COMPOSITION_HOST_APP_PUMP_WOULD_BLOCK;
        }
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INVALID_ARGUMENT) {
            terminate_active_media(
                app, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
                EPROTO
            );
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY;
            return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_PROGRESS;
        return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
}

static MeisoCompositionHostAppPumpStatus pump_local(
    MeisoCompositionHostApp *app,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    size_t index;
    if (app->phase != MEISO_COMPOSITION_HOST_APP_PHASE_READY) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
    }
    if (!app->deliveries_started) {
        if (!start_desired_deliveries(app)) {
            enter_terminal(
                app,
                MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
                MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
            );
            return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY;
        return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    if (handle_source_completion(app, out_result)) {
        return app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL ?
            MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL :
            MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT;
         ++index) {
        if (queue_actual_accept(app, index, out_result)) {
            return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
        }
    }
    if (!maybe_echo_presentation_token(app)) {
        enter_terminal(
            app, MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT;
         ++index) {
        if (queue_desired_offer(app, index, out_result)) {
            return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
        }
    }
    if (queue_blob_work(app, out_result)) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
}

static MeisoCompositionHostAppPumpStatus pump_control(
    MeisoCompositionHostApp *app,
    const MeisoCompositionHostAppIoReady *ready,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    MeisoCompositionDebugPeerPumpResult peer_result;
    MeisoCompositionDebugPeerPumpStatus peer_status;
    MeisoCompositionHostAppPumpResult result;
    app->mutable_active = true;
    peer_status = meiso_composition_debug_peer_pump(
        app->peer, ready->control_readable, ready->control_writable,
        &peer_result
    );
    app->mutable_active = false;
    refresh_phase(app);
    if (app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL ||
        peer_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL;
    }
    if (peer_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK ||
        peer_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TX_BUSY) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_WOULD_BLOCK;
    }
    if (peer_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
    }
    if (peer_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT) {
        if (!peer_result.event_present || !handle_runtime_event(
                app, &peer_result.event, &result
            )) {
            enter_terminal(
                app,
                MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
                MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
            );
            return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
        }
        refresh_phase(app);
        *out_result = result;
        return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
    }
    if (peer_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_RX_OBLIGATION) {
        enter_terminal(
            app, MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
    }
    if (peer_status > MEISO_COMPOSITION_DEBUG_PEER_PUMP_SOURCE_INVALID) {
        enter_terminal(
            app, MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE;
    }
    memset(&result, 0, sizeof(result));
    result.event = MEISO_COMPOSITION_HOST_APP_PUMP_CONTROL_PROGRESS;
    result.event_present = true;
    *out_result = result;
    return MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS;
}

MeisoCompositionHostAppPumpStatus meiso_composition_host_app_pump(
    MeisoCompositionHostApp *app,
    const MeisoCompositionHostAppIoReady *ready,
    MeisoCompositionHostAppPumpResult *out_result
)
{
    MeisoCompositionHostAppPumpResult result;
    MeisoCompositionHostAppPumpStatus status;
    uint8_t scan;
    if (!app_valid(app) || ready == NULL || out_result == NULL ||
        app_range_overlap(app, ready, sizeof(*ready)) ||
        app_range_overlap(app, out_result, sizeof(*out_result)) ||
        ranges_overlap(ready, sizeof(*ready), out_result,
                       sizeof(*out_result))) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_INVALID_ARGUMENT;
    }
    if (app->mutable_active) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_INVALID_ARGUMENT;
    }
    if (app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL;
    }
    refresh_phase(app);
    if (app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL;
    }
    for (scan = 0U; scan < 3U; ++scan) {
        const uint8_t domain = (uint8_t)((app->fairness_cursor + scan) % 3U);
        if (domain == 0U) {
            status = pump_local(app, &result);
        } else if (domain == 1U) {
            status = pump_media(app, ready, &result);
        } else {
            status = pump_control(app, ready, &result);
        }
        if (status == MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS) {
            app->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            *out_result = result;
            return status;
        }
        if (status == MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL ||
            status == MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE) {
            return status;
        }
        if (status == MEISO_COMPOSITION_HOST_APP_PUMP_WOULD_BLOCK) {
            app->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            return status;
        }
    }
    return MEISO_COMPOSITION_HOST_APP_PUMP_IDLE;
}

bool meiso_composition_host_app_readiness(
    const MeisoCompositionHostApp *app,
    MeisoCompositionHostAppIoInterest *out_interest
)
{
    MeisoCompositionHostAppIoInterest result;
    if (!app_valid(app) || out_interest == NULL ||
        app_range_overlap(app, out_interest, sizeof(*out_interest)) ||
        app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL) {
        return false;
    }
    memset(&result, 0, sizeof(result));
    result.control_read = meiso_composition_debug_peer_wants_read(app->peer);
    result.control_write = meiso_composition_debug_peer_wants_write(app->peer);
    if (app->active_media != NULL) {
        result.media_read = meiso_composition_media_posix_tcp_wants_read(
            app->active_media->adapter
        );
        result.media_write = meiso_composition_media_posix_tcp_wants_write(
            app->active_media->adapter
        );
    }
    *out_interest = result;
    return true;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_media_latest(
    const MeisoCompositionHostApp *app,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest
)
{
    MeisoCompositionMediaTransportStatus status;
    if (!app_valid(app) || out_latest == NULL ||
        app_range_overlap(app, out_latest, sizeof(*out_latest)) ||
        (source != MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
         source != MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA)) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_APP_TERMINAL;
    }
    if (app->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_APP_NO_FRAME;
    }
    status = meiso_composition_media_transport_latest(
        app->active_media->transport, source, out_latest
    );
    if (status == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return MEISO_COMPOSITION_HOST_APP_OK;
    }
    return status == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME ?
        MEISO_COMPOSITION_HOST_APP_NO_FRAME :
        MEISO_COMPOSITION_HOST_APP_INVALID_STATE;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_media_release(
    MeisoCompositionHostApp *app,
    const MeisoCompositionMediaFrameRef *expected_frame
)
{
    MeisoCompositionHostAppStatus status = mutation_status(app);
    MeisoCompositionMediaTransportStatus media_status;
    if (status != MEISO_COMPOSITION_HOST_APP_OK) return status;
    if (expected_frame == NULL ||
        app_range_overlap(app, expected_frame, sizeof(*expected_frame))) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (app->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_APP_NO_FRAME;
    }
    media_status = meiso_composition_media_transport_release_latest(
        app->active_media->transport, expected_frame
    );
    if (media_status == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return MEISO_COMPOSITION_HOST_APP_OK;
    }
    return media_status == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME ?
        MEISO_COMPOSITION_HOST_APP_NO_FRAME :
        MEISO_COMPOSITION_HOST_APP_FRAME_MISMATCH;
}

bool meiso_composition_host_app_snapshot(
    const MeisoCompositionHostApp *app,
    MeisoCompositionHostAppSnapshot *out_snapshot
)
{
    MeisoCompositionHostAppSnapshot snapshot;
    MeisoCompositionDebugPeerSnapshot peer;
    MeisoCompositionMediaTransportSnapshot media;
    MeisoCompositionMediaLatestView latest;
    MeisoCompositionMediaRouteConfig current_media_route;
    size_t index;
    if (!app_valid(app) || out_snapshot == NULL ||
        app_range_overlap(app, out_snapshot, sizeof(*out_snapshot)) ||
        !meiso_composition_debug_peer_snapshot(app->peer, &peer)) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.phase = app->phase;
    snapshot.terminal_reason = app->terminal_reason;
    snapshot.control_session_state = peer.session.state;
    snapshot.control_session_id = peer.session.session_id;
    snapshot.active_catalog_generation = peer.runtime.active_catalog_generation;
    snapshot.control_ready = peer.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY;
    snapshot.media_bound = app->active_media != NULL;
    snapshot.media_slots_consumed = app->media_slots_consumed;
    snapshot.media_slots_capacity = app->config.media_connection_slots;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT; ++index)
        snapshot.desired[index] = app->desired[index].snapshot;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT; ++index)
        snapshot.actual[index] = app->actual[index].snapshot;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT;
         ++index)
        snapshot.inline_streams[index] = app->inline_streams[index].snapshot;
    snapshot.mesh = app->mesh;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
         ++index) {
        snapshot.media[index].source = index == 0U ?
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        if (media_route_config(app, &peer, index, &current_media_route)) {
            snapshot.media[index].actual_binding_ready = true;
            snapshot.media[index].binding = current_media_route.binding;
        }
    }
    if (app->active_media != NULL &&
        meiso_composition_media_transport_snapshot(
            app->active_media->transport, &media
        ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        snapshot.media_active_route_mask = media.configured_active_route_mask;
        for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
             ++index) {
            if ((media.configured_active_route_mask &
                 (uint8_t)(UINT8_C(1) << index)) != 0U) {
                snapshot.media[index].source = media.routes[index].source;
                snapshot.media[index].actual_binding_ready = true;
                snapshot.media[index].binding =
                    app->active_media->transport_config.routes[index].binding;
                snapshot.media[index].latest_present =
                    media.routes[index].receive_latest;
                snapshot.media[index].latest_frame_sequence =
                    media.routes[index].receive_frame_sequence;
                snapshot.media[index].published_sequence_high_water =
                    media.routes[index].published_sequence_high_water;
                snapshot.media[index].sequence_gap_frames =
                    media.routes[index].sequence_gap_frames;
                snapshot.media[index].producer_drop_high_water =
                    media.routes[index].producer_drop_high_water;
                snapshot.media[index].reassembly_frames_dropped =
                    media.routes[index].reassembly_frames_dropped;
                snapshot.media[index].unread_latest_frames_dropped =
                    media.routes[index].unread_latest_frames_dropped;
            }
            if ((media.configured_active_route_mask &
                 (uint8_t)(UINT8_C(1) << index)) != 0U &&
                media.routes[index].receive_latest &&
                meiso_composition_media_transport_latest(
                    app->active_media->transport,
                    media.routes[index].source, &latest
                ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
                snapshot.media[index].latest_frame_sequence =
                    latest.ref.frame_sequence;
                snapshot.media[index].latest_source_time =
                    latest.unit.source_time;
                snapshot.media[index].latest_access_unit_bytes =
                    (uint32_t)latest.unit.access_unit.size;
            }
        }
    }
    snapshot.wifi_effective_present = app->wifi_effective_present;
    snapshot.wifi_effective = app->wifi_effective;
    snapshot.wifi_sample_present = app->wifi_sample_present;
    snapshot.wifi_sample = app->wifi_sample;
    snapshot.presentation_status_present = app->presentation_status_present;
    snapshot.presentation_status = app->presentation_status;
    snapshot.scene_status_present = app->scene_status_present;
    snapshot.scene_status = app->scene_status;
    snapshot.render_result_present = app->render_result_present;
    snapshot.render_result = app->render_result;
    snapshot.device_effective_present = app->device_effective_present;
    snapshot.device_effective = app->device_effective;
    snapshot.device_sample_present = app->device_sample_present;
    snapshot.device_sample = app->device_sample;
    snapshot.render_media_actual_present = app->render_media_actual_present;
    snapshot.render_media_actual = app->render_media_actual;
    snapshot.camera_media_actual_present = app->camera_media_actual_present;
    snapshot.camera_media_actual = app->camera_media_actual;
    snapshot.next_command_id = app->next_command_id;
    snapshot.plan_generation_high_water = app->plan_generation_high_water;
    snapshot.arena_bytes = app->arena_bytes;
    snapshot.control_peer_bytes = app->control_peer_bytes;
    snapshot.core_state_bytes = app->core_state_bytes;
    snapshot.core_stream_bytes = app->core_stream_bytes;
    snapshot.mesh_staging_bytes = app->mesh_staging_bytes;
    snapshot.media_slot_bytes = app->media_slot_bytes;
    *out_snapshot = snapshot;
    return true;
}

MeisoCompositionHostAppStatus meiso_composition_host_app_abort(
    MeisoCompositionHostApp *app,
    MeisoCompositionHostAppTerminalReason reason,
    int error_number
)
{
    MeisoCompositionMediaTerminalReason media_reason;
    if (!app_valid(app) || reason == MEISO_COMPOSITION_HOST_APP_TERMINAL_NONE ||
        reason > MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT;
    }
    if (app->mutable_active) {
        return MEISO_COMPOSITION_HOST_APP_BUSY;
    }
    if (app->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_APP_TERMINAL;
    }
    if (reason == MEISO_COMPOSITION_HOST_APP_TERMINAL_CATALOG_CHANGED) {
        media_reason = MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED;
    } else if (reason == MEISO_COMPOSITION_HOST_APP_TERMINAL_LOCAL_ABORT) {
        media_reason = MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE;
    } else {
        media_reason = MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION;
    }
    app->mutable_active = true;
    enter_terminal(app, reason, media_reason, error_number);
    app->mutable_active = false;
    return MEISO_COMPOSITION_HOST_APP_OK;
}
