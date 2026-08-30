/* Host Runtime arena layout, configuration validation and initialization. */
#include "meiso_composition_host_runtime_private.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>

const uint8_t meiso_composition_host_runtime_private_desired_route_index[
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT
] = {
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_PRESENTATION_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_DESIRED
};

const uint8_t meiso_composition_host_runtime_private_desired_source_index[
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT
] = {
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_WIFI_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_PRESENTATION_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_SCENE_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_DEVICE_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_RENDER_MEDIA_DESIRED,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CAMERA_MEDIA_DESIRED
};

const uint8_t meiso_composition_host_runtime_private_actual_route_index[
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT
] = {
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_ACTUAL,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_PRESENTATION_ACTUAL,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_ACTUAL,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_ACTUAL,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_ACTUAL,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_ACTUAL
};

const uint8_t meiso_composition_host_runtime_private_actual_source_index[
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT
] = {
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_WIFI_ACTUAL_ACCEPT,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_PRESENTATION_ACTUAL_ACCEPT,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_SCENE_ACTUAL_ACCEPT,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_DEVICE_ACTUAL_ACCEPT,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_RENDER_MEDIA_ACTUAL_ACCEPT,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CAMERA_MEDIA_ACTUAL_ACCEPT
};

const uint8_t meiso_composition_host_runtime_private_inline_route_index[
    MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT
] = {
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_INLINE, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_INLINE, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_INLINE
};

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

bool meiso_composition_host_runtime_private_ranges_overlap(
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
    MeisoCompositionControlSessionConfig *config,
    uint8_t index,
    MeisoRuntimeSchedulerWorkKind kind,
    uint8_t route_index
)
{
    config->sources[index].work_kind = kind;
    config->sources[index].route_index = route_index;
}

static void configure_peer_sources(MeisoCompositionControlSessionConfig *config)
{
    size_t index;
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CATALOG,
                MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT,
                MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_WIFI_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_DESIRED);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_WIFI_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_ACTUAL);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_BEGIN,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_CHUNK,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_COMMIT,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_ABORT,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_PRESENTATION_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_PRESENTATION_DESIRED);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_PRESENTATION_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_PRESENTATION_ACTUAL);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_SCENE_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_DESIRED);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_SCENE_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_ACTUAL);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_DEVICE_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_DESIRED);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_DEVICE_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_ACTUAL);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_RENDER_MEDIA_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_DESIRED);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_RENDER_MEDIA_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_ACTUAL);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CAMERA_MEDIA_DESIRED,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_DESIRED);
    peer_source(config, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CAMERA_MEDIA_ACTUAL_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_ACTUAL);
    for (index = 17U; index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT;
         ++index) {
        peer_source(config, (uint8_t)index,
                    MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_DESIRED);
    }
}

static bool control_session_config_from_host(
    const MeisoCompositionHostRuntimeConfig *config,
    MeisoCompositionControlSessionConfig *out_config
)
{
    MeisoCompositionControlSessionConfig candidate;
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
    const MeisoCompositionHostRuntimeConfig *config,
    MeisoCompositionProfileRouteDescriptor *out_routes
)
{
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT
    ];
    uint8_t capability[MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES];
    return config != NULL && out_routes != NULL &&
        meiso_composition_profile_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){capability, sizeof(capability)},
            endpoints, MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            out_routes, MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) == MEISO_COMPOSITION_PROFILE_OK;
}

static bool config_shape_valid(const MeisoCompositionHostRuntimeConfig *config)
{
    MeisoCompositionControlSessionConfig control_session_config;
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    return config != NULL && config->control_connected_fd >= 0 &&
        config->local_incarnation != 0U &&
        meiso_session_nonce_valid(config->local_nonce) &&
        config->catalog_generation != 0U &&
        config->max_control_io_bytes_per_step != 0U &&
        config->control_adapter_reserve_bytes >=
            MEISO_COMPOSITION_HOST_RUNTIME_CONTROL_ADAPTER_MIN_RESERVE_BYTES &&
        config->control_runtime_reserve_bytes != 0U &&
        config->control_scheduler_reserve_bytes != 0U &&
        meiso_delivery_generation_valid(
            config->initial_desired_delivery_generation
        ) && config->media_connection_slots != 0U &&
        config->media_connection_slots <=
            MEISO_COMPOSITION_HOST_RUNTIME_MAX_MEDIA_CONNECTIONS &&
        config->max_media_io_bytes_per_step != 0U &&
        control_session_config_from_host(config, &control_session_config) &&
        meiso_composition_control_session_memory_required(&control_session_config) != 0U &&
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
    const MeisoCompositionHostRuntimeConfig *config,
    MeisoCompositionHostRuntimePrivateArenaLayout *out_layout,
    MeisoCompositionProfileRouteDescriptor *out_routes
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
        MEISO_COMPOSITION_HOST_RUNTIME_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    MeisoCompositionControlSessionConfig control_session_config;
    MeisoCompositionHostRuntimePrivateArenaLayout layout;
    size_t cursor;
    size_t index;
    size_t slots_bytes;
    uint32_t max_fragment;

    if (out_layout == NULL || out_routes == NULL ||
        !config_shape_valid(config) ||
        !control_session_config_from_host(config, &control_session_config) ||
        !build_catalog_for_config(config, out_routes)) {
        return false;
    }
    memset(&layout, 0, sizeof(layout));
    if (!align_up(sizeof(MeisoCompositionHostRuntime), alignment, &cursor)) {
        return false;
    }
    layout.control_session_bytes =
        meiso_composition_control_session_memory_required(&control_session_config);
    if (!add_aligned_region(
            &cursor, layout.control_session_bytes, alignment, &layout.control_session_offset
        )) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        layout.publisher_bytes[index] =
            meiso_state_publisher_memory_required(
                &out_routes[meiso_composition_host_runtime_private_desired_route_index[index]].route.resource,
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
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT;
         ++index) {
        layout.replica_bytes[index] = meiso_state_replica_memory_required(
            &out_routes[meiso_composition_host_runtime_private_actual_route_index[index]].route.resource
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
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT;
         ++index) {
        layout.stream_bytes[index] = meiso_stream_memory_required(
            &out_routes[meiso_composition_host_runtime_private_inline_route_index[index]].route.resource,
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
    layout.mesh_bytes = out_routes[MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB].route.resource
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
            MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MEDIA_ADAPTER_RESERVE_BYTES,
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

size_t meiso_composition_host_runtime_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_composition_host_runtime_memory_required(
    const MeisoCompositionHostRuntimeConfig *config
)
{
    MeisoCompositionHostRuntimePrivateArenaLayout layout;
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    return calculate_layout(config, &layout, routes) ? layout.total : 0U;
}

bool meiso_composition_host_runtime_private_fd_preflight(int fd)
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

bool meiso_composition_host_runtime_private_fd_prepare_send_safety(int fd)
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

MeisoCompositionHostRuntimeInitStatus meiso_composition_host_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionHostRuntimeConfig *config,
    MeisoCompositionHostRuntime **out_runtime
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_COMPOSITION_HOST_RUNTIME_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    MeisoCompositionControlSessionConfig control_session_config;
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    MeisoCompositionHostRuntimePrivateArenaLayout layout;
    MeisoCompositionHostRuntime *runtime;
    MeisoCompositionControlSession *initialized_control_session;
    size_t index;
    if (arena == NULL || config == NULL || out_runtime == NULL ||
        meiso_composition_host_runtime_private_ranges_overlap(arena, arena_size, config, sizeof(*config)) ||
        meiso_composition_host_runtime_private_ranges_overlap(arena, arena_size, out_runtime, sizeof(*out_runtime)) ||
        meiso_composition_host_runtime_private_ranges_overlap(config, sizeof(*config), out_runtime, sizeof(*out_runtime))) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INIT_INVALID_ARGUMENT;
    }
    if (!calculate_layout(config, &layout, routes) ||
        !control_session_config_from_host(config, &control_session_config)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INIT_INVALID_CONFIG;
    }
    if (((uintptr_t)arena % alignof(max_align_t)) != 0U) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < layout.total) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INIT_ARENA_TOO_SMALL;
    }
    if (!meiso_composition_host_runtime_private_fd_preflight(config->control_connected_fd) ||
        !meiso_composition_host_runtime_private_fd_prepare_send_safety(config->control_connected_fd)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INIT_FD_ERROR;
    }

    if (meiso_composition_control_session_init(
            (uint8_t *)arena + layout.control_session_offset, layout.control_session_bytes,
            &control_session_config, &initialized_control_session
        ) != MEISO_COMPOSITION_CONTROL_SESSION_INIT_OK) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INIT_INTERNAL_FAILURE;
    }
    /* Complete layout/config/fd preflight makes child failure unreachable for
     * the same config and a caller-stable fd in this single-threaded call.
     * The defensive status remains because a child failure may have written
     * its subarena; in that case out_runtime stays unchanged and the arena is
     * intentionally invalidated, as the public init contract states.
     * Remaining child layouts/descriptors are the exact values precomputed
     * above and therefore have no reachable failure result. */
    memset(arena, 0, sizeof(MeisoCompositionHostRuntime));
    runtime = (MeisoCompositionHostRuntime *)arena;
    runtime->arena_begin = (uint8_t *)arena;
    runtime->arena_bytes = layout.total;
    runtime->config = *config;
    memcpy(runtime->catalog_routes, routes, sizeof(routes));
    runtime->control_session = initialized_control_session;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        (void)meiso_state_publisher_init(
            runtime->arena_begin + layout.publisher_offset[index],
            layout.publisher_bytes[index],
            &routes[meiso_composition_host_runtime_private_desired_route_index[index]].route.resource,
            &state_config, &runtime->desired[index].publisher
        );
        runtime->desired[index].snapshot.delivery_generation =
            config->initial_desired_delivery_generation;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT;
         ++index) {
        (void)meiso_state_replica_init(
            runtime->arena_begin + layout.replica_offset[index],
            layout.replica_bytes[index],
            &routes[meiso_composition_host_runtime_private_actual_route_index[index]].route.resource,
            &runtime->actual[index].replica
        );
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT;
         ++index) {
        (void)meiso_stream_init(
            runtime->arena_begin + layout.stream_offset[index],
            layout.stream_bytes[index],
            &routes[meiso_composition_host_runtime_private_inline_route_index[index]].route.resource,
            &stream_config, &runtime->inline_streams[index].stream
        );
    }
    runtime->mesh_bytes = runtime->arena_begin + layout.mesh_offset;
    runtime->mesh_capacity = layout.mesh_bytes;
    memset(runtime->mesh_bytes, 0, runtime->mesh_capacity);
    memset(runtime->arena_begin + layout.media_slots_offset, 0,
           layout.media_slot_bytes * config->media_connection_slots);
    for (index = 0U; index < config->media_connection_slots; ++index) {
        runtime->media_slots[index].offset =
            layout.media_slots_offset + index * layout.media_slot_bytes;
        runtime->media_slots[index].fd = -1;
    }
    runtime->phase = MEISO_COMPOSITION_HOST_RUNTIME_PHASE_HANDSHAKE;
    runtime->next_command_id = 1U;
    runtime->control_session_bytes = layout.control_session_bytes;
    runtime->core_state_bytes = layout.core_state_bytes;
    runtime->core_stream_bytes = layout.core_stream_bytes;
    runtime->mesh_staging_bytes = layout.mesh_bytes;
    runtime->media_slot_bytes = layout.media_slot_bytes;
    runtime->media_transport_bytes = layout.media_transport_bytes;
    runtime->magic = MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MAGIC;
    *out_runtime = runtime;
    return MEISO_COMPOSITION_HOST_RUNTIME_INIT_OK;
}

bool meiso_composition_host_runtime_private_valid(const MeisoCompositionHostRuntime *runtime)
{
    return runtime != NULL && runtime->magic == MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MAGIC &&
        runtime->arena_begin == (const uint8_t *)runtime &&
        runtime->arena_bytes >= sizeof(*runtime) && runtime->control_session != NULL;
}

bool meiso_composition_host_runtime_private_range_overlap(
    const MeisoCompositionHostRuntime *runtime,
    const void *value,
    size_t size
)
{
    return meiso_composition_host_runtime_private_valid(runtime) && meiso_composition_host_runtime_private_ranges_overlap(
        runtime->arena_begin, runtime->arena_bytes, value, size
    );
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_private_mutation_status(
    MeisoCompositionHostRuntime *runtime
)
{
    if (!meiso_composition_host_runtime_private_valid(runtime)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->mutable_active) {
        return MEISO_COMPOSITION_HOST_RUNTIME_BUSY;
    }
    if (runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL;
    }
    return MEISO_COMPOSITION_HOST_RUNTIME_OK;
}

bool meiso_composition_host_runtime_private_receipt_valid(
    const MeisoCompositionHostRuntime *runtime,
    const void *input,
    size_t input_size,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    return out_receipt != NULL &&
        !meiso_composition_host_runtime_private_range_overlap(runtime, out_receipt, sizeof(*out_receipt)) &&
        (input == NULL || !meiso_composition_host_runtime_private_range_overlap(runtime, input, input_size)) &&
        (input == NULL || !meiso_composition_host_runtime_private_ranges_overlap(
            input, input_size, out_receipt, sizeof(*out_receipt)
        ));
}
