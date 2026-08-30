#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Arena layout, configuration validation, initialization, and shared invariants. */

const uint8_t meiso_k230d_composition_edge_runtime_private_desired_route_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT
] = {
    ROUTE_WIFI_DESIRED,
    ROUTE_PRESENTATION_DESIRED,
    ROUTE_SCENE_DESIRED,
    ROUTE_DEVICE_DESIRED,
    ROUTE_RENDER_MEDIA_DESIRED,
    ROUTE_CAMERA_MEDIA_DESIRED
};

const uint8_t meiso_k230d_composition_edge_runtime_private_actual_route_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT
] = {
    ROUTE_WIFI_ACTUAL,
    ROUTE_PRESENTATION_ACTUAL,
    ROUTE_SCENE_ACTUAL,
    ROUTE_DEVICE_ACTUAL,
    ROUTE_RENDER_MEDIA_ACTUAL,
    ROUTE_CAMERA_MEDIA_ACTUAL
};

const uint8_t meiso_k230d_composition_edge_runtime_private_inline_route_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT
] = {
    ROUTE_WIFI_INLINE, ROUTE_RENDER_INLINE, ROUTE_DEVICE_INLINE
};

const uint8_t meiso_k230d_composition_edge_runtime_private_desired_source_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT
] = {
    SOURCE_WIFI_DESIRED_ACCEPT,
    SOURCE_PRESENTATION_DESIRED_ACCEPT,
    SOURCE_SCENE_DESIRED_ACCEPT,
    SOURCE_DEVICE_DESIRED_ACCEPT,
    SOURCE_RENDER_MEDIA_DESIRED_ACCEPT,
    SOURCE_CAMERA_MEDIA_DESIRED_ACCEPT
};

const uint8_t meiso_k230d_composition_edge_runtime_private_actual_source_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT
] = {
    SOURCE_WIFI_ACTUAL,
    SOURCE_PRESENTATION_ACTUAL,
    SOURCE_SCENE_ACTUAL,
    SOURCE_DEVICE_ACTUAL,
    SOURCE_RENDER_MEDIA_ACTUAL,
    SOURCE_CAMERA_MEDIA_ACTUAL
};

const uint8_t meiso_k230d_composition_edge_runtime_private_inline_source_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT
] = {
    SOURCE_WIFI_INLINE, SOURCE_RENDER_INLINE, SOURCE_DEVICE_INLINE
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

void meiso_k230d_composition_edge_runtime_private_cadence_reset(EdgeCadence *cadence)
{
    if (cadence != NULL) {
        memset(cadence, 0, sizeof(*cadence));
    }
}

bool meiso_k230d_composition_edge_runtime_private_cadence_ready(
    EdgeCadence *cadence,
    uint64_t production_epoch,
    uint32_t rate_millihz,
    uint64_t now_ns
)
{
    if (cadence == NULL || production_epoch == 0U || rate_millihz == 0U) {
        return false;
    }
    if (cadence->production_epoch != production_epoch ||
        cadence->rate_millihz != rate_millihz) {
        meiso_k230d_composition_edge_runtime_private_cadence_reset(cadence);
        cadence->production_epoch = production_epoch;
        cadence->rate_millihz = rate_millihz;
    }
    return !cadence->armed || now_ns >= cadence->next_due_ns;
}

bool meiso_k230d_composition_edge_runtime_private_cadence_commit(EdgeCadence *cadence, uint64_t now_ns)
{
    const uint64_t period_numerator = UINT64_C(1000000000000);
    uint64_t numerator;
    uint64_t delta_ns;
    if (cadence == NULL || cadence->production_epoch == 0U ||
        cadence->rate_millihz == 0U ||
        cadence->remainder > UINT64_MAX - period_numerator) {
        return false;
    }
    numerator = period_numerator + cadence->remainder;
    delta_ns = numerator / cadence->rate_millihz;
    if (delta_ns == 0U || now_ns > UINT64_MAX - delta_ns) {
        return false;
    }
    cadence->remainder = numerator % cadence->rate_millihz;
    cadence->next_due_ns = now_ns + delta_ns;
    cadence->armed = true;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_ranges_overlap(
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

static bool bounded_string_size(
    const char *value,
    size_t capacity,
    size_t *out_size
)
{
    size_t index;
    if (value == NULL || out_size == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        if (value[index] == '\0') {
            *out_size = index + 1U;
            return index != 0U;
        }
    }
    return false;
}

static void control_session_source(
    MeisoCompositionControlSessionConfig *config,
    uint8_t index,
    MeisoRuntimeSchedulerWorkKind kind,
    uint8_t route_index
)
{
    config->sources[index].work_kind = kind;
    config->sources[index].route_index = route_index;
}

static void configure_control_session_sources(MeisoCompositionControlSessionConfig *config)
{
    size_t index;
    control_session_source(config, SOURCE_CATALOG,
                MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
                MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE);
    control_session_source(config, SOURCE_WIFI_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_WIFI_DESIRED);
    control_session_source(config, SOURCE_WIFI_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_WIFI_ACTUAL);
    control_session_source(config, SOURCE_WIFI_INLINE,
                MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                ROUTE_WIFI_INLINE);
    control_session_source(config, SOURCE_BLOB_RESULT,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT,
                ROUTE_MESH_BLOB);
    control_session_source(config, SOURCE_PRESENTATION_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_PRESENTATION_DESIRED);
    control_session_source(config, SOURCE_PRESENTATION_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_PRESENTATION_ACTUAL);
    control_session_source(config, SOURCE_SCENE_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_SCENE_DESIRED);
    control_session_source(config, SOURCE_SCENE_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_SCENE_ACTUAL);
    control_session_source(config, SOURCE_RENDER_INLINE,
                MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                ROUTE_RENDER_INLINE);
    control_session_source(config, SOURCE_DEVICE_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_DEVICE_DESIRED);
    control_session_source(config, SOURCE_DEVICE_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_DEVICE_ACTUAL);
    control_session_source(config, SOURCE_DEVICE_INLINE,
                MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                ROUTE_DEVICE_INLINE);
    control_session_source(config, SOURCE_RENDER_MEDIA_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_RENDER_MEDIA_DESIRED);
    control_session_source(config, SOURCE_RENDER_MEDIA_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_RENDER_MEDIA_ACTUAL);
    control_session_source(config, SOURCE_CAMERA_MEDIA_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_CAMERA_MEDIA_DESIRED);
    control_session_source(config, SOURCE_CAMERA_MEDIA_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_CAMERA_MEDIA_ACTUAL);
    for (index = 17U; index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT;
         ++index) {
        control_session_source(config, (uint8_t)index,
                    MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                    ROUTE_WIFI_ACTUAL);
    }
}

static MeisoCompositionMediaCapability render_capability(void)
{
    return (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ * 1000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE,
        0U
    };
}

static MeisoCompositionMediaCapability camera_capability(void)
{
    return (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        MEISO_CAMERA_V4L2_WIDTH,
        MEISO_CAMERA_V4L2_HEIGHT,
        MEISO_CAMERA_V4L2_WIDTH,
        MEISO_CAMERA_V4L2_HEIGHT,
        30000U,
        MEISO_CAMERA_V4L2_SIZE_IMAGE,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE,
        0U
    };
}

static MeisoCompositionProfileCatalogConfig catalog_config_for_fields(
    const MeisoK230dCompositionEdgeRuntimeConfig *config,
    MeisoWifiTelemetryFieldMask wifi_fields,
    MeisoDeviceTelemetryFieldMask device_fields
)
{
    MeisoCompositionProfileCatalogConfig value;
    memset(&value, 0, sizeof(value));
    value.wifi_incarnation = config->wifi_incarnation;
    value.presentation_incarnation = config->presentation_incarnation;
    value.scene_incarnation = config->scene_incarnation;
    value.device_telemetry_incarnation =
        config->device_telemetry_incarnation;
    value.render_media_incarnation = config->render_media_incarnation;
    value.camera_media_incarnation = config->camera_media_incarnation;
    value.wifi_capability = (MeisoWifiTelemetryCapability){
        wifi_fields,
        MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        device_fields,
        MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS,
        MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS,
        MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS,
        -40000,
        125000,
        MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC
    };
    value.render_media_capability = render_capability();
    value.camera_media_capability = camera_capability();
    return value;
}

static bool control_session_config_from_edge(
    const MeisoK230dCompositionEdgeRuntimeConfig *config,
    const MeisoCompositionProfileCatalogConfig *catalog,
    MeisoCompositionControlSessionConfig *out_config
)
{
    MeisoCompositionControlSessionConfig value;
    if (config == NULL || catalog == NULL || out_config == NULL) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.connected_fd = config->control_connected_fd;
    value.local_role = MEISO_SESSION_ROLE_EDGE;
    value.local_peer_id = config->local_peer_id;
    value.local_incarnation = config->local_incarnation;
    value.local_nonce = config->local_nonce;
    value.expected_peer_id = config->expected_host_peer_id;
    value.protocol = config->protocol;
    value.catalog_generation = config->catalog_generation;
    value.catalog = *catalog;
    value.max_io_bytes_per_step = config->max_control_io_bytes_per_step;
    value.adapter_reserve_bytes = config->control_adapter_reserve_bytes;
    value.runtime_reserve_bytes = config->control_runtime_reserve_bytes;
    value.scheduler_reserve_bytes = config->control_scheduler_reserve_bytes;
    configure_control_session_sources(&value);
    *out_config = value;
    return true;
}

static bool build_catalog(
    const MeisoCompositionProfileCatalogConfig *catalog,
    MeisoCompositionProfileRouteDescriptor *out_routes
)
{
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT
    ];
    uint8_t capability[MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES];
    return catalog != NULL && out_routes != NULL &&
        meiso_composition_profile_build_catalog(
            catalog,
            (MeisoMutableBuffer){capability, sizeof(capability)},
            endpoints, MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            out_routes, MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) == MEISO_COMPOSITION_PROFILE_OK;
}

static bool config_shape_valid(const MeisoK230dCompositionEdgeRuntimeConfig *config)
{
    MeisoWifiLinuxAdapter wifi;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoCompositionProfileCatalogConfig catalog;
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    MeisoCompositionControlSessionConfig control_session_config;
    size_t path_size;
    if (config == NULL || config->control_connected_fd < 0 ||
        config->local_incarnation == 0U ||
        !meiso_session_nonce_valid(config->local_nonce) ||
        config->catalog_generation == 0U ||
        config->wifi_incarnation == 0U ||
        config->presentation_incarnation == 0U ||
        config->scene_incarnation == 0U ||
        config->device_telemetry_incarnation == 0U ||
        config->render_media_incarnation == 0U ||
        config->camera_media_incarnation == 0U ||
        config->edge_clock_epoch == 0U ||
        config->max_control_io_bytes_per_step == 0U ||
        config->control_adapter_reserve_bytes == 0U ||
        config->control_runtime_reserve_bytes == 0U ||
        config->control_scheduler_reserve_bytes == 0U ||
        !meiso_delivery_generation_valid(
            config->initial_actual_delivery_generation
        ) || config->media_connection_slots == 0U ||
        config->media_connection_slots >
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MAX_MEDIA_CONNECTIONS ||
        config->max_media_io_bytes_per_step == 0U ||
        config->thermal_worker_budget_ms == 0U ||
        !bounded_string_size(
            config->camera.device_path,
            MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY,
            &path_size
        )) {
        return false;
    }
    memset(&wifi, 0, sizeof(wifi));
    memset(&device, 0, sizeof(device));
    if (meiso_wifi_linux_adapter_init(&wifi, &config->wifi) !=
            MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_device_telemetry_linux_adapter_init(
            &device, &config->device_telemetry
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK) {
        return false;
    }
    catalog = catalog_config_for_fields(
        config,
        config->wifi.allowed_fields & MEISO_WIFI_TELEMETRY_FIELD_ALL,
        config->device_telemetry.allowed_fields &
            MEISO_DEVICE_TELEMETRY_FIELD_ALL
    );
    return control_session_config_from_edge(config, &catalog, &control_session_config) &&
        meiso_composition_control_session_memory_required(&control_session_config) != 0U &&
        build_catalog(&catalog, routes);
}

static bool calculate_layout(
    const MeisoK230dCompositionEdgeRuntimeConfig *config,
    const MeisoCompositionProfileCatalogConfig *catalog,
    K230dCompositionEdgeRuntimeLayout *out_layout,
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
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    const MeisoBlobConfig blob_config = {
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MESH_BLOB_SLOTS
    };
    MeisoCompositionControlSessionConfig control_session_config;
    K230dCompositionEdgeRuntimeLayout layout;
    size_t cursor;
    size_t slots_bytes;
    size_t index;
    uint32_t max_fragment;
    if (config == NULL || catalog == NULL || out_layout == NULL ||
        out_routes == NULL || !config_shape_valid(config) ||
        !control_session_config_from_edge(config, catalog, &control_session_config) ||
        !build_catalog(catalog, out_routes)) {
        return false;
    }
    memset(&layout, 0, sizeof(layout));
    if (!align_up(sizeof(MeisoK230dCompositionEdgeRuntime), alignment, &cursor)) {
        return false;
    }
    layout.control_session_bytes =
        meiso_composition_control_session_memory_required(&control_session_config);
    if (!add_aligned_region(
            &cursor, layout.control_session_bytes, alignment, &layout.control_session_offset
        )) {
        return false;
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT;
         ++index) {
        layout.replica_bytes[index] = meiso_state_replica_memory_required(
            &out_routes[meiso_k230d_composition_edge_runtime_private_desired_route_index[index]].route.resource
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
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT;
         ++index) {
        layout.publisher_bytes[index] =
            meiso_state_publisher_memory_required(
                &out_routes[meiso_k230d_composition_edge_runtime_private_actual_route_index[index]].route.resource,
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
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT;
         ++index) {
        layout.stream_bytes[index] = meiso_stream_memory_required(
            &out_routes[meiso_k230d_composition_edge_runtime_private_inline_route_index[index]].route.resource,
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
    layout.blob_bytes = meiso_blob_store_memory_required(
        &out_routes[ROUTE_MESH_BLOB].route.resource, &blob_config
    );
    layout.scene_bytes = meiso_composition_scene_store_memory_required();
    layout.coordinator_bytes = meiso_composition_coordinator_memory_required();
    layout.renderer_bytes =
        meiso_composition_cpu_raster_renderer_memory_required();
    layout.camera_bytes = meiso_camera_v4l2_memory_required();
    if (!add_aligned_region(
            &cursor, layout.blob_bytes, alignment, &layout.blob_offset
        ) || !add_aligned_region(
            &cursor, layout.scene_bytes, alignment, &layout.scene_offset
        ) || !add_aligned_region(
            &cursor, layout.coordinator_bytes, alignment,
            &layout.coordinator_offset
        ) || !add_aligned_region(
            &cursor, layout.renderer_bytes, alignment,
            &layout.renderer_offset
        ) || !add_aligned_region(
            &cursor, layout.camera_bytes, alignment, &layout.camera_offset
        )) {
        return false;
    }
    max_fragment = catalog->render_media_capability
            .max_fragment_payload_bytes >
            catalog->camera_media_capability.max_fragment_payload_bytes ?
        catalog->render_media_capability.max_fragment_payload_bytes :
        catalog->camera_media_capability.max_fragment_payload_bytes;
    layout.media_transport_bytes =
        MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES +
        2U * ((size_t)catalog->render_media_capability
                    .max_access_unit_bytes +
              (size_t)catalog->camera_media_capability
                    .max_access_unit_bytes) +
        MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + max_fragment;
    if (!align_up(
            layout.media_transport_bytes, alignment,
            &layout.media_transport_bytes
        ) || !checked_add(
            layout.media_transport_bytes, EDGE_MEDIA_ADAPTER_RESERVE_BYTES,
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

size_t meiso_k230d_composition_edge_runtime_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_k230d_composition_edge_runtime_memory_required(
    const MeisoK230dCompositionEdgeRuntimeConfig *config
)
{
    MeisoCompositionProfileCatalogConfig catalog;
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    K230dCompositionEdgeRuntimeLayout layout;
    if (!config_shape_valid(config)) {
        return 0U;
    }
    catalog = catalog_config_for_fields(
        config,
        config->wifi.allowed_fields & MEISO_WIFI_TELEMETRY_FIELD_ALL,
        config->device_telemetry.allowed_fields &
            MEISO_DEVICE_TELEMETRY_FIELD_ALL
    );
    return calculate_layout(config, &catalog, &layout, routes) ?
        layout.total : 0U;
}

bool meiso_k230d_composition_edge_runtime_private_fd_preflight(int fd)
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

bool meiso_k230d_composition_edge_runtime_private_fd_prepare_send_safety(int fd)
{
#if defined(MSG_NOSIGNAL)
    (void)fd;
    return true;
#elif defined(SO_NOSIGPIPE)
    const int enabled = 1;
    return setsockopt(
               fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
               (socklen_t)sizeof(enabled)
           ) == 0;
#else
    (void)fd;
    return false;
#endif
}

static bool config_ranges_disjoint(
    void *arena,
    size_t arena_size,
    const MeisoK230dCompositionEdgeRuntimeConfig *config,
    MeisoK230dCompositionEdgeRuntime **out_runtime
)
{
    EdgeRange ranges[16];
    size_t count = 0U;
    size_t size;
    size_t left;
    size_t right;
#define ADD_PATH(value, capacity)                                             \
    do {                                                                      \
        if (!bounded_string_size((value), (capacity), &size)) {              \
            return false;                                                     \
        }                                                                     \
        ranges[count++] = (EdgeRange){(value), size};                         \
    } while (0)
    if (arena == NULL || config == NULL || out_runtime == NULL) {
        return false;
    }
    ranges[count++] = (EdgeRange){arena, arena_size};
    ranges[count++] = (EdgeRange){config, sizeof(*config)};
    ranges[count++] = (EdgeRange){out_runtime, sizeof(*out_runtime)};
    if (config->wifi.sys_class_net_root != NULL) {
        ADD_PATH(config->wifi.sys_class_net_root,
                 MEISO_WIFI_LINUX_LOCATOR_CAPACITY);
    }
    if (config->wifi.proc_net_wireless_path != NULL) {
        ADD_PATH(config->wifi.proc_net_wireless_path,
                 MEISO_WIFI_LINUX_LOCATOR_CAPACITY);
    }
    ADD_PATH(config->device_telemetry.proc_stat_path,
             MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY);
    ADD_PATH(config->device_telemetry.proc_meminfo_path,
             MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY);
    ADD_PATH(config->device_telemetry.thermal_type_path,
             MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY);
    ADD_PATH(config->device_telemetry.thermal_value_path,
             MEISO_DEVICE_TELEMETRY_LINUX_PATH_CAPACITY);
    ADD_PATH(config->camera.device_path,
             MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY);
    if (config->camera.syscalls != NULL) {
        if (count >= sizeof(ranges) / sizeof(ranges[0])) {
            return false;
        }
        ranges[count++] = (EdgeRange){
            config->camera.syscalls, sizeof(*config->camera.syscalls)
        };
    }
    if (config->camera.syscall_context != NULL) {
        if (count >= sizeof(ranges) / sizeof(ranges[0])) {
            return false;
        }
        ranges[count++] = (EdgeRange){config->camera.syscall_context, 1U};
    }
    for (left = 0U; left < count; ++left) {
        for (right = left + 1U; right < count; ++right) {
            if (meiso_k230d_composition_edge_runtime_private_ranges_overlap(
                    ranges[left].data, ranges[left].size,
                    ranges[right].data, ranges[right].size
                )) {
                return false;
            }
        }
    }
#undef ADD_PATH
    return true;
}

MeisoOwnerScope meiso_k230d_composition_edge_runtime_private_edge_owner(
    const MeisoK230dCompositionEdgeRuntimeConfig *config
)
{
    return (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE, config->local_incarnation
    };
}

MeisoK230dCompositionEdgeRuntimeInitStatus meiso_k230d_composition_edge_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoK230dCompositionEdgeRuntimeConfig *config,
    MeisoK230dCompositionEdgeRuntime **out_runtime
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    const MeisoBlobConfig blob_config = {
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MESH_BLOB_SLOTS
    };
    MeisoWifiLinuxAdapter wifi;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoWifiTelemetryFieldMask wifi_fields;
    MeisoDeviceTelemetryCapability device_capability;
    MeisoCompositionProfileCatalogConfig catalog;
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    MeisoCompositionControlSessionConfig control_session_config;
    K230dCompositionEdgeRuntimeLayout layout;
    MeisoK230dCompositionEdgeRuntime *runtime;
    MeisoCompositionControlSession *control_session;
    size_t index;

    if (!config_ranges_disjoint(arena, arena_size, config, out_runtime)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INVALID_ARGUMENT;
    }
    if (!config_shape_valid(config)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INVALID_CONFIG;
    }
    if (((uintptr_t)arena % alignof(max_align_t)) != 0U) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_MISALIGNED_ARENA;
    }
    if (!meiso_k230d_composition_edge_runtime_private_fd_preflight(config->control_connected_fd) ||
        !meiso_k230d_composition_edge_runtime_private_fd_prepare_send_safety(config->control_connected_fd)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_FD_ERROR;
    }
    memset(&wifi, 0, sizeof(wifi));
    if (meiso_wifi_linux_adapter_init(&wifi, &config->wifi) !=
            MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_wifi_linux_adapter_probe(&wifi, &wifi_fields) !=
            MEISO_WIFI_LINUX_ADAPTER_OK) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_PROBE_UNAVAILABLE;
    }
    memset(&device, 0, sizeof(device));
    if (meiso_device_telemetry_linux_adapter_init(
            &device, &config->device_telemetry
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK ||
        meiso_device_telemetry_linux_adapter_probe(
            &device, &device_capability
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_PROBE_UNAVAILABLE;
    }
    catalog = catalog_config_for_fields(
        config, wifi_fields, device_capability.supported_fields
    );
    catalog.device_telemetry_capability = device_capability;
    if (!calculate_layout(config, &catalog, &layout, routes) ||
        arena_size < layout.total ||
        !control_session_config_from_edge(config, &catalog, &control_session_config)) {
        return arena_size < meiso_k230d_composition_edge_runtime_memory_required(config) ?
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_ARENA_TOO_SMALL :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_CAPACITY;
    }
    if (meiso_composition_control_session_init(
            (uint8_t *)arena + layout.control_session_offset, layout.control_session_bytes,
            &control_session_config, &control_session
        ) != MEISO_COMPOSITION_CONTROL_SESSION_INIT_OK) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INTERNAL_FAILURE;
    }

    memset(arena, 0, sizeof(MeisoK230dCompositionEdgeRuntime));
    runtime = (MeisoK230dCompositionEdgeRuntime *)arena;
    runtime->arena_begin = (uint8_t *)arena;
    runtime->arena_bytes = layout.total;
    runtime->config = *config;
    runtime->config.wifi.sys_class_net_root = NULL;
    runtime->config.wifi.proc_net_wireless_path = NULL;
    runtime->config.device_telemetry.proc_stat_path = NULL;
    runtime->config.device_telemetry.proc_meminfo_path = NULL;
    runtime->config.device_telemetry.thermal_type_path = NULL;
    runtime->config.device_telemetry.thermal_value_path = NULL;
    runtime->config.camera.device_path = NULL;
    runtime->config.camera.syscalls = NULL;
    runtime->control_session = control_session;
    runtime->catalog = catalog;
    runtime->wifi = wifi;
    runtime->device = device;
    runtime->wifi_capability = catalog.wifi_capability;
    runtime->device_capability = catalog.device_telemetry_capability;
    memcpy(runtime->catalog_routes, routes, sizeof(routes));

    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT;
         ++index) {
        if (meiso_state_replica_init(
                runtime->arena_begin + layout.replica_offset[index],
                layout.replica_bytes[index],
                &routes[meiso_k230d_composition_edge_runtime_private_desired_route_index[index]].route.resource,
                &runtime->desired[index].replica
            ) != MEISO_STATE_INIT_OK) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INTERNAL_FAILURE;
        }
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT;
         ++index) {
        if (meiso_state_publisher_init(
                runtime->arena_begin + layout.publisher_offset[index],
                layout.publisher_bytes[index],
                &routes[meiso_k230d_composition_edge_runtime_private_actual_route_index[index]].route.resource,
                &state_config, &runtime->actual[index].publisher
            ) != MEISO_STATE_INIT_OK ||
            meiso_state_publisher_begin_delivery(
                runtime->actual[index].publisher,
                config->initial_actual_delivery_generation
            ) != MEISO_STATE_DELIVERY_STARTED) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INTERNAL_FAILURE;
        }
        runtime->actual[index].snapshot.delivery_generation =
            config->initial_actual_delivery_generation;
    }
    for (index = 0U;
         index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT; ++index) {
        if (meiso_stream_init(
                runtime->arena_begin + layout.stream_offset[index],
                layout.stream_bytes[index],
                &routes[meiso_k230d_composition_edge_runtime_private_inline_route_index[index]].route.resource,
                &stream_config, &runtime->inline_streams[index].stream
            ) != MEISO_STREAM_INIT_OK) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INTERNAL_FAILURE;
        }
    }
    if (meiso_blob_store_init(
            runtime->arena_begin + layout.blob_offset, layout.blob_bytes,
            &routes[ROUTE_MESH_BLOB].route.resource, &blob_config,
            &runtime->blob_store
        ) != MEISO_BLOB_INIT_OK ||
        meiso_composition_scene_store_init(
            runtime->arena_begin + layout.scene_offset, layout.scene_bytes,
            runtime->blob_store, &runtime->scene_store
        ) != MEISO_COMPOSITION_SCENE_STORE_INIT_OK ||
        meiso_composition_cpu_raster_renderer_init(
            runtime->arena_begin + layout.renderer_offset,
            layout.renderer_bytes, runtime->blob_store, &runtime->renderer
        ) != MEISO_COMPOSITION_CPU_RASTER_INIT_OK ||
        meiso_camera_v4l2_init(
            runtime->arena_begin + layout.camera_offset, layout.camera_bytes,
            &config->camera, &runtime->camera
        ) != MEISO_CAMERA_V4L2_INIT_OK) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INTERNAL_FAILURE;
    }
    memset(runtime->arena_begin + layout.coordinator_offset, 0,
           layout.coordinator_bytes);
    memset(runtime->arena_begin + layout.media_slots_offset, 0,
           layout.media_slot_bytes * config->media_connection_slots);
    for (index = 0U; index < config->media_connection_slots; ++index) {
        runtime->media_slots[index].offset =
            layout.media_slots_offset + index * layout.media_slot_bytes;
        runtime->media_slots[index].fd = -1;
    }
    runtime->phase = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_HANDSHAKE;
    runtime->next_thermal_request_id = 1U;
    runtime->control_session_bytes = layout.control_session_bytes;
    runtime->blob_store_bytes = layout.blob_bytes;
    runtime->scene_store_bytes = layout.scene_bytes;
    runtime->coordinator_bytes = layout.coordinator_bytes;
    runtime->renderer_bytes = layout.renderer_bytes;
    runtime->camera_bytes = layout.camera_bytes;
    runtime->core_state_bytes = layout.core_state_bytes;
    runtime->core_stream_bytes = layout.core_stream_bytes;
    runtime->media_slot_bytes = layout.media_slot_bytes;
    runtime->media_transport_bytes = layout.media_transport_bytes;
    runtime->coordinator_arena =
        runtime->arena_begin + layout.coordinator_offset;
    runtime->magic = K230D_COMPOSITION_EDGE_RUNTIME_MAGIC;
    (void)meiso_k230d_composition_edge_runtime_private_edge_owner(config);
    *out_runtime = runtime;
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_OK;
}

bool meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(const MeisoK230dCompositionEdgeRuntime *runtime)
{
    return runtime != NULL && runtime->magic == K230D_COMPOSITION_EDGE_RUNTIME_MAGIC &&
        runtime->arena_begin == (const uint8_t *)runtime &&
        runtime->arena_bytes >= sizeof(*runtime) && runtime->control_session != NULL &&
        runtime->blob_store != NULL && runtime->scene_store != NULL &&
        runtime->renderer != NULL && runtime->camera != NULL;
}

bool meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    const void *value,
    size_t size
)
{
    return meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) && meiso_k230d_composition_edge_runtime_private_ranges_overlap(
        runtime->arena_begin, runtime->arena_bytes, value, size
    );
}

bool meiso_k230d_composition_edge_runtime_private_owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

bool meiso_k230d_composition_edge_runtime_private_route_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}
