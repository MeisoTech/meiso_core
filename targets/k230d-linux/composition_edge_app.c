#define _POSIX_C_SOURCE 200809L

#include "composition_edge_app.h"

#include <fcntl.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>

enum {
    EDGE_APP_MAGIC = 0x45415050U, /* EAPP */
    EDGE_MEDIA_ADAPTER_RESERVE_BYTES = 16384U,

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
    SOURCE_WIFI_DESIRED_ACCEPT = 1,
    SOURCE_WIFI_ACTUAL = 2,
    SOURCE_WIFI_INLINE = 3,
    SOURCE_BLOB_RESULT = 4,
    SOURCE_PRESENTATION_DESIRED_ACCEPT = 5,
    SOURCE_PRESENTATION_ACTUAL = 6,
    SOURCE_SCENE_DESIRED_ACCEPT = 7,
    SOURCE_SCENE_ACTUAL = 8,
    SOURCE_RENDER_INLINE = 9,
    SOURCE_DEVICE_DESIRED_ACCEPT = 10,
    SOURCE_DEVICE_ACTUAL = 11,
    SOURCE_DEVICE_INLINE = 12,
    SOURCE_RENDER_MEDIA_DESIRED_ACCEPT = 13,
    SOURCE_RENDER_MEDIA_ACTUAL = 14,
    SOURCE_CAMERA_MEDIA_DESIRED_ACCEPT = 15,
    SOURCE_CAMERA_MEDIA_ACTUAL = 16
};

static const uint8_t DESIRED_ROUTE_INDEX[
    MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT
] = {
    ROUTE_WIFI_DESIRED,
    ROUTE_PRESENTATION_DESIRED,
    ROUTE_SCENE_DESIRED,
    ROUTE_DEVICE_DESIRED,
    ROUTE_RENDER_MEDIA_DESIRED,
    ROUTE_CAMERA_MEDIA_DESIRED
};

static const uint8_t ACTUAL_ROUTE_INDEX[
    MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT
] = {
    ROUTE_WIFI_ACTUAL,
    ROUTE_PRESENTATION_ACTUAL,
    ROUTE_SCENE_ACTUAL,
    ROUTE_DEVICE_ACTUAL,
    ROUTE_RENDER_MEDIA_ACTUAL,
    ROUTE_CAMERA_MEDIA_ACTUAL
};

static const uint8_t INLINE_ROUTE_INDEX[
    MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT
] = {
    ROUTE_WIFI_INLINE, ROUTE_RENDER_INLINE, ROUTE_DEVICE_INLINE
};

static const uint8_t DESIRED_SOURCE_INDEX[
    MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT
] = {
    SOURCE_WIFI_DESIRED_ACCEPT,
    SOURCE_PRESENTATION_DESIRED_ACCEPT,
    SOURCE_SCENE_DESIRED_ACCEPT,
    SOURCE_DEVICE_DESIRED_ACCEPT,
    SOURCE_RENDER_MEDIA_DESIRED_ACCEPT,
    SOURCE_CAMERA_MEDIA_DESIRED_ACCEPT
};

static const uint8_t ACTUAL_SOURCE_INDEX[
    MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT
] = {
    SOURCE_WIFI_ACTUAL,
    SOURCE_PRESENTATION_ACTUAL,
    SOURCE_SCENE_ACTUAL,
    SOURCE_DEVICE_ACTUAL,
    SOURCE_RENDER_MEDIA_ACTUAL,
    SOURCE_CAMERA_MEDIA_ACTUAL
};

static const uint8_t INLINE_SOURCE_INDEX[
    MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT
] = {
    SOURCE_WIFI_INLINE, SOURCE_RENDER_INLINE, SOURCE_DEVICE_INLINE
};

typedef struct {
    MeisoStateReplica *replica;
    bool owner_started;
    bool accept_pending;
    bool accept_queued;
    MeisoDeliveryGeneration accept_generation;
    MeisoOwnerScope accept_owner;
    uint64_t accept_version;
    MeisoRuntimeDesiredState decoded;
    MeisoRuntimeDesiredReference reference;
} EdgeDesiredChannel;

typedef struct {
    MeisoStatePublisher *publisher;
    MeisoCompositionEdgeAppActualSnapshot snapshot;
    bool offer_queued;
    bool offer_sent;
    uint64_t production_epoch_high_water;
} EdgeActualChannel;

typedef struct {
    MeisoStream *stream;
    MeisoCompositionEdgeAppInlineSnapshot snapshot;
    bool sample_queued;
    bool sample_sent;
    uint64_t pending_sequence;
} EdgeInlineChannel;

typedef struct {
    size_t offset;
    bool consumed;
    int fd;
    uint8_t active_route_mask;
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaPosixTcp *adapter;
    MeisoCompositionMediaTransportConfig transport_config;
} EdgeMediaSlot;

typedef struct {
    uint64_t production_epoch;
    uint64_t next_due_ns;
    uint64_t remainder;
    uint32_t rate_millihz;
    bool armed;
} EdgeCadence;

typedef struct {
    size_t total;
    size_t peer_offset;
    size_t peer_bytes;
    size_t replica_offset[MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT];
    size_t replica_bytes[MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT];
    size_t publisher_offset[MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT];
    size_t publisher_bytes[MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT];
    size_t stream_offset[MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT];
    size_t stream_bytes[MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT];
    size_t blob_offset;
    size_t blob_bytes;
    size_t scene_offset;
    size_t scene_bytes;
    size_t coordinator_offset;
    size_t coordinator_bytes;
    size_t renderer_offset;
    size_t renderer_bytes;
    size_t camera_offset;
    size_t camera_bytes;
    size_t media_slots_offset;
    size_t media_transport_bytes;
    size_t media_slot_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
} EdgeAppLayout;

typedef struct {
    const void *data;
    size_t size;
} EdgeRange;

struct MeisoCompositionEdgeApp {
    uint32_t magic;
    uint8_t *arena_begin;
    size_t arena_bytes;
    MeisoCompositionEdgeAppConfig config;
    MeisoCompositionDebugPeer *peer;
    MeisoCompositionSessionV3CatalogConfig catalog;
    MeisoCompositionSessionV3RouteDescriptor catalog_routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    EdgeDesiredChannel desired[MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT];
    EdgeActualChannel actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT];
    EdgeInlineChannel inline_streams[
        MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT
    ];
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionCoordinator *coordinator;
    uint8_t *coordinator_arena;
    MeisoCompositionCpuRasterRenderer *renderer;
    MeisoCameraV4l2Adapter *camera;
    MeisoWifiLinuxAdapter wifi;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoWifiTelemetryCapability wifi_capability;
    MeisoDeviceTelemetryCapability device_capability;
    EdgeMediaSlot media_slots[MEISO_COMPOSITION_EDGE_APP_MAX_MEDIA_CONNECTIONS];
    EdgeMediaSlot *active_media;
    uint8_t media_slots_consumed;
    uint8_t fairness_cursor;
    bool deliveries_started;
    bool coordinator_initialized;
    bool desired_owner_present;
    MeisoOwnerScope desired_owner;
    bool mutable_active;
    MeisoCompositionEdgeAppPhase phase;
    MeisoCompositionEdgeAppTerminalReason terminal_reason;
    uint64_t next_thermal_request_id;
    MeisoCompositionEdgeAppThermalRequest thermal_request;
    bool thermal_request_pending;
    bool thermal_request_dispatched;
    MeisoCompositionEdgeAppThermalResult thermal_result;
    bool thermal_result_present;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_effective_present;
    MeisoWifiTelemetrySample wifi_sample;
    bool wifi_sample_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_effective_present;
    MeisoDeviceTelemetrySample device_sample;
    bool device_sample_present;
    MeisoCompositionMediaActual render_media_actual;
    bool render_media_actual_present;
    MeisoCompositionMediaActual camera_media_actual;
    bool camera_media_actual_present;
    MeisoCompositionScenePresentationStatus presentation_status;
    bool presentation_status_present;
    MeisoCompositionSceneStatus scene_status;
    bool scene_status_present;
    MeisoCompositionCpuRasterJobRef render_job;
    bool render_job_pending;
    MeisoCompositionCpuRasterCompleteView render_complete;
    bool render_complete_present;
    bool render_inline_materialized;
    bool render_inline_accepted;
    bool render_media_materialized;
    uint64_t render_media_sequence;
    MeisoCameraV4l2FrameView camera_frame;
    bool camera_frame_pending;
    bool camera_frame_submitted;
    uint64_t camera_media_sequence;
    uint64_t wifi_next_due_ns;
    uint64_t device_next_due_ns;
    EdgeCadence render_result_cadence;
    EdgeCadence render_media_cadence;
    EdgeCadence camera_media_cadence;
    MeisoCompositionSceneDesired scene_desired;
    bool scene_desired_present;
    MeisoCompositionScenePresentationPlan presentation_plan;
    bool presentation_plan_present;
    MeisoContentId visible_blob_ids[
        MEISO_COMPOSITION_EDGE_APP_MESH_BLOB_SLOTS
    ];
    uint64_t visible_blob_sizes[
        MEISO_COMPOSITION_EDGE_APP_MESH_BLOB_SLOTS
    ];
    uint8_t visible_blob_count;
    uint64_t blob_published_bytes;
    uint64_t blob_transfer_id_high_water;
    MeisoRuntimeBlobTransferRef blob_receive_transfer;
    MeisoRuntimeBlobTransferRef blob_result_transfer;
    uint64_t blob_receive_next_offset;
    MeisoRuntimeBlobOperation blob_result_operation;
    bool blob_receive_active;
    bool blob_receive_begin_ready;
    bool blob_result_pending;
    bool blob_result_queued;
    bool blob_result_marks_ready;
    bool blob_result_clears_active;
    uint8_t state_scratch[
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t actual_scratch[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t profile_scratch[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    uint8_t stream_scratch[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    size_t control_peer_bytes;
    size_t blob_store_bytes;
    size_t scene_store_bytes;
    size_t coordinator_bytes;
    size_t renderer_bytes;
    size_t camera_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t media_slot_bytes;
    size_t media_transport_bytes;
};

_Static_assert(
    MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT == 6U &&
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT == 6U &&
        MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT == 3U,
    "edge channel tables"
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

static void cadence_reset(EdgeCadence *cadence)
{
    if (cadence != NULL) {
        memset(cadence, 0, sizeof(*cadence));
    }
}

static bool cadence_ready(
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
        cadence_reset(cadence);
        cadence->production_epoch = production_epoch;
        cadence->rate_millihz = rate_millihz;
    }
    return !cadence->armed || now_ns >= cadence->next_due_ns;
}

static bool cadence_commit(EdgeCadence *cadence, uint64_t now_ns)
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
                MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
                MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE);
    peer_source(config, SOURCE_WIFI_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_WIFI_DESIRED);
    peer_source(config, SOURCE_WIFI_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_WIFI_ACTUAL);
    peer_source(config, SOURCE_WIFI_INLINE,
                MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                ROUTE_WIFI_INLINE);
    peer_source(config, SOURCE_BLOB_RESULT,
                MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT,
                ROUTE_MESH_BLOB);
    peer_source(config, SOURCE_PRESENTATION_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_PRESENTATION_DESIRED);
    peer_source(config, SOURCE_PRESENTATION_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_PRESENTATION_ACTUAL);
    peer_source(config, SOURCE_SCENE_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_SCENE_DESIRED);
    peer_source(config, SOURCE_SCENE_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_SCENE_ACTUAL);
    peer_source(config, SOURCE_RENDER_INLINE,
                MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                ROUTE_RENDER_INLINE);
    peer_source(config, SOURCE_DEVICE_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_DEVICE_DESIRED);
    peer_source(config, SOURCE_DEVICE_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_DEVICE_ACTUAL);
    peer_source(config, SOURCE_DEVICE_INLINE,
                MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                ROUTE_DEVICE_INLINE);
    peer_source(config, SOURCE_RENDER_MEDIA_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_RENDER_MEDIA_DESIRED);
    peer_source(config, SOURCE_RENDER_MEDIA_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_RENDER_MEDIA_ACTUAL);
    peer_source(config, SOURCE_CAMERA_MEDIA_DESIRED_ACCEPT,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                ROUTE_CAMERA_MEDIA_DESIRED);
    peer_source(config, SOURCE_CAMERA_MEDIA_ACTUAL,
                MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                ROUTE_CAMERA_MEDIA_ACTUAL);
    for (index = 17U; index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT;
         ++index) {
        peer_source(config, (uint8_t)index,
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

static MeisoCompositionSessionV3CatalogConfig catalog_config_for_fields(
    const MeisoCompositionEdgeAppConfig *config,
    MeisoWifiTelemetryFieldMask wifi_fields,
    MeisoDeviceTelemetryFieldMask device_fields
)
{
    MeisoCompositionSessionV3CatalogConfig value;
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

static bool peer_config_from_edge(
    const MeisoCompositionEdgeAppConfig *config,
    const MeisoCompositionSessionV3CatalogConfig *catalog,
    MeisoCompositionDebugPeerConfig *out_config
)
{
    MeisoCompositionDebugPeerConfig value;
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
    configure_peer_sources(&value);
    *out_config = value;
    return true;
}

static bool build_catalog(
    const MeisoCompositionSessionV3CatalogConfig *catalog,
    MeisoCompositionSessionV3RouteDescriptor *out_routes
)
{
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    uint8_t capability[MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES];
    return catalog != NULL && out_routes != NULL &&
        meiso_composition_session_v3_build_catalog(
            catalog,
            (MeisoMutableBuffer){capability, sizeof(capability)},
            endpoints, MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            out_routes, MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) == MEISO_COMPOSITION_SESSION_V3_OK;
}

static bool config_shape_valid(const MeisoCompositionEdgeAppConfig *config)
{
    MeisoWifiLinuxAdapter wifi;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoCompositionSessionV3CatalogConfig catalog;
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    MeisoCompositionDebugPeerConfig peer_config;
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
            MEISO_COMPOSITION_EDGE_APP_MAX_MEDIA_CONNECTIONS ||
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
    return peer_config_from_edge(config, &catalog, &peer_config) &&
        meiso_composition_debug_peer_memory_required(&peer_config) != 0U &&
        build_catalog(&catalog, routes);
}

static bool calculate_layout(
    const MeisoCompositionEdgeAppConfig *config,
    const MeisoCompositionSessionV3CatalogConfig *catalog,
    EdgeAppLayout *out_layout,
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
        MEISO_COMPOSITION_EDGE_APP_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    const MeisoBlobConfig blob_config = {
        MEISO_COMPOSITION_EDGE_APP_MESH_BLOB_SLOTS
    };
    MeisoCompositionDebugPeerConfig peer_config;
    EdgeAppLayout layout;
    size_t cursor;
    size_t slots_bytes;
    size_t index;
    uint32_t max_fragment;
    if (config == NULL || catalog == NULL || out_layout == NULL ||
        out_routes == NULL || !config_shape_valid(config) ||
        !peer_config_from_edge(config, catalog, &peer_config) ||
        !build_catalog(catalog, out_routes)) {
        return false;
    }
    memset(&layout, 0, sizeof(layout));
    if (!align_up(sizeof(MeisoCompositionEdgeApp), alignment, &cursor)) {
        return false;
    }
    layout.peer_bytes =
        meiso_composition_debug_peer_memory_required(&peer_config);
    if (!add_aligned_region(
            &cursor, layout.peer_bytes, alignment, &layout.peer_offset
        )) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT;
         ++index) {
        layout.replica_bytes[index] = meiso_state_replica_memory_required(
            &out_routes[DESIRED_ROUTE_INDEX[index]].route.resource
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
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT;
         ++index) {
        layout.publisher_bytes[index] =
            meiso_state_publisher_memory_required(
                &out_routes[ACTUAL_ROUTE_INDEX[index]].route.resource,
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
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT;
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

size_t meiso_composition_edge_app_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_composition_edge_app_memory_required(
    const MeisoCompositionEdgeAppConfig *config
)
{
    MeisoCompositionSessionV3CatalogConfig catalog;
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    EdgeAppLayout layout;
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
    const MeisoCompositionEdgeAppConfig *config,
    MeisoCompositionEdgeApp **out_app
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
    if (arena == NULL || config == NULL || out_app == NULL) {
        return false;
    }
    ranges[count++] = (EdgeRange){arena, arena_size};
    ranges[count++] = (EdgeRange){config, sizeof(*config)};
    ranges[count++] = (EdgeRange){out_app, sizeof(*out_app)};
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
            if (ranges_overlap(
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

static MeisoOwnerScope edge_owner(
    const MeisoCompositionEdgeAppConfig *config
)
{
    return (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE, config->local_incarnation
    };
}

MeisoCompositionEdgeAppInitStatus meiso_composition_edge_app_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionEdgeAppConfig *config,
    MeisoCompositionEdgeApp **out_app
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_COMPOSITION_EDGE_APP_INLINE_QUEUE_SLOTS,
        0U,
        0U
    };
    const MeisoBlobConfig blob_config = {
        MEISO_COMPOSITION_EDGE_APP_MESH_BLOB_SLOTS
    };
    MeisoWifiLinuxAdapter wifi;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoWifiTelemetryFieldMask wifi_fields;
    MeisoDeviceTelemetryCapability device_capability;
    MeisoCompositionSessionV3CatalogConfig catalog;
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    MeisoCompositionDebugPeerConfig peer_config;
    EdgeAppLayout layout;
    MeisoCompositionEdgeApp *app;
    MeisoCompositionDebugPeer *peer;
    size_t index;

    if (!config_ranges_disjoint(arena, arena_size, config, out_app)) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_INVALID_ARGUMENT;
    }
    if (!config_shape_valid(config)) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_INVALID_CONFIG;
    }
    if (((uintptr_t)arena % alignof(max_align_t)) != 0U) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_MISALIGNED_ARENA;
    }
    if (!fd_preflight(config->control_connected_fd) ||
        !fd_prepare_send_safety(config->control_connected_fd)) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_FD_ERROR;
    }
    memset(&wifi, 0, sizeof(wifi));
    if (meiso_wifi_linux_adapter_init(&wifi, &config->wifi) !=
            MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_wifi_linux_adapter_probe(&wifi, &wifi_fields) !=
            MEISO_WIFI_LINUX_ADAPTER_OK) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_PROBE_UNAVAILABLE;
    }
    memset(&device, 0, sizeof(device));
    if (meiso_device_telemetry_linux_adapter_init(
            &device, &config->device_telemetry
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK ||
        meiso_device_telemetry_linux_adapter_probe(
            &device, &device_capability
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_PROBE_UNAVAILABLE;
    }
    catalog = catalog_config_for_fields(
        config, wifi_fields, device_capability.supported_fields
    );
    catalog.device_telemetry_capability = device_capability;
    if (!calculate_layout(config, &catalog, &layout, routes) ||
        arena_size < layout.total ||
        !peer_config_from_edge(config, &catalog, &peer_config)) {
        return arena_size < meiso_composition_edge_app_memory_required(config) ?
            MEISO_COMPOSITION_EDGE_APP_INIT_ARENA_TOO_SMALL :
            MEISO_COMPOSITION_EDGE_APP_INIT_CAPACITY;
    }
    if (meiso_composition_debug_peer_init(
            (uint8_t *)arena + layout.peer_offset, layout.peer_bytes,
            &peer_config, &peer
        ) != MEISO_COMPOSITION_DEBUG_PEER_INIT_OK) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_INTERNAL_FAILURE;
    }

    memset(arena, 0, sizeof(MeisoCompositionEdgeApp));
    app = (MeisoCompositionEdgeApp *)arena;
    app->arena_begin = (uint8_t *)arena;
    app->arena_bytes = layout.total;
    app->config = *config;
    app->config.wifi.sys_class_net_root = NULL;
    app->config.wifi.proc_net_wireless_path = NULL;
    app->config.device_telemetry.proc_stat_path = NULL;
    app->config.device_telemetry.proc_meminfo_path = NULL;
    app->config.device_telemetry.thermal_type_path = NULL;
    app->config.device_telemetry.thermal_value_path = NULL;
    app->config.camera.device_path = NULL;
    app->config.camera.syscalls = NULL;
    app->peer = peer;
    app->catalog = catalog;
    app->wifi = wifi;
    app->device = device;
    app->wifi_capability = catalog.wifi_capability;
    app->device_capability = catalog.device_telemetry_capability;
    memcpy(app->catalog_routes, routes, sizeof(routes));

    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT;
         ++index) {
        if (meiso_state_replica_init(
                app->arena_begin + layout.replica_offset[index],
                layout.replica_bytes[index],
                &routes[DESIRED_ROUTE_INDEX[index]].route.resource,
                &app->desired[index].replica
            ) != MEISO_STATE_INIT_OK) {
            return MEISO_COMPOSITION_EDGE_APP_INIT_INTERNAL_FAILURE;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT;
         ++index) {
        if (meiso_state_publisher_init(
                app->arena_begin + layout.publisher_offset[index],
                layout.publisher_bytes[index],
                &routes[ACTUAL_ROUTE_INDEX[index]].route.resource,
                &state_config, &app->actual[index].publisher
            ) != MEISO_STATE_INIT_OK ||
            meiso_state_publisher_begin_delivery(
                app->actual[index].publisher,
                config->initial_actual_delivery_generation
            ) != MEISO_STATE_DELIVERY_STARTED) {
            return MEISO_COMPOSITION_EDGE_APP_INIT_INTERNAL_FAILURE;
        }
        app->actual[index].snapshot.delivery_generation =
            config->initial_actual_delivery_generation;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT; ++index) {
        if (meiso_stream_init(
                app->arena_begin + layout.stream_offset[index],
                layout.stream_bytes[index],
                &routes[INLINE_ROUTE_INDEX[index]].route.resource,
                &stream_config, &app->inline_streams[index].stream
            ) != MEISO_STREAM_INIT_OK) {
            return MEISO_COMPOSITION_EDGE_APP_INIT_INTERNAL_FAILURE;
        }
    }
    if (meiso_blob_store_init(
            app->arena_begin + layout.blob_offset, layout.blob_bytes,
            &routes[ROUTE_MESH_BLOB].route.resource, &blob_config,
            &app->blob_store
        ) != MEISO_BLOB_INIT_OK ||
        meiso_composition_scene_store_init(
            app->arena_begin + layout.scene_offset, layout.scene_bytes,
            app->blob_store, &app->scene_store
        ) != MEISO_COMPOSITION_SCENE_STORE_INIT_OK ||
        meiso_composition_cpu_raster_renderer_init(
            app->arena_begin + layout.renderer_offset,
            layout.renderer_bytes, app->blob_store, &app->renderer
        ) != MEISO_COMPOSITION_CPU_RASTER_INIT_OK ||
        meiso_camera_v4l2_init(
            app->arena_begin + layout.camera_offset, layout.camera_bytes,
            &config->camera, &app->camera
        ) != MEISO_CAMERA_V4L2_INIT_OK) {
        return MEISO_COMPOSITION_EDGE_APP_INIT_INTERNAL_FAILURE;
    }
    memset(app->arena_begin + layout.coordinator_offset, 0,
           layout.coordinator_bytes);
    memset(app->arena_begin + layout.media_slots_offset, 0,
           layout.media_slot_bytes * config->media_connection_slots);
    for (index = 0U; index < config->media_connection_slots; ++index) {
        app->media_slots[index].offset =
            layout.media_slots_offset + index * layout.media_slot_bytes;
        app->media_slots[index].fd = -1;
    }
    app->phase = MEISO_COMPOSITION_EDGE_APP_PHASE_HANDSHAKE;
    app->next_thermal_request_id = 1U;
    app->control_peer_bytes = layout.peer_bytes;
    app->blob_store_bytes = layout.blob_bytes;
    app->scene_store_bytes = layout.scene_bytes;
    app->coordinator_bytes = layout.coordinator_bytes;
    app->renderer_bytes = layout.renderer_bytes;
    app->camera_bytes = layout.camera_bytes;
    app->core_state_bytes = layout.core_state_bytes;
    app->core_stream_bytes = layout.core_stream_bytes;
    app->media_slot_bytes = layout.media_slot_bytes;
    app->media_transport_bytes = layout.media_transport_bytes;
    app->coordinator_arena =
        app->arena_begin + layout.coordinator_offset;
    app->magic = EDGE_APP_MAGIC;
    (void)edge_owner(config);
    *out_app = app;
    return MEISO_COMPOSITION_EDGE_APP_INIT_OK;
}

static bool app_valid(const MeisoCompositionEdgeApp *app)
{
    return app != NULL && app->magic == EDGE_APP_MAGIC &&
        app->arena_begin == (const uint8_t *)app &&
        app->arena_bytes >= sizeof(*app) && app->peer != NULL &&
        app->blob_store != NULL && app->scene_store != NULL &&
        app->renderer != NULL && app->camera != NULL;
}

static bool app_range_overlap(
    const MeisoCompositionEdgeApp *app,
    const void *value,
    size_t size
)
{
    return app_valid(app) && ranges_overlap(
        app->arena_begin, app->arena_bytes, value, size
    );
}

static bool owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

static bool route_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static bool route_channel(
    const MeisoCompositionEdgeApp *app,
    MeisoRuntimeRouteKey route,
    const uint8_t *route_indices,
    size_t count,
    size_t *out_index
)
{
    size_t index;
    MeisoRuntimeRouteKey expected;
    if (!app_valid(app) || route_indices == NULL || out_index == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (meiso_composition_debug_peer_route_key(
                app->peer, route_indices[index], &expected
            ) && route_equal(route, expected)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

static bool ensure_coordinator(MeisoCompositionEdgeApp *app)
{
    MeisoCompositionDebugPeerSnapshot peer;
    MeisoCompositionCoordinatorConfig config;
    if (app->coordinator_initialized) {
        return true;
    }
    if (!meiso_composition_debug_peer_snapshot(app->peer, &peer) ||
        peer.phase != MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY ||
        !peer.session.session_id_known) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.scene_store = app->scene_store;
    config.session_id = peer.session.session_id;
    config.edge_incarnation = app->config.local_incarnation;
    config.capability_generation = app->config.catalog_generation;
    config.edge_clock_epoch = app->config.edge_clock_epoch;
    if (meiso_composition_coordinator_init(
            app->coordinator_arena, app->coordinator_bytes, &config,
            &app->coordinator
        ) != MEISO_COMPOSITION_COORDINATOR_INIT_OK) {
        return false;
    }
    app->coordinator_initialized = true;
    return true;
}

static bool ensure_desired_owner(
    MeisoCompositionEdgeApp *app,
    MeisoOwnerScope owner
)
{
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionCoordinatorOwnerStatus coordinator_status;
    MeisoCompositionCpuRasterScopeStatus renderer_status;
    if (owner.role != MEISO_OWNER_ROLE_HOST || owner.incarnation == 0U ||
        !ensure_coordinator(app)) {
        return false;
    }
    if (app->desired_owner_present) {
        return owner_equal(app->desired_owner, owner);
    }
    coordinator_status = meiso_composition_coordinator_begin_owner(
        app->coordinator, owner.incarnation
    );
    if (coordinator_status != MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED ||
        !meiso_composition_scene_store_control_scope(
            app->scene_store, &scope
        )) {
        return false;
    }
    renderer_status = meiso_composition_cpu_raster_renderer_begin_control_scope(
        app->renderer, &scope
    );
    if (renderer_status != MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED &&
        renderer_status != MEISO_COMPOSITION_CPU_RASTER_SCOPE_UNCHANGED) {
        return false;
    }
    app->desired_owner = owner;
    app->desired_owner_present = true;
    return true;
}

static bool encode_actual_profile(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoByteView *out_profile
)
{
    size_t size = 0U;
    MeisoCompositionCpuRasterSnapshot renderer;
    MeisoCompositionSceneResourceUsage usage;
    if (app == NULL || out_profile == NULL) {
        return false;
    }
    out_profile->data = NULL;
    out_profile->size = 0U;
    if (!app->actual[index].snapshot.effective_enabled &&
        index != MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION &&
        index != MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE) {
        return true;
    }
    switch (index) {
    case MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI:
        if (!app->wifi_effective_present ||
            meiso_wifi_telemetry_effective_parameters_encode(
                &app->wifi_effective,
                (MeisoMutableBuffer){
                    app->profile_scratch, sizeof(app->profile_scratch)
                }, &size
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
            return false;
        }
        break;
    case MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION:
        if (!app->coordinator_initialized ||
            !meiso_composition_coordinator_status(
                app->coordinator, &app->presentation_status
            ) || meiso_composition_scene_presentation_status_encode(
                &app->presentation_status,
                (MeisoMutableBuffer){
                    app->profile_scratch, sizeof(app->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return false;
        }
        app->presentation_status_present = true;
        break;
    case MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE:
        memset(&usage, 0, sizeof(usage));
        if (!meiso_composition_cpu_raster_renderer_snapshot(
                app->renderer, &renderer
            )) {
            return false;
        }
        usage.published_blob_count = (uint32_t)meiso_blob_store_count(
            app->blob_store
        );
        usage.published_blob_bytes = app->blob_published_bytes;
        usage.target_slots_used = renderer.target_slots_allocated;
        usage.target_bytes = renderer.target_bytes;
        usage.render_jobs_in_flight = renderer.queued_jobs;
        usage.result_queue_depth = (uint32_t)meiso_stream_count(
            app->inline_streams[
                MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
            ].stream
        );
        if (!meiso_composition_scene_store_status(
                app->scene_store, &usage, &app->scene_status
            ) || meiso_composition_scene_status_encode(
                &app->scene_status,
                (MeisoMutableBuffer){
                    app->profile_scratch, sizeof(app->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return false;
        }
        app->scene_status_present = true;
        break;
    case MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE:
        if (!app->device_effective_present ||
            meiso_device_telemetry_parameters_encode(
                &app->device_effective,
                (MeisoMutableBuffer){
                    app->profile_scratch, sizeof(app->profile_scratch)
                }, &size
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
            return false;
        }
        break;
    case MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA:
        if (!app->render_media_actual_present ||
            meiso_composition_media_actual_encode(
                &app->render_media_actual,
                (MeisoMutableBuffer){
                    app->profile_scratch, sizeof(app->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SESSION_V3_OK) {
            return false;
        }
        break;
    case MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA:
        if (!app->camera_media_actual_present ||
            meiso_composition_media_actual_encode(
                &app->camera_media_actual,
                (MeisoMutableBuffer){
                    app->profile_scratch, sizeof(app->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SESSION_V3_OK) {
            return false;
        }
        break;
    default:
        return false;
    }
    out_profile->data = app->profile_scratch;
    out_profile->size = size;
    return true;
}

static bool publish_actual(MeisoCompositionEdgeApp *app, size_t index)
{
    EdgeActualChannel *channel;
    MeisoRuntimeActualState actual;
    MeisoByteView profile;
    MeisoByteView payload;
    MeisoStatePublishStatus status;
    uint64_t version;
    if (!app_valid(app) || index >= MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT) {
        return false;
    }
    channel = &app->actual[index];
    if (channel->snapshot.actual_state_version == UINT64_MAX ||
        !encode_actual_profile(app, index, &profile)) {
        return false;
    }
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = channel->snapshot.observed_desired;
    actual.applied_desired = channel->snapshot.applied_desired;
    actual.apply_result = channel->snapshot.apply_result;
    actual.availability = channel->snapshot.availability;
    actual.effective_enabled = channel->snapshot.effective_enabled;
    actual.production_status = channel->snapshot.production_status;
    actual.reason = channel->snapshot.reason;
    actual.production_epoch = channel->snapshot.production_epoch;
    actual.profile = profile;
    if (!meiso_runtime_actual_state_encode(
            &actual,
            (MeisoMutableBuffer){
                app->actual_scratch, sizeof(app->actual_scratch)
            }, &payload
        )) {
        return false;
    }
    version = channel->snapshot.actual_state_version + 1U;
    status = meiso_state_publisher_publish(
        channel->publisher, edge_owner(&app->config), version, payload
    );
    if (status != MEISO_STATE_PUBLISH_NEW &&
        status != MEISO_STATE_PUBLISH_REPLACED_PENDING &&
        status != MEISO_STATE_PUBLISH_DUPLICATE) {
        return false;
    }
    channel->snapshot.present = true;
    channel->snapshot.actual_state_version = version;
    channel->snapshot.delivery_pending = status != MEISO_STATE_PUBLISH_DUPLICATE;
    channel->snapshot.delivery_accepted = false;
    channel->snapshot.delivery_failed = false;
    channel->offer_sent = false;
    return true;
}

static void set_actual_rejected(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeProductionReason reason
)
{
    EdgeActualChannel *channel = &app->actual[index];
    channel->snapshot.observed_desired = observed;
    channel->snapshot.apply_result = MEISO_RUNTIME_APPLY_REJECTED;
    channel->snapshot.reason = reason;
    if (!channel->snapshot.present ||
        !channel->snapshot.applied_desired.present) {
        memset(&channel->snapshot.applied_desired, 0,
               sizeof(channel->snapshot.applied_desired));
        channel->snapshot.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        channel->snapshot.effective_enabled = false;
        channel->snapshot.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
        channel->snapshot.production_epoch = 0U;
    }
}

static bool set_actual_applied(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoRuntimeDesiredReference desired,
    bool enabled,
    bool running,
    bool start_new_epoch
)
{
    EdgeActualChannel *channel = &app->actual[index];
    channel->snapshot.observed_desired = desired;
    channel->snapshot.applied_desired = desired;
    channel->snapshot.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    channel->snapshot.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    channel->snapshot.effective_enabled = enabled && running;
    channel->snapshot.production_status = enabled ?
        (running ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                   MEISO_RUNTIME_PRODUCTION_STARTING) :
        MEISO_RUNTIME_PRODUCTION_STOPPED;
    channel->snapshot.reason = enabled && !running ?
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING :
        (enabled ? MEISO_RUNTIME_PRODUCTION_REASON_NONE :
                   MEISO_RUNTIME_PRODUCTION_REASON_DISABLED);
    if (enabled && running) {
        if (!start_new_epoch &&
            channel->snapshot.production_epoch != 0U) {
            /* Same producer/configuration; only desired identity advanced. */
        } else {
            if (channel->production_epoch_high_water == UINT64_MAX) {
                return false;
            }
            ++channel->production_epoch_high_water;
            channel->snapshot.production_epoch =
                channel->production_epoch_high_water;
        }
    } else {
        channel->snapshot.production_epoch = 0U;
    }
    return true;
}

static bool camera_resources_clean(
    const MeisoCompositionEdgeApp *app,
    bool require_stopped
)
{
    MeisoCameraV4l2Snapshot snapshot;
    return app != NULL &&
        meiso_camera_v4l2_snapshot(app->camera, &snapshot) &&
        (!require_stopped ||
         snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) &&
        !snapshot.stream_on && !snapshot.frame_borrowed &&
        snapshot.driver_buffer_count == 0U &&
        snapshot.mapped_buffer_count == 0U &&
        snapshot.queued_buffer_count == 0U && snapshot.mapped_bytes == 0U;
}

/* The adapter retains cleanup debt after a failed stop so one retry is legal. */
static bool stop_camera_bounded(
    MeisoCompositionEdgeApp *app,
    bool require_stopped
)
{
    unsigned attempt;
    if (camera_resources_clean(app, require_stopped)) {
        return true;
    }
    for (attempt = 0U; attempt < 2U; ++attempt) {
        (void)meiso_camera_v4l2_stop(app->camera);
        if (camera_resources_clean(app, require_stopped)) {
            return true;
        }
    }
    return false;
}

static bool fill_media_actual(
    MeisoCompositionEdgeApp *app,
    size_t index,
    const MeisoCompositionMediaDesired *effective
)
{
    const bool render =
        index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA;
    MeisoCompositionMediaActual *actual = render ?
        &app->render_media_actual : &app->camera_media_actual;
    memset(actual, 0, sizeof(*actual));
    actual->source = render ?
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    actual->effective = *effective;
    actual->media_stream_epoch = app->actual[index].snapshot.production_epoch;
    actual->channel_nonce = (MeisoSessionNonce){
        app->config.local_nonce.high ^ app->actual[index].snapshot.production_epoch,
        app->config.local_nonce.low ^ (render ? UINT64_C(0x52554e44) :
                                               UINT64_C(0x43414d45))
    };
    if (!meiso_session_nonce_valid(actual->channel_nonce)) {
        actual->channel_nonce.low = 1U;
    }
    actual->queue_depth = 1U;
    if (render) {
        app->render_media_actual_present = true;
    } else {
        app->camera_media_actual_present = true;
    }
    return true;
}

static bool media_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->pixel_format == rhs->pixel_format && lhs->codec == rhs->codec &&
        lhs->width == rhs->width && lhs->height == rhs->height &&
        lhs->fps_millihz == rhs->fps_millihz &&
        lhs->bitrate_bps == rhs->bitrate_bps &&
        lhs->gop_frames == rhs->gop_frames &&
        lhs->recovery_flags == rhs->recovery_flags;
}

static bool start_inline_producer(
    MeisoCompositionEdgeApp *app,
    size_t inline_index,
    uint64_t production_epoch
)
{
    EdgeInlineChannel *channel;
    MeisoStreamProducerStatus status;
    if (inline_index >= MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT ||
        production_epoch == 0U) {
        return false;
    }
    channel = &app->inline_streams[inline_index];
    status = meiso_stream_begin_producer(
        channel->stream, edge_owner(&app->config), production_epoch
    );
    if (status != MEISO_STREAM_PRODUCER_STARTED &&
        status != MEISO_STREAM_PRODUCER_SWITCHED &&
        status != MEISO_STREAM_PRODUCER_UNCHANGED) {
        return false;
    }
    channel->snapshot.enabled = true;
    channel->snapshot.stream_epoch = production_epoch;
    if (status == MEISO_STREAM_PRODUCER_STARTED ||
        status == MEISO_STREAM_PRODUCER_SWITCHED) {
        channel->snapshot.next_sequence = 1U;
        channel->snapshot.queue_depth = 0U;
        channel->sample_queued = false;
        channel->sample_sent = false;
        channel->pending_sequence = 0U;
    }
    return true;
}

static bool candidate_nonce(
    const MeisoCompositionCoordinatorPlanRef *ref,
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE]
)
{
    size_t index;
    if (ref == NULL || nonce == NULL || ref->plan_generation == 0U) {
        return false;
    }
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        nonce[index] = (uint8_t)(
            ref->candidate_digest[index] ^
            (uint8_t)(ref->plan_generation >> ((index % 8U) * 8U)) ^
            (uint8_t)(index + 1U)
        );
    }
    nonce[0] |= 1U;
    return true;
}

static bool world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
        lhs->desired_state_version == rhs->desired_state_version &&
        lhs->plan_generation == rhs->plan_generation &&
        lhs->world_generation == rhs->world_generation;
}

static void terminate_active_media(
    MeisoCompositionEdgeApp *app,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
);

static bool disable_inline_producer(EdgeInlineChannel *channel)
{
    if (channel == NULL || channel->sample_queued || channel->sample_sent) {
        return false;
    }
    while (meiso_stream_count(channel->stream) != 0U) {
        if (!meiso_stream_pop(channel->stream)) {
            return false;
        }
    }
    channel->snapshot.enabled = false;
    channel->snapshot.stream_epoch = 0U;
    channel->snapshot.next_sequence = 0U;
    channel->snapshot.queue_depth = 0U;
    channel->pending_sequence = 0U;
    return true;
}

static bool clear_render_pipeline(
    MeisoCompositionEdgeApp *app,
    bool disable_inline
)
{
    EdgeInlineChannel *channel;
    if (app == NULL) {
        return false;
    }
    channel = &app->inline_streams[
        MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
    ];
    if (disable_inline) {
        if (!disable_inline_producer(channel)) {
            return false;
        }
    } else {
        if (channel->sample_queued || channel->sample_sent) {
            return false;
        }
        while (meiso_stream_count(channel->stream) != 0U) {
            if (!meiso_stream_pop(channel->stream)) {
                return false;
            }
        }
        channel->snapshot.queue_depth = 0U;
        channel->pending_sequence = 0U;
    }
    app->render_job_pending = false;
    memset(&app->render_job, 0, sizeof(app->render_job));
    app->render_complete_present = false;
    memset(&app->render_complete, 0, sizeof(app->render_complete));
    app->render_inline_materialized = false;
    app->render_inline_accepted = false;
    app->render_media_materialized = false;
    cadence_reset(&app->render_result_cadence);
    return true;
}

static bool retire_world(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionSceneWorldRef *world
)
{
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionCpuRasterRetireStatus status;
    const EdgeInlineChannel *channel;
    if (app == NULL || world == NULL ||
        !meiso_composition_scene_store_control_scope(
            app->scene_store, &scope
        )) {
        return false;
    }
    channel = &app->inline_streams[
        MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
    ];
    if (channel->sample_queued || channel->sample_sent) {
        return false;
    }
    status = meiso_composition_cpu_raster_renderer_retire_world(
        app->renderer, &scope, world
    );
    return (status == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED ||
            status == MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED) &&
        clear_render_pipeline(app, true);
}

static bool prepare_stored_scene(
    MeisoCompositionEdgeApp *app,
    bool *out_attempted,
    bool *out_active_plan,
    MeisoCompositionScenePrepareStatus *out_status
)
{
    MeisoCompositionCoordinatorPlanView plan;
    MeisoCompositionScenePrepareStatus status;
    const EdgeDesiredChannel *desired;
    bool active_plan = false;
    if (app == NULL || out_attempted == NULL || out_active_plan == NULL ||
        out_status == NULL) {
        return false;
    }
    *out_attempted = false;
    *out_active_plan = false;
    *out_status = MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB;
    desired = &app->desired[MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE];
    if (!app->scene_desired_present || !desired->reference.present ||
        !desired->decoded.enabled) {
        return true;
    }
    if (!meiso_composition_coordinator_candidate(app->coordinator, &plan)) {
        if (!meiso_composition_coordinator_active(app->coordinator, &plan)) {
            return true;
        }
        active_plan = true;
    }
    status = meiso_composition_scene_store_prepare(
        app->scene_store, desired->reference.owner.incarnation,
        desired->reference.version, plan.ref.plan_generation,
        &app->scene_desired
    );
    if (status != MEISO_COMPOSITION_SCENE_PREPARE_PREPARED &&
        status != MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED &&
        status != MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE &&
        status != MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
        return false;
    }
    *out_attempted = true;
    *out_active_plan = active_plan;
    *out_status = status;
    return true;
}

static bool try_prepare_presentation(
    MeisoCompositionEdgeApp *app,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionSceneWorldView scene;
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    if (!app->coordinator_initialized ||
        !meiso_composition_coordinator_candidate(
            app->coordinator, &candidate
        ) || candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING) {
        return true;
    }
    if (!meiso_composition_scene_store_candidate(app->scene_store, &scene) ||
        scene.ref.plan_generation != candidate.ref.plan_generation) {
        return meiso_composition_coordinator_report_prepare_outcome(
            app->coordinator, &candidate.ref,
            MEISO_COMPOSITION_COORDINATOR_BACKEND_READY, now,
            (const uint8_t[MEISO_SHA256_DIGEST_SIZE]){1U}
        ) == MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE;
    }
    if (!candidate_nonce(&candidate.ref, nonce)) {
        return false;
    }
    return meiso_composition_coordinator_report_prepare_outcome(
        app->coordinator, &candidate.ref,
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY, now, nonce
    ) == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED;
}

static bool apply_presentation(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorObserveStatus observe;
    MeisoCompositionCoordinatorActivateStatus activate;
    MeisoCompositionCoordinatorDisableStatus disable;
    MeisoCompositionSceneWorldRef active;
    MeisoCompositionSceneWorldView old_world;
    MeisoCompositionCoordinatorPlanView plan;
    bool scene_attempted;
    bool active_plan;
    MeisoCompositionScenePrepareStatus scene_prepare;
    bool old_world_present;
    EdgeActualChannel *actual = &app->actual[
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION
    ];
    if (desired->enabled) {
        if (meiso_composition_scene_presentation_plan_decode(
                desired->profile, &app->presentation_plan
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            set_actual_rejected(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION,
                reference,
                MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
            );
            return publish_actual(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION
            );
        }
        app->presentation_plan_present = true;
        observe = meiso_composition_coordinator_observe_desired(
            app->coordinator, reference.owner.incarnation, reference.version,
            desired, now
        );
        if (observe == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING) {
            if (!prepare_stored_scene(
                    app, &scene_attempted, &active_plan, &scene_prepare
                ) || !try_prepare_presentation(app, now)) {
                return false;
            }
        } else if (observe ==
                   MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING) {
            if (!meiso_composition_coordinator_candidate(
                    app->coordinator, &plan
                ) || !plan.prepared_token_present) {
                return false;
            }
            old_world_present = meiso_composition_scene_store_active(
                app->scene_store, &old_world
            );
            activate = meiso_composition_coordinator_activate_frame_boundary(
                app->coordinator, plan.prepared_token, now, &active
            );
            if (activate != MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED) {
                return false;
            }
            if (old_world_present &&
                !world_ref_equal(&old_world.ref, &active) &&
                !retire_world(app, &old_world.ref)) {
                return false;
            }
            if (app->desired[
                    MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE
                ].reference.present) {
                if (!set_actual_applied(
                        app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE,
                        app->desired[
                            MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE
                        ].reference,
                        true, true, true
                    ) || !start_inline_producer(
                        app,
                        MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT,
                        app->actual[
                            MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
                        ].snapshot.production_epoch
                    ) || !publish_actual(
                        app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
                    )) {
                    return false;
                }
            }
        } else if (observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE &&
                   observe !=
                       MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE) {
            set_actual_rejected(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION,
                reference,
                MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
            );
            return publish_actual(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION
            );
        }
        {
            const bool running =
                meiso_composition_coordinator_active(
                    app->coordinator, &plan
                ) && plan.ref.desired_owner_incarnation ==
                    reference.owner.incarnation &&
                plan.ref.desired_state_version == reference.version;
            if (!set_actual_applied(
                    app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION,
                    reference, true, running, running
                )) {
                return false;
            }
        }
    } else {
        if (desired->profile.size != 0U ||
            meiso_composition_coordinator_observe_desired(
                app->coordinator, reference.owner.incarnation,
                reference.version, desired, now
            ) != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING) {
            return false;
        }
        old_world_present = meiso_composition_scene_store_active(
            app->scene_store, &old_world
        );
        disable = meiso_composition_coordinator_disable_frame_boundary(
            app->coordinator, true, now
        );
        if (disable != MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED &&
            disable != MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED) {
            return false;
        }
        if ((old_world_present && !retire_world(app, &old_world.ref)) ||
            (!old_world_present && !clear_render_pipeline(app, true))) {
            return false;
        }
        app->presentation_plan_present = false;
        cadence_reset(&app->render_media_cadence);
        if (!set_actual_applied(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION,
                reference, false, false, false
            )) {
            return false;
        }
        if (app->desired[
                MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE
            ].reference.present && app->desired[
                MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE
            ].decoded.enabled && app->scene_desired_present) {
            if (!set_actual_applied(
                    app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE,
                    app->desired[
                        MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE
                    ].reference,
                    true, false, false
                ) || !publish_actual(
                    app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
                )) {
                return false;
            }
        }
    }
    actual->snapshot.applied_desired = reference;
    return publish_actual(
        app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION
    );
}

static bool refresh_scene_actual(MeisoCompositionEdgeApp *app)
{
    MeisoCompositionSceneWorldView world;
    EdgeDesiredChannel *scene;
    bool running;
    scene = &app->desired[MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE];
    if (scene->reference.present && scene->decoded.enabled &&
        app->scene_desired_present) {
        running = meiso_composition_scene_store_active(
            app->scene_store, &world
        ) && world.ref.desired_owner_incarnation ==
                scene->reference.owner.incarnation &&
            world.ref.desired_state_version == scene->reference.version;
        if (!set_actual_applied(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE,
                scene->reference, true, running, false
            ) || !publish_actual(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
            )) {
            return false;
        }
    }
    return true;
}

static bool refresh_presentation_actual(MeisoCompositionEdgeApp *app)
{
    MeisoCompositionCoordinatorPlanView plan;
    EdgeDesiredChannel *presentation;
    bool running;
    presentation = &app->desired[
        MEISO_COMPOSITION_EDGE_APP_DESIRED_PRESENTATION
    ];
    if (presentation->reference.present && presentation->decoded.enabled &&
        app->presentation_plan_present) {
        running = meiso_composition_coordinator_active(
            app->coordinator, &plan
        ) && plan.ref.desired_owner_incarnation ==
                presentation->reference.owner.incarnation &&
            plan.ref.desired_state_version == presentation->reference.version;
        if (!set_actual_applied(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION,
                presentation->reference, true, running, false
            ) || !publish_actual(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION
            )) {
            return false;
        }
    }
    return true;
}

static bool refresh_composition_actuals(MeisoCompositionEdgeApp *app)
{
    return refresh_scene_actual(app) && refresh_presentation_actual(app);
}

static bool apply_scene(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference,
    MeisoTimePoint now
)
{
    MeisoCompositionScenePrepareStatus prepare;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldView old_world;
    MeisoCompositionSceneWorldRef activated;
    MeisoCompositionCoordinatorSwapStatus swap;
    bool prepare_attempted;
    bool active_plan;
    bool old_world_present;
    bool running;
    if (!desired->enabled) {
        old_world_present = meiso_composition_scene_store_active(
            app->scene_store, &old_world
        );
        if (desired->profile.size != 0U) {
            set_actual_rejected(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE, reference,
                MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
            );
            return publish_actual(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
            );
        }
        if (old_world_present) {
            set_actual_rejected(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE, reference,
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
            );
            return publish_actual(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
            );
        }
        if (!set_actual_applied(
                app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE,
                reference, false, false, false
            )) {
            return false;
        }
        app->scene_desired_present = false;
        return publish_actual(app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE);
    }
    if (meiso_composition_scene_desired_decode(
            desired->profile, &app->scene_desired
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        set_actual_rejected(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE, reference,
            MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
        );
        return publish_actual(app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE);
    }
    app->scene_desired_present = true;
    if (!prepare_stored_scene(
            app, &prepare_attempted, &active_plan, &prepare
        )) {
        set_actual_rejected(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE, reference,
            MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
        );
        return publish_actual(app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE);
    }
    if (prepare_attempted && active_plan &&
        (prepare == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED ||
         prepare == MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED)) {
        old_world_present = meiso_composition_scene_store_active(
            app->scene_store, &old_world
        );
        swap = meiso_composition_coordinator_swap_scene_frame_boundary(
            app->coordinator, reference.owner.incarnation, reference.version,
            now, &activated
        );
        if (swap != MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED ||
            (old_world_present &&
             !world_ref_equal(&old_world.ref, &activated) &&
             !retire_world(app, &old_world.ref))) {
            return false;
        }
    }
    if (!try_prepare_presentation(app, now)) {
        return false;
    }
    running = meiso_composition_scene_store_active(app->scene_store, &active) &&
        active.ref.desired_owner_incarnation == reference.owner.incarnation &&
        active.ref.desired_state_version == reference.version;
    if (!set_actual_applied(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE,
            reference, true, running, running
        )) {
        return false;
    }
    if (running && !start_inline_producer(
            app, MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT,
            app->actual[
                MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
            ].snapshot.production_epoch
        )) {
        return false;
    }
    return publish_actual(app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE) &&
        refresh_presentation_actual(app);
}

static bool apply_simple_desired(
    MeisoCompositionEdgeApp *app,
    size_t index,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference
)
{
    MeisoWifiTelemetryDesiredParameters wifi;
    MeisoDeviceTelemetryParameters device;
    MeisoCompositionMediaDesired media;
    const MeisoCompositionMediaCapability *media_capability;
    bool restart_production = true;
    if (!desired->enabled) {
        if (desired->profile.size != 0U) {
            return false;
        }
        if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA &&
            !stop_camera_bounded(app, true)) {
            return false;
        }
        if (app->active_media != NULL &&
            ((index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA &&
              (app->active_media->active_route_mask &
               MEISO_COMPOSITION_EDGE_APP_MEDIA_RENDER) != 0U) ||
             (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA &&
              (app->active_media->active_route_mask &
               MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA) != 0U))) {
            terminate_active_media(
                app, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
            );
        }
        if (!set_actual_applied(
                app, index, reference, false, false, false
            )) {
            return false;
        }
        if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI) {
            app->wifi_effective_present = false;
            app->wifi_next_due_ns = 0U;
            if (!disable_inline_producer(
                    &app->inline_streams[
                        MEISO_COMPOSITION_EDGE_APP_INLINE_WIFI
                    ]
                )) {
                return false;
            }
        } else if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE) {
            app->device_effective_present = false;
            app->device_next_due_ns = 0U;
            app->thermal_request_pending = false;
            app->thermal_request_dispatched = false;
            app->thermal_result_present = false;
            memset(&app->thermal_request, 0, sizeof(app->thermal_request));
            memset(&app->thermal_result, 0, sizeof(app->thermal_result));
            if (!disable_inline_producer(
                    &app->inline_streams[
                        MEISO_COMPOSITION_EDGE_APP_INLINE_DEVICE
                    ]
                )) {
                return false;
            }
            (void)meiso_device_telemetry_linux_adapter_reset_sampling(
                &app->device
            );
        } else if (index ==
                   MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA) {
            app->render_media_actual_present = false;
            app->render_media_sequence = 0U;
            cadence_reset(&app->render_media_cadence);
        } else if (index ==
                   MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA) {
            app->camera_media_actual_present = false;
            app->camera_media_sequence = 0U;
            cadence_reset(&app->camera_media_cadence);
            app->camera_frame_pending = false;
            app->camera_frame_submitted = false;
            memset(&app->camera_frame, 0, sizeof(app->camera_frame));
        }
        return publish_actual(app, index);
    }
    if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI) {
        if (meiso_wifi_telemetry_desired_parameters_decode(
                desired->profile, &wifi
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
            !meiso_wifi_telemetry_parameters_supported(
                &app->wifi_capability, &wifi
            )) {
            goto rejected;
        }
        app->wifi_effective.selected_fields = wifi.selected_fields;
        app->wifi_effective.sample_period_ms = wifi.sample_period_ms;
        app->wifi_effective_present = true;
    } else if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE) {
        if (meiso_device_telemetry_parameters_decode(
                desired->profile, &device
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
            !meiso_device_telemetry_parameters_supported(
                &app->device_capability, &device
            )) {
            goto rejected;
        }
        app->device_effective = device;
        app->device_effective_present = true;
    } else if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA ||
               index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA) {
        media_capability = index ==
                MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA ?
            &app->catalog.render_media_capability :
            &app->catalog.camera_media_capability;
        if (meiso_composition_media_desired_decode(
                desired->profile, &media
            ) != MEISO_COMPOSITION_SESSION_V3_OK ||
            !meiso_composition_media_desired_supported(
                media_capability, &media
            )) {
            goto rejected;
        }
    } else {
        return false;
    }
    if (restart_production && app->active_media != NULL &&
        ((index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA &&
          (app->active_media->active_route_mask &
           MEISO_COMPOSITION_EDGE_APP_MEDIA_RENDER) != 0U) ||
         (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA &&
          (app->active_media->active_route_mask &
           MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA) != 0U))) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
    }
    if (restart_production &&
        index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA) {
        if (!stop_camera_bounded(app, true)) {
            return false;
        }
        app->camera_frame_pending = false;
        app->camera_frame_submitted = false;
        memset(&app->camera_frame, 0, sizeof(app->camera_frame));
    }
    if (!set_actual_applied(
            app, index, reference, true, true, restart_production
        )) {
        return false;
    }
    if ((index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA ||
         index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA) &&
        !fill_media_actual(app, index, &media)) {
        return false;
    }
    if (restart_production) {
        if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI) {
            app->wifi_next_due_ns = 0U;
        } else if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE) {
            app->device_next_due_ns = 0U;
            app->thermal_request_pending = false;
            app->thermal_request_dispatched = false;
            app->thermal_result_present = false;
            memset(&app->thermal_request, 0, sizeof(app->thermal_request));
            memset(&app->thermal_result, 0, sizeof(app->thermal_result));
        } else if (index ==
                   MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA) {
            app->render_media_sequence = 0U;
            cadence_reset(&app->render_media_cadence);
        } else if (index ==
                   MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA) {
            app->camera_media_sequence = 0U;
            cadence_reset(&app->camera_media_cadence);
        }
    }
    if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI &&
        !start_inline_producer(
            app, MEISO_COMPOSITION_EDGE_APP_INLINE_WIFI,
            app->actual[index].snapshot.production_epoch
        )) {
        return false;
    }
    if (index == MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE &&
        !start_inline_producer(
            app, MEISO_COMPOSITION_EDGE_APP_INLINE_DEVICE,
            app->actual[index].snapshot.production_epoch
        )) {
        return false;
    }
    return publish_actual(app, index);

rejected:
    set_actual_rejected(
        app, index, reference,
        MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
    );
    return publish_actual(app, index);
}

static bool apply_desired(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoStateView current,
    MeisoTimePoint now
)
{
    MeisoRuntimeDesiredState desired;
    MeisoRuntimeDesiredReference reference;
    if (!meiso_runtime_desired_state_decode(current.payload, &desired)) {
        return false;
    }
    reference.present = true;
    reference.owner = current.owner;
    reference.version = current.version;
    app->desired[index].decoded = desired;
    app->desired[index].reference = reference;
    switch (index) {
    case MEISO_COMPOSITION_EDGE_APP_DESIRED_PRESENTATION:
        return apply_presentation(app, &desired, reference, now);
    case MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE:
        return apply_scene(app, &desired, reference, now);
    case MEISO_COMPOSITION_EDGE_APP_DESIRED_WIFI:
        return apply_simple_desired(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI,
            &desired, reference
        );
    case MEISO_COMPOSITION_EDGE_APP_DESIRED_DEVICE:
        return apply_simple_desired(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE,
            &desired, reference
        );
    case MEISO_COMPOSITION_EDGE_APP_DESIRED_RENDER_MEDIA:
        return apply_simple_desired(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA,
            &desired, reference
        );
    case MEISO_COMPOSITION_EDGE_APP_DESIRED_CAMERA_MEDIA:
        return apply_simple_desired(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA,
            &desired, reference
        );
    default:
        return false;
    }
}

static void enter_terminal(
    MeisoCompositionEdgeApp *app,
    MeisoCompositionEdgeAppTerminalReason reason,
    int error_number
)
{
    bool camera_clean;
    MeisoCompositionMediaTerminalReason media_reason;
    if (!app_valid(app) ||
        app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
        return;
    }
    camera_clean = stop_camera_bounded(app, false);
    if (app->active_media != NULL && app->active_media->adapter != NULL) {
        media_reason = reason ==
                MEISO_COMPOSITION_EDGE_APP_TERMINAL_CATALOG_CHANGED ?
            MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED :
            reason == MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT ?
                MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE :
                MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION;
        (void)meiso_composition_media_posix_tcp_terminate(
            app->active_media->adapter,
            media_reason,
            error_number
        );
    }
    meiso_composition_debug_peer_abort(app->peer, error_number);
    app->active_media = NULL;
    memset(&app->blob_receive_transfer, 0,
           sizeof(app->blob_receive_transfer));
    memset(&app->blob_result_transfer, 0,
           sizeof(app->blob_result_transfer));
    app->blob_receive_next_offset = 0U;
    app->blob_result_operation = 0U;
    app->blob_receive_active = false;
    app->blob_receive_begin_ready = false;
    app->blob_result_pending = false;
    app->blob_result_queued = false;
    app->blob_result_marks_ready = false;
    app->blob_result_clears_active = false;
    app->thermal_request_pending = false;
    app->thermal_request_dispatched = false;
    app->thermal_result_present = false;
    app->terminal_reason = camera_clean ? reason :
        MEISO_COMPOSITION_EDGE_APP_TERMINAL_CAMERA_CLEANUP;
    app->phase = MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL;
}

static bool refresh_phase(MeisoCompositionEdgeApp *app)
{
    MeisoCompositionDebugPeerSnapshot peer;
    if (!app_valid(app) ||
        !meiso_composition_debug_peer_snapshot(app->peer, &peer)) {
        return false;
    }
    if (peer.terminal ||
        peer.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL) {
        enter_terminal(
            app,
            peer.session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST ?
                MEISO_COMPOSITION_EDGE_APP_TERMINAL_CONTROL_TRANSPORT :
                MEISO_COMPOSITION_EDGE_APP_TERMINAL_CONTROL_SESSION,
            0
        );
        return true;
    }
    switch (peer.phase) {
    case MEISO_COMPOSITION_DEBUG_PEER_PHASE_HANDSHAKE:
        app->phase = MEISO_COMPOSITION_EDGE_APP_PHASE_HANDSHAKE;
        break;
    case MEISO_COMPOSITION_DEBUG_PEER_PHASE_CATALOG:
        app->phase = MEISO_COMPOSITION_EDGE_APP_PHASE_CATALOG;
        break;
    case MEISO_COMPOSITION_DEBUG_PEER_PHASE_REBIND:
        if (app->deliveries_started) {
            enter_terminal(
                app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_CATALOG_CHANGED, 0
            );
        } else {
            app->phase = MEISO_COMPOSITION_EDGE_APP_PHASE_REBIND;
        }
        break;
    case MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY:
        if (!ensure_coordinator(app)) {
            return false;
        }
        app->deliveries_started = true;
        app->phase = MEISO_COMPOSITION_EDGE_APP_PHASE_READY;
        break;
    case MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION:
        break;
    case MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL:
    default:
        return false;
    }
    return true;
}

static bool remember_visible_blob(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeBlobTransferRef *transfer
)
{
    size_t index;
    if (app == NULL || transfer == NULL) {
        return false;
    }
    for (index = 0U; index < app->visible_blob_count; ++index) {
        if (meiso_content_id_equal(
                &app->visible_blob_ids[index], &transfer->content_id
            )) {
            return app->visible_blob_sizes[index] == transfer->total_size;
        }
    }
    if (app->visible_blob_count >=
        MEISO_COMPOSITION_EDGE_APP_MESH_BLOB_SLOTS) {
        return false;
    }
    index = app->visible_blob_count++;
    app->visible_blob_ids[index] = transfer->content_id;
    app->visible_blob_sizes[index] = transfer->total_size;
    return true;
}

static bool apply_visible_blobs(MeisoCompositionEdgeApp *app)
{
    size_t index;
    MeisoCompositionSceneBlobVisibilityStatus status;
    if (!app->desired_owner_present) {
        return true;
    }
    for (index = 0U; index < app->visible_blob_count; ++index) {
        status = meiso_composition_scene_store_mark_blob_visible(
            app->scene_store, app->desired_owner.incarnation,
            &app->visible_blob_ids[index],
            (size_t)app->visible_blob_sizes[index]
        );
        if (status != MEISO_COMPOSITION_SCENE_BLOB_VISIBLE &&
            status != MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED) {
            return false;
        }
    }
    return true;
}

static bool handle_desired_offer(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    size_t index;
    EdgeDesiredChannel *channel;
    MeisoStateReceiveStatus receive;
    MeisoStateView current;
    MeisoStateOwnerStatus owner_status;
    if (!route_channel(
            app, event->route, DESIRED_ROUTE_INDEX,
            MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT, &index
        ) || !ensure_desired_owner(app, event->state_offer.state.owner)) {
        return false;
    }
    channel = &app->desired[index];
    if (!channel->owner_started) {
        owner_status = meiso_state_replica_begin_owner(
            channel->replica, event->state_offer.state.owner
        );
        if (owner_status != MEISO_STATE_OWNER_STARTED &&
            owner_status != MEISO_STATE_OWNER_UNCHANGED) {
            return false;
        }
        channel->owner_started = true;
    }
    receive = meiso_state_replica_receive(
        channel->replica, event->state_offer.state.owner,
        event->state_offer.state.version,
        event->state_offer.state.payload
    );
    if (meiso_composition_debug_peer_commit_state_receive(
            app->peer, event->route, &event->state_offer, receive
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK ||
        (receive != MEISO_STATE_RECEIVE_NEW &&
         receive != MEISO_STATE_RECEIVE_DUPLICATE) ||
        !meiso_state_replica_current(channel->replica, &current)) {
        return false;
    }
    channel->accept_pending = true;
    channel->accept_generation = event->state_offer.generation;
    channel->accept_owner = event->state_offer.state.owner;
    channel->accept_version = event->state_offer.state.version;
    if ((receive == MEISO_STATE_RECEIVE_NEW &&
         !apply_desired(app, index, current, now)) ||
        !apply_visible_blobs(app)) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event =
        app->actual[index].snapshot.apply_result ==
                MEISO_RUNTIME_APPLY_REJECTED ?
            MEISO_COMPOSITION_EDGE_APP_PUMP_DESIRED_REJECTED :
            MEISO_COMPOSITION_EDGE_APP_PUMP_DESIRED_APPLIED;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    out_result->desired_reference_present = true;
    out_result->desired_reference = (MeisoRuntimeDesiredReference){
        true, current.owner, current.version
    };
    return true;
}

static bool handle_actual_accept(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeEvent *event,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    size_t index;
    EdgeActualChannel *channel;
    MeisoStateOffer next;
    MeisoStateAcceptStatus status;
    if (!route_channel(
            app, event->route, ACTUAL_ROUTE_INDEX,
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT, &index
        )) {
        return false;
    }
    channel = &app->actual[index];
    status = meiso_state_publisher_mark_delivery_accepted(
        channel->publisher, event->state_accept_generation,
        event->state_accept_owner, event->state_accept_version
    );
    if (status != MEISO_STATE_ACCEPT_CONFIRMED &&
        status != MEISO_STATE_ACCEPT_DUPLICATE &&
        status != MEISO_STATE_ACCEPT_STALE) {
        return false;
    }
    channel->offer_sent = false;
    channel->snapshot.delivery_pending =
        meiso_state_publisher_next_offer(channel->publisher, &next);
    channel->snapshot.delivery_accepted =
        !channel->snapshot.delivery_pending;
    channel->snapshot.delivery_failed = false;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_ACTUAL_DELIVERY;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

static bool blob_route_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static bool blob_transfer_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        blob_route_equal(lhs->route, rhs->route) &&
        lhs->transfer_id == rhs->transfer_id &&
        meiso_content_id_equal(&lhs->content_id, &rhs->content_id) &&
        lhs->total_size == rhs->total_size;
}

static bool queue_blob_result(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobOperation operation,
    bool marks_ready,
    bool clears_active
)
{
    MeisoRuntimeSchedulerWork work;
    if (app == NULL || transfer == NULL || app->blob_result_pending ||
        app->blob_result_queued) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
    work.value.blob_result.transfer = *transfer;
    work.value.blob_result.operation = operation;
    if (meiso_composition_debug_peer_queue_work(
            app->peer, SOURCE_BLOB_RESULT, &work
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    app->blob_result_transfer = *transfer;
    app->blob_result_operation = operation;
    app->blob_result_pending = true;
    app->blob_result_queued = true;
    app->blob_result_marks_ready = marks_ready;
    app->blob_result_clears_active = clears_active;
    return true;
}

static bool handle_blob_request(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoRuntimeBlobPublishReport report;
    uint64_t previous_high_water;
    uint64_t chunk_end;
    bool prepare_attempted;
    bool active_plan;
    bool begin_accepted = false;
    bool result_required = false;
    bool result_clears_active = false;
    bool core_published = false;
    MeisoCompositionScenePrepareStatus prepare;
    previous_high_water = app->blob_transfer_id_high_water;
    if (event->blob_transfer.transfer_id >
        app->blob_transfer_id_high_water) {
        app->blob_transfer_id_high_water = event->blob_transfer.transfer_id;
    }
    switch (event->blob_operation) {
    case MEISO_RUNTIME_BLOB_BEGIN:
        begin_accepted = event->blob_transfer.transfer_id >
                previous_high_water &&
            meiso_content_id_valid(&event->blob_transfer.content_id) &&
            !app->blob_receive_active &&
            event->blob_transfer.total_size <=
                MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
        if (begin_accepted) {
            app->blob_receive_transfer = event->blob_transfer;
            app->blob_receive_next_offset = 0U;
            app->blob_receive_active = true;
            app->blob_receive_begin_ready = false;
        }
        result_required = true;
        break;
    case MEISO_RUNTIME_BLOB_CHUNK:
        if (!app->blob_receive_active ||
            !blob_transfer_equal(
                &event->blob_transfer, &app->blob_receive_transfer
            ) || !app->blob_receive_begin_ready) {
            return false;
        }
        if (event->blob_offset != app->blob_receive_next_offset ||
            event->blob_payload.size > UINT64_MAX - event->blob_offset) {
            result_required = true;
            result_clears_active = true;
            break;
        }
        chunk_end = event->blob_offset + event->blob_payload.size;
        if (chunk_end > app->blob_receive_transfer.total_size ||
            (event->blob_payload.size <
                    MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE &&
             chunk_end != app->blob_receive_transfer.total_size)) {
            result_required = true;
            result_clears_active = true;
        } else {
            app->blob_receive_next_offset = chunk_end;
        }
        break;
    case MEISO_RUNTIME_BLOB_COMMIT:
        if (!app->blob_receive_active ||
            !blob_transfer_equal(
                &event->blob_transfer, &app->blob_receive_transfer
            ) || !app->blob_receive_begin_ready) {
            return false;
        }
        result_required = true;
        result_clears_active = true;
        if (app->blob_receive_next_offset ==
                event->blob_transfer.total_size &&
            meiso_runtime_blob_sha256_verify(
                &event->blob_transfer.content_id,
                event->blob_payload, NULL
            )) {
            memset(&report, 0, sizeof(report));
            if (meiso_composition_debug_peer_publish_blob_receive(
                    app->peer, &event->blob_transfer, app->blob_store,
                    meiso_composition_scene_mesh_blob_verify, NULL, &report
                ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
                return false;
            }
            if (report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED) {
                if (app->blob_published_bytes >
                    UINT64_MAX - event->blob_transfer.total_size) {
                    return false;
                }
                app->blob_published_bytes +=
                    event->blob_transfer.total_size;
            }
            core_published =
                report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
                report.result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
            if (core_published &&
                (!remember_visible_blob(app, &event->blob_transfer) ||
                 !apply_visible_blobs(app) ||
                 !prepare_stored_scene(
                     app, &prepare_attempted, &active_plan, &prepare
                 ) || !try_prepare_presentation(app, now) ||
                 !refresh_composition_actuals(app))) {
                return false;
            }
        }
        break;
    case MEISO_RUNTIME_BLOB_ABORT:
        if (!app->blob_receive_active ||
            !blob_transfer_equal(
                &event->blob_transfer, &app->blob_receive_transfer
            )) {
            return false;
        }
        result_required = true;
        result_clears_active = true;
        break;
    default:
        return false;
    }
    if (result_required && !queue_blob_result(
            app, &event->blob_transfer, event->blob_operation,
            begin_accepted, result_clears_active
        )) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = core_published ?
        MEISO_COMPOSITION_EDGE_APP_PUMP_BLOB_PUBLISHED :
        MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS;
    return true;
}

static bool handle_runtime_event(
    MeisoCompositionEdgeApp *app,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    switch (event->type) {
    case MEISO_RUNTIME_EVENT_STATE_OFFER:
        return handle_desired_offer(app, event, now, out_result);
    case MEISO_RUNTIME_EVENT_STATE_ACCEPT:
        return handle_actual_accept(app, event, out_result);
    case MEISO_RUNTIME_EVENT_BLOB_REQUEST:
        return handle_blob_request(app, event, now, out_result);
    case MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED:
        if (event->catalog_generation != app->config.catalog_generation) {
            return false;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS;
        return true;
    case MEISO_RUNTIME_EVENT_CATALOG_OFFERED:
    case MEISO_RUNTIME_EVENT_CATALOG_REJECTED:
    case MEISO_RUNTIME_EVENT_STREAM_SAMPLE:
    case MEISO_RUNTIME_EVENT_BLOB_RESULT:
    default:
        return false;
    }
}

static bool handle_source_completion(
    MeisoCompositionEdgeApp *app,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoRuntimeSchedulerCompletion completion;
    size_t index;
    if (meiso_composition_debug_peer_take_completion(
            app->peer, SOURCE_BLOB_RESULT, &completion
        ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
        app->blob_result_queued = false;
        if (!app->blob_result_pending || completion.code !=
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
            enter_terminal(
                app,
                MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE,
                (int)completion.runtime_status
            );
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_COMPOSITION_EDGE_APP_PUMP_EVENT_TERMINAL;
            return true;
        }
        if (app->blob_result_marks_ready && app->blob_receive_active &&
            blob_transfer_equal(
                &app->blob_result_transfer, &app->blob_receive_transfer
            )) {
            app->blob_receive_begin_ready = true;
        }
        if (app->blob_result_clears_active && app->blob_receive_active &&
            blob_transfer_equal(
                &app->blob_result_transfer, &app->blob_receive_transfer
            )) {
            memset(
                &app->blob_receive_transfer, 0,
                sizeof(app->blob_receive_transfer)
            );
            app->blob_receive_next_offset = 0U;
            app->blob_receive_active = false;
            app->blob_receive_begin_ready = false;
        }
        memset(
            &app->blob_result_transfer, 0,
            sizeof(app->blob_result_transfer)
        );
        app->blob_result_operation = 0U;
        app->blob_result_pending = false;
        app->blob_result_marks_ready = false;
        app->blob_result_clears_active = false;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event =
            MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS;
        return true;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT;
         ++index) {
        if (meiso_composition_debug_peer_take_completion(
                app->peer, DESIRED_SOURCE_INDEX[index], &completion
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
            app->desired[index].accept_queued = false;
            if (completion.code !=
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                enter_terminal(
                    app,
                    MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE,
                    (int)completion.runtime_status
                );
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_COMPOSITION_EDGE_APP_PUMP_EVENT_TERMINAL;
                return true;
            }
            app->desired[index].accept_pending = false;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS;
            out_result->desired_channel_present = true;
            out_result->desired_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT;
         ++index) {
        if (meiso_composition_debug_peer_take_completion(
                app->peer, ACTUAL_SOURCE_INDEX[index], &completion
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
            app->actual[index].offer_queued = false;
            if (completion.code !=
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                app->actual[index].snapshot.delivery_failed = true;
                enter_terminal(
                    app,
                    MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE,
                    (int)completion.runtime_status
                );
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_COMPOSITION_EDGE_APP_PUMP_EVENT_TERMINAL;
                return true;
            }
            app->actual[index].offer_sent = true;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_COMPOSITION_EDGE_APP_PUMP_ACTUAL_DELIVERY;
            out_result->actual_channel_present = true;
            out_result->actual_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT; ++index) {
        if (meiso_composition_debug_peer_take_completion(
                app->peer, INLINE_SOURCE_INDEX[index], &completion
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK) {
            app->inline_streams[index].sample_queued = false;
            app->inline_streams[index].sample_sent = false;
            if (!meiso_stream_pop(app->inline_streams[index].stream)) {
                return false;
            }
            app->inline_streams[index].snapshot.queue_depth =
                (uint32_t)meiso_stream_count(
                    app->inline_streams[index].stream
                );
            if (completion.code ==
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                ++app->inline_streams[index].snapshot.samples_accepted;
                if (index ==
                        MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT &&
                    app->render_complete_present &&
                    app->inline_streams[index].pending_sequence != 0U) {
                    if (!meiso_composition_scene_store_record_result_published(
                            app->scene_store,
                            &app->render_complete.job.world,
                            app->inline_streams[index].snapshot.stream_epoch,
                            app->inline_streams[index].pending_sequence
                        )) {
                        return false;
                    }
                    app->render_inline_accepted = true;
                }
            } else {
                ++app->inline_streams[index].snapshot.samples_rejected;
                if (index ==
                    MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT) {
                    app->render_inline_accepted = true;
                }
            }
            app->inline_streams[index].pending_sequence = 0U;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_INLINE_SAMPLE;
            out_result->inline_channel_present = true;
            out_result->inline_channel = (uint8_t)index;
            return true;
        }
    }
    return false;
}

static bool app_source_work_outstanding(
    const MeisoCompositionEdgeApp *app
)
{
    size_t index;
    if (app == NULL) {
        return false;
    }
    if (app->blob_result_pending || app->blob_result_queued) {
        return true;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT;
         ++index) {
        if (app->desired[index].accept_queued ||
            app->actual[index].offer_queued) {
            return true;
        }
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT; ++index) {
        if (app->inline_streams[index].sample_queued) {
            return true;
        }
    }
    return false;
}

static bool queue_desired_accept(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    EdgeDesiredChannel *channel = &app->desired[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    if (!channel->accept_pending || channel->accept_queued ||
        !meiso_composition_debug_peer_route_key(
            app->peer, DESIRED_ROUTE_INDEX[index], &route
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
            app->peer, DESIRED_SOURCE_INDEX[index], &work
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    channel->accept_queued = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    return true;
}

static bool queue_actual_offer(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    EdgeActualChannel *channel = &app->actual[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoStateOffer offer;
    if (channel->offer_queued || channel->offer_sent ||
        !meiso_state_publisher_next_offer(channel->publisher, &offer) ||
        !meiso_composition_debug_peer_route_key(
            app->peer, ACTUAL_ROUTE_INDEX[index], &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = route;
    work.value.state_offer.offer = offer;
    if (meiso_composition_debug_peer_queue_work(
            app->peer, ACTUAL_SOURCE_INDEX[index], &work
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    channel->offer_queued = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_ACTUAL_DELIVERY;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

static bool queue_inline_sample(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    EdgeInlineChannel *channel = &app->inline_streams[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoStreamSample sample;
    if (channel->sample_queued || channel->sample_sent ||
        !meiso_stream_peek(channel->stream, &sample) ||
        !meiso_composition_debug_peer_route_key(
            app->peer, INLINE_ROUTE_INDEX[index], &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    work.value.stream_sample.route = route;
    work.value.stream_sample.production_epoch = sample.key.epoch;
    work.value.stream_sample.sample = sample;
    if (meiso_composition_debug_peer_queue_work(
            app->peer, INLINE_SOURCE_INDEX[index], &work
        ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
        return false;
    }
    channel->sample_queued = true;
    channel->sample_sent = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_INLINE_SAMPLE;
    out_result->inline_channel_present = true;
    out_result->inline_channel = (uint8_t)index;
    return true;
}

static bool enqueue_inline_payload(
    MeisoCompositionEdgeApp *app,
    size_t index,
    MeisoTimePoint source_time,
    MeisoByteView payload
)
{
    EdgeInlineChannel *channel;
    MeisoStreamSample sample;
    MeisoStreamReceiveResult receive;
    uint64_t sequence;
    if (index >= MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT ||
        !meiso_time_point_valid(source_time) ||
        source_time.clock_epoch != app->config.edge_clock_epoch ||
        payload.data == NULL || payload.size == 0U) {
        return false;
    }
    channel = &app->inline_streams[index];
    if (!channel->snapshot.enabled ||
        channel->snapshot.next_sequence == 0U ||
        channel->snapshot.next_sequence == UINT64_MAX) {
        return false;
    }
    sequence = channel->snapshot.next_sequence;
    memset(&sample, 0, sizeof(sample));
    sample.key.producer = edge_owner(&app->config);
    sample.key.epoch = channel->snapshot.stream_epoch;
    sample.key.sequence = sequence;
    sample.source_time = source_time;
    sample.payload = payload;
    receive = meiso_stream_receive(channel->stream, &sample, NULL);
    if (receive.code != MEISO_STREAM_RECEIVE_ACCEPTED) {
        return false;
    }
    channel->pending_sequence = sequence;
    channel->snapshot.next_sequence = sequence + 1U;
    ++channel->snapshot.samples_materialized;
    channel->snapshot.queue_depth = (uint32_t)meiso_stream_count(
        channel->stream
    );
    return true;
}

static bool actual_delivery_current(
    const MeisoCompositionEdgeApp *app,
    size_t index
)
{
    return app != NULL && index < MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT &&
        app->actual[index].snapshot.delivery_accepted &&
        !app->actual[index].snapshot.delivery_pending &&
        !app->actual[index].snapshot.delivery_failed;
}

static int pump_wifi_sample(
    MeisoCompositionEdgeApp *app,
    MeisoTimePoint now,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    size_t size = 0U;
    uint64_t period_ns;
    EdgeInlineChannel *channel = &app->inline_streams[
        MEISO_COMPOSITION_EDGE_APP_INLINE_WIFI
    ];
    if (!app->wifi_effective_present || !channel->snapshot.enabled ||
        !actual_delivery_current(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI
        ) ||
        meiso_stream_count(channel->stream) >=
            MEISO_COMPOSITION_EDGE_APP_INLINE_QUEUE_SLOTS ||
        (app->wifi_next_due_ns != 0U &&
         now.monotonic_ns < app->wifi_next_due_ns)) {
        return 0;
    }
    if (meiso_wifi_linux_adapter_sample(
            &app->wifi, app->wifi_effective.selected_fields,
            &app->wifi_sample
        ) != MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_wifi_telemetry_sample_encode(
            &app->wifi_sample,
            (MeisoMutableBuffer){
                app->stream_scratch, sizeof(app->stream_scratch)
            }, &size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        !enqueue_inline_payload(
            app, MEISO_COMPOSITION_EDGE_APP_INLINE_WIFI, now,
            (MeisoByteView){app->stream_scratch, size}
        )) {
        return -1;
    }
    period_ns = (uint64_t)app->wifi_effective.sample_period_ms *
        UINT64_C(1000000);
    if (now.monotonic_ns > UINT64_MAX - period_ns) {
        return -1;
    }
    app->wifi_next_due_ns = now.monotonic_ns + period_ns;
    app->wifi_sample_present = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_INLINE_SAMPLE;
    out_result->inline_channel_present = true;
    out_result->inline_channel = MEISO_COMPOSITION_EDGE_APP_INLINE_WIFI;
    return 1;
}

static bool desired_reference_equal(
    MeisoRuntimeDesiredReference lhs,
    MeisoRuntimeDesiredReference rhs
)
{
    return lhs.present == rhs.present && (!lhs.present ||
        (owner_equal(lhs.owner, rhs.owner) && lhs.version == rhs.version));
}

static int pump_device_sample(
    MeisoCompositionEdgeApp *app,
    MeisoTimePoint now,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    EdgeInlineChannel *channel = &app->inline_streams[
        MEISO_COMPOSITION_EDGE_APP_INLINE_DEVICE
    ];
    const bool temperature_selected =
        (app->device_effective.selected_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U;
    const MeisoDeviceTelemetryLinuxTemperature *temperature = NULL;
    uint64_t period_ns;
    uint64_t budget_ns;
    size_t size = 0U;
    if (!app->device_effective_present || !channel->snapshot.enabled ||
        !actual_delivery_current(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE
        ) ||
        meiso_stream_count(channel->stream) >=
            MEISO_COMPOSITION_EDGE_APP_INLINE_QUEUE_SLOTS ||
        (app->device_next_due_ns != 0U &&
         now.monotonic_ns < app->device_next_due_ns)) {
        return 0;
    }
    if (temperature_selected && !app->thermal_request_pending) {
        if (app->next_thermal_request_id == UINT64_MAX) {
            return -1;
        }
        budget_ns = (uint64_t)app->config.thermal_worker_budget_ms *
            UINT64_C(1000000);
        if (now.monotonic_ns > UINT64_MAX - budget_ns) {
            return -1;
        }
        memset(&app->thermal_request, 0, sizeof(app->thermal_request));
        app->thermal_request.request_id = app->next_thermal_request_id++;
        app->thermal_request.observed_desired = app->actual[
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE
        ].snapshot.observed_desired;
        app->thermal_request.production_epoch = app->actual[
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_DEVICE
        ].snapshot.production_epoch;
        app->thermal_request.sample_due = now;
        app->thermal_request.deadline = (MeisoTimePoint){
            now.clock_epoch, now.monotonic_ns + budget_ns
        };
        app->thermal_request.reader = app->device;
        app->thermal_request_pending = true;
        app->thermal_request_dispatched = false;
        app->thermal_result_present = false;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_THERMAL_REQUEST;
        return 1;
    }
    if (temperature_selected && app->thermal_request_pending &&
        !app->thermal_result_present &&
        now.monotonic_ns <= app->thermal_request.deadline.monotonic_ns) {
        return 0;
    }
    if (temperature_selected && app->thermal_result_present &&
        app->thermal_result.read_status ==
            MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_READY &&
        app->thermal_result.request_id == app->thermal_request.request_id &&
        app->thermal_result.production_epoch ==
            app->thermal_request.production_epoch &&
        desired_reference_equal(
            app->thermal_result.observed_desired,
            app->thermal_request.observed_desired
        )) {
        temperature = &app->thermal_result.temperature;
    }
    if (meiso_device_telemetry_linux_adapter_sample(
            &app->device, app->device_effective.selected_fields,
            temperature, &app->device_sample
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK ||
        meiso_device_telemetry_sample_encode(
            &app->device_sample,
            (MeisoMutableBuffer){
                app->stream_scratch, sizeof(app->stream_scratch)
            }, &size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        !enqueue_inline_payload(
            app, MEISO_COMPOSITION_EDGE_APP_INLINE_DEVICE, now,
            (MeisoByteView){app->stream_scratch, size}
        )) {
        return -1;
    }
    period_ns = (uint64_t)app->device_effective.sample_period_ms *
        UINT64_C(1000000);
    if (now.monotonic_ns > UINT64_MAX - period_ns) {
        return -1;
    }
    app->device_next_due_ns = now.monotonic_ns + period_ns;
    app->device_sample_present = true;
    app->thermal_request_pending = false;
    app->thermal_request_dispatched = false;
    app->thermal_result_present = false;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_INLINE_SAMPLE;
    out_result->inline_channel_present = true;
    out_result->inline_channel = MEISO_COMPOSITION_EDGE_APP_INLINE_DEVICE;
    return 1;
}

static bool media_route_active(
    const MeisoCompositionEdgeApp *app,
    uint8_t route_mask
)
{
    MeisoCompositionMediaTransportSnapshot snapshot;
    return app->active_media != NULL &&
        (app->active_media->active_route_mask & route_mask) != 0U &&
        meiso_composition_media_transport_snapshot(
            app->active_media->transport, &snapshot
        ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
        snapshot.phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE;
}

static bool fail_camera_production(
    MeisoCompositionEdgeApp *app,
    bool unavailable
)
{
    EdgeActualChannel *channel = &app->actual[
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
    ];
    if (!stop_camera_bounded(app, false)) {
        enter_terminal(
            app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_CAMERA_CLEANUP, 0
        );
        return false;
    }
    if (app->active_media != NULL) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
    }
    channel->snapshot.apply_result = MEISO_RUNTIME_APPLY_FAILED;
    channel->snapshot.availability = unavailable ?
        MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE :
        MEISO_RUNTIME_AVAILABILITY_PRESENT;
    channel->snapshot.effective_enabled = false;
    channel->snapshot.production_status = MEISO_RUNTIME_PRODUCTION_FAILED;
    channel->snapshot.reason = unavailable ?
        MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE :
        MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED;
    channel->snapshot.production_epoch = 0U;
    app->camera_media_actual_present = false;
    app->camera_media_sequence = 0U;
    cadence_reset(&app->camera_media_cadence);
    app->camera_frame_pending = false;
    app->camera_frame_submitted = false;
    memset(&app->camera_frame, 0, sizeof(app->camera_frame));
    return publish_actual(
        app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
    );
}

static int pump_renderer(
    MeisoCompositionEdgeApp *app,
    MeisoTimePoint now,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionSceneWorldView world;
    MeisoCompositionCpuRasterSubmitStatus submit;
    MeisoCompositionCpuRasterRenderStatus render;
    MeisoCompositionMediaUnit unit;
    size_t size = 0U;
    bool render_route;
    uint32_t result_rate_millihz;
    uint64_t result_epoch;
    if (!app->actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE]
             .snapshot.effective_enabled ||
        !app->actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION]
             .snapshot.effective_enabled ||
        !actual_delivery_current(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE
        ) || !actual_delivery_current(
            app, MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION
        ) ||
        !app->presentation_plan_present ||
        !meiso_composition_scene_store_control_scope(
            app->scene_store, &scope
        ) || !meiso_composition_scene_store_active(app->scene_store, &world)) {
        return 0;
    }
    result_epoch = app->inline_streams[
        MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
    ].snapshot.stream_epoch;
    result_rate_millihz =
        (uint32_t)app->presentation_plan.max_result_hz * UINT32_C(1000);
    if (!app->render_job_pending && !app->render_complete_present) {
        if (!cadence_ready(
                &app->render_result_cadence, result_epoch,
                result_rate_millihz, now.monotonic_ns
            )) {
            return 0;
        }
        submit = meiso_composition_cpu_raster_renderer_submit(
            app->renderer, &scope, &world, app->config.edge_clock_epoch,
            &app->render_job
        );
        if (submit != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED) {
            return submit == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUE_FULL ?
                0 : -1;
        }
        app->render_job_pending = true;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_SCENE_PROGRESS;
        return 1;
    }
    if (app->render_job_pending) {
        if (now.monotonic_ns == UINT64_MAX) {
            return -1;
        }
        render = meiso_composition_cpu_raster_renderer_render_next(
            app->renderer, &app->render_job, now.monotonic_ns,
            now.monotonic_ns + 1U, &app->render_complete
        );
        if (render != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
            return render == MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB ?
                0 : -1;
        }
        app->render_job_pending = false;
        app->render_complete_present = true;
        app->render_inline_materialized = false;
        app->render_inline_accepted = false;
        app->render_media_materialized = false;
        if (!cadence_commit(
                &app->render_result_cadence, now.monotonic_ns
            )) {
            return -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_RENDER_COMPLETE;
        return 1;
    }
    if (!app->render_inline_materialized) {
        if (meiso_composition_scene_render_result_encode(
                &app->render_complete.result,
                (MeisoMutableBuffer){
                    app->stream_scratch, sizeof(app->stream_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
            !enqueue_inline_payload(
                app, MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT, now,
                (MeisoByteView){app->stream_scratch, size}
            )) {
            return -1;
        }
        app->render_inline_materialized = true;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_INLINE_SAMPLE;
        out_result->inline_channel_present = true;
        out_result->inline_channel =
            MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT;
        return 1;
    }
    render_route = media_route_active(
        app, MEISO_COMPOSITION_EDGE_APP_MEDIA_RENDER
    );
    if (render_route && !app->render_media_materialized) {
        if (!app->render_media_actual_present || !cadence_ready(
                &app->render_media_cadence,
                app->actual[
                    MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA
                ].snapshot.production_epoch,
                app->render_media_actual.effective.fps_millihz,
                now.monotonic_ns
            )) {
            app->render_media_materialized = true;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_SCENE_PROGRESS;
            return 1;
        }
        if (app->render_media_sequence == UINT64_MAX) {
            return -1;
        }
        memset(&unit, 0, sizeof(unit));
        unit.binding = app->active_media->transport_config.routes[0].binding;
        unit.frame_sequence = app->render_media_sequence + 1U;
        unit.source_time = now;
        unit.applied_scene_owner = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_HOST,
            app->render_complete.job.world.desired_owner_incarnation
        };
        unit.applied_scene_state_version =
            app->render_complete.job.world.desired_state_version;
        unit.active_plan_generation =
            app->render_complete.job.world.plan_generation;
        unit.world_generation =
            app->render_complete.job.world.world_generation;
        unit.dependency_sequence = app->inline_streams[
            MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
        ].pending_sequence;
        unit.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
        unit.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
        unit.width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
        unit.height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
        unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
        unit.access_unit = app->render_complete.color_rgba8888;
        {
            const MeisoCompositionMediaTransportStatus submit_status =
                meiso_composition_media_transport_submit(
                    app->active_media->transport, &unit
                );
            if (submit_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
                submit_status !=
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED) {
                return -1;
            }
        }
        app->render_media_sequence = unit.frame_sequence;
        app->render_media_materialized = true;
        if (!cadence_commit(
                &app->render_media_cadence, now.monotonic_ns
            )) {
            return -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_MEDIA_PROGRESS;
        out_result->media_source_present = true;
        out_result->media_source =
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
        return 1;
    }
    if (!render_route) {
        app->render_media_materialized = true;
    }
    if (app->render_inline_accepted && app->render_media_materialized) {
        app->render_complete_present = false;
        memset(&app->render_complete, 0, sizeof(app->render_complete));
        return 0;
    }
    return 0;
}

static int pump_camera(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionEdgeAppIoReady *ready,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoCameraV4l2Snapshot snapshot;
    MeisoCameraV4l2StartStatus start;
    MeisoCameraV4l2PollStatus poll;
    MeisoCameraV4l2ReleaseStatus release;
    MeisoCompositionMediaUnit unit;
    if (!app->camera_media_actual_present ||
        !app->actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA]
             .snapshot.effective_enabled ||
        !media_route_active(app, MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA)) {
        if (meiso_camera_v4l2_snapshot(app->camera, &snapshot) &&
            snapshot.phase != MEISO_CAMERA_V4L2_PHASE_STOPPED) {
            if (!stop_camera_bounded(app, true)) {
                return -3;
            }
            app->camera_frame_pending = false;
            app->camera_frame_submitted = false;
            memset(&app->camera_frame, 0, sizeof(app->camera_frame));
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
            return 1;
        }
        return 0;
    }
    if (!meiso_camera_v4l2_snapshot(app->camera, &snapshot)) {
        return -1;
    }
    if (snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL) {
        if (!fail_camera_production(app, false)) {
            return app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ?
                -3 : -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
        return 1;
    }
    if (snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) {
        start = meiso_camera_v4l2_start(app->camera);
        if (start != MEISO_CAMERA_V4L2_START_STARTED &&
            start != MEISO_CAMERA_V4L2_START_UNCHANGED) {
            if (!fail_camera_production(app, true)) {
                return app->phase ==
                        MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ? -3 : -1;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
            return 1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_CAMERA_FRAME;
        return 1;
    }
    if (app->camera_frame_pending && !app->camera_frame_submitted) {
        if (app->camera_media_sequence == UINT64_MAX) {
            return -1;
        }
        memset(&unit, 0, sizeof(unit));
        unit.binding = app->active_media->transport_config.routes[1].binding;
        unit.frame_sequence = app->camera_media_sequence + 1U;
        unit.source_time = app->camera_frame.source_time;
        unit.producer_dropped_frames =
            app->camera_frame.producer_dropped_frames;
        unit.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
        unit.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
        unit.width = MEISO_CAMERA_V4L2_WIDTH;
        unit.height = MEISO_CAMERA_V4L2_HEIGHT;
        unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
        unit.access_unit = app->camera_frame.bytes;
        {
            const MeisoCompositionMediaTransportStatus submit_status =
                meiso_composition_media_transport_submit(
                    app->active_media->transport, &unit
                );
            if (submit_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
                submit_status !=
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED) {
                if (!fail_camera_production(app, false)) {
                    return app->phase ==
                            MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ?
                        -3 : -1;
                }
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
                return 1;
            }
        }
        app->camera_media_sequence = unit.frame_sequence;
        app->camera_frame_submitted = true;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_MEDIA_PROGRESS;
        out_result->media_source_present = true;
        out_result->media_source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        return 1;
    }
    if (app->camera_frame_pending && app->camera_frame_submitted) {
        release = meiso_camera_v4l2_release_frame(
            app->camera, &app->camera_frame.ref
        );
        if (release != MEISO_CAMERA_V4L2_RELEASE_RELEASED) {
            if (!fail_camera_production(app, false)) {
                return app->phase ==
                        MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ? -3 : -1;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
            return 1;
        }
        app->camera_frame_pending = false;
        app->camera_frame_submitted = false;
        memset(&app->camera_frame, 0, sizeof(app->camera_frame));
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_CAMERA_FRAME;
        return 1;
    }
    if (!ready->camera_poll_allowed) {
        return 0;
    }
    if (!cadence_ready(
            &app->camera_media_cadence,
            app->actual[
                MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
            ].snapshot.production_epoch,
            app->camera_media_actual.effective.fps_millihz,
            ready->now.monotonic_ns
        )) {
        return 0;
    }
    poll = meiso_camera_v4l2_poll_frame(app->camera, &app->camera_frame);
    if (poll == MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK ||
        poll == MEISO_CAMERA_V4L2_POLL_INTERRUPTED) {
        return -2;
    }
    if (poll != MEISO_CAMERA_V4L2_POLL_FRAME) {
        if (!fail_camera_production(app, false)) {
            return app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ?
                -3 : -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
        return 1;
    }
    if (!cadence_commit(
            &app->camera_media_cadence, ready->now.monotonic_ns
        )) {
        if (!fail_camera_production(app, false)) {
            return app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ?
                -3 : -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
        return 1;
    }
    app->camera_frame_pending = true;
    app->camera_frame_submitted = false;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_CAMERA_FRAME;
    out_result->media_source_present = true;
    out_result->media_source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    return 1;
}

static MeisoCompositionEdgeAppPumpStatus pump_local(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionEdgeAppIoReady *ready,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    size_t index;
    int local_status;
    if (app->phase != MEISO_COMPOSITION_EDGE_APP_PHASE_READY) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
    }
    if (handle_source_completion(app, out_result)) {
        return app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ?
            MEISO_COMPOSITION_EDGE_APP_PUMP_TERMINAL :
            MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
    }
    if (app_source_work_outstanding(app)) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_DESIRED_COUNT;
         ++index) {
        if (queue_desired_accept(app, index, out_result)) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT;
         ++index) {
        if (queue_actual_offer(app, index, out_result)) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
        }
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT; ++index) {
        if (queue_inline_sample(app, index, out_result)) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
        }
    }
    local_status = pump_renderer(app, ready->now, out_result);
    if (local_status != 0) {
        return local_status > 0 ? MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS :
            MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    local_status = pump_camera(app, ready, out_result);
    if (local_status != 0) {
        if (local_status > 0) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
        }
        if (local_status == -2) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_WOULD_BLOCK;
        }
        if (local_status == -3) {
            enter_terminal(
                app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_CAMERA_CLEANUP, 0
            );
        }
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    local_status = pump_wifi_sample(app, ready->now, out_result);
    if (local_status != 0) {
        return local_status > 0 ? MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS :
            MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    local_status = pump_device_sample(app, ready->now, out_result);
    if (local_status != 0) {
        return local_status > 0 ? MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS :
            MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
}

static MeisoCompositionEdgeAppPumpStatus pump_control(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionEdgeAppIoReady *ready,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoCompositionDebugPeerPumpResult peer_result;
    MeisoCompositionDebugPeerPumpStatus status;
    MeisoCompositionEdgeAppPumpResult result;
    app->mutable_active = true;
    status = meiso_composition_debug_peer_pump(
        app->peer,
        ready->control_readable && !app_source_work_outstanding(app),
        ready->control_writable,
        &peer_result
    );
    app->mutable_active = false;
    if (!refresh_phase(app)) {
        enter_terminal(
            app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE, 0
        );
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    if (app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_TERMINAL;
    }
    if (status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TX_BUSY) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_WOULD_BLOCK;
    }
    if (status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
    }
    if (status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT) {
        if (!peer_result.event_present || !handle_runtime_event(
                app, &peer_result.event, ready->now, &result
            )) {
            enter_terminal(
                app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE, 0
            );
            return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
        }
        *out_result = result;
        return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
    }
    if (status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_RX_OBLIGATION ||
        status > MEISO_COMPOSITION_DEBUG_PEER_PUMP_SOURCE_INVALID) {
        enter_terminal(
            app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE, 0
        );
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    memset(&result, 0, sizeof(result));
    result.event_present = true;
    result.event = MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS;
    *out_result = result;
    return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
}

static bool media_route_config(
    const MeisoCompositionEdgeApp *app,
    const MeisoCompositionDebugPeerSnapshot *peer,
    size_t media_index,
    MeisoCompositionMediaRouteConfig *out_route
)
{
    const size_t actual_index = media_index == 0U ?
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA;
    const size_t route_index = media_index == 0U ?
        ROUTE_RENDER_MEDIA_DATA : ROUTE_CAMERA_MEDIA_DATA;
    const MeisoCompositionMediaActual *media = media_index == 0U ?
        &app->render_media_actual : &app->camera_media_actual;
    const MeisoCompositionMediaCapability *capability = media_index == 0U ?
        &app->catalog.render_media_capability :
        &app->catalog.camera_media_capability;
    const EdgeActualChannel *actual = &app->actual[actual_index];
    const MeisoRuntimeRouteDescriptor *descriptor =
        &app->catalog_routes[route_index].route;
    MeisoCompositionMediaRouteConfig route;
    if (out_route == NULL || !actual->snapshot.present ||
        !actual->snapshot.effective_enabled ||
        actual->snapshot.availability != MEISO_RUNTIME_AVAILABILITY_PRESENT ||
        actual->snapshot.production_status !=
            MEISO_RUNTIME_PRODUCTION_RUNNING ||
        actual->snapshot.production_epoch == 0U ||
        media->media_stream_epoch == 0U ||
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
    route.binding.production_epoch = actual->snapshot.production_epoch;
    route.binding.media_stream_epoch = media->media_stream_epoch;
    route.binding.channel_nonce = media->channel_nonce;
    route.capability = *capability;
    route.effective = media->effective;
    *out_route = route;
    return true;
}

static bool media_actual_running(
    const MeisoCompositionEdgeApp *app,
    size_t media_index
)
{
    const size_t actual_index = media_index == 0U ?
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA;
    const MeisoCompositionMediaActual *media = media_index == 0U ?
        &app->render_media_actual : &app->camera_media_actual;
    const EdgeActualChannel *actual = &app->actual[actual_index];
    return actual->snapshot.present && actual->snapshot.effective_enabled &&
        actual->snapshot.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
        actual->snapshot.production_status ==
            MEISO_RUNTIME_PRODUCTION_RUNNING &&
        actual->snapshot.production_epoch != 0U &&
        media->media_stream_epoch == actual->snapshot.production_epoch &&
        meiso_session_nonce_valid(media->channel_nonce);
}

static bool build_media_config(
    const MeisoCompositionEdgeApp *app,
    bool require_delivery_accepted,
    MeisoCompositionMediaTransportConfig *out_config
)
{
    MeisoCompositionDebugPeerSnapshot peer;
    MeisoCompositionMediaTransportConfig config;
    size_t index;
    uint8_t intended_mask = 0U;
    if (out_config == NULL ||
        !meiso_composition_debug_peer_snapshot(app->peer, &peer) ||
        peer.phase != MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY ||
        peer.runtime.active_catalog_generation != app->config.catalog_generation ||
        peer.session.state != MEISO_SESSION_STATE_READY ||
        !peer.session.session_id_known) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_EDGE;
    config.scope.session_id = peer.session.session_id;
    config.scope.catalog_generation = app->config.catalog_generation;
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if (media_actual_running(app, index)) {
            intended_mask = (uint8_t)(intended_mask | bit);
        }
    }
    if (intended_mask == 0U) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        const size_t actual_index = index == 0U ?
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA :
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA;
        if ((intended_mask & bit) != 0U) {
            if ((require_delivery_accepted &&
                 (!app->actual[actual_index].snapshot.delivery_accepted ||
                  app->actual[actual_index].snapshot.delivery_pending)) ||
                !media_route_config(
                    app, &peer, index, &config.routes[index]
                )) {
                return false;
            }
        } else {
            config.routes[index].capability = index == 0U ?
                app->catalog.render_media_capability :
                app->catalog.camera_media_capability;
        }
    }
    config.active_route_mask = intended_mask;
    *out_config = config;
    return true;
}

static bool media_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        meiso_endpoint_id_equal(lhs->endpoint_id, rhs->endpoint_id) &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        meiso_resource_id_equal(lhs->resource_id, rhs->resource_id) &&
        lhs->source == rhs->source &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

static bool active_media_scope_current(MeisoCompositionEdgeApp *app)
{
    MeisoCompositionMediaTransportConfig current;
    size_t index;
    if (app->active_media == NULL ||
        !build_media_config(app, false, &current) ||
        current.active_route_mask != app->active_media->active_route_mask ||
        !meiso_session_id_equal(
            current.scope.session_id,
            app->active_media->transport_config.scope.session_id
        ) || current.scope.catalog_generation !=
            app->active_media->transport_config.scope.catalog_generation) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_MEDIA_ROUTE_COUNT;
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
    MeisoCompositionEdgeApp *app,
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

static bool fail_active_media_production(
    MeisoCompositionEdgeApp *app,
    bool unavailable
)
{
    uint8_t failed_mask;
    size_t media_index;
    if (app == NULL || app->active_media == NULL) {
        return false;
    }
    failed_mask = app->active_media->active_route_mask;
    terminate_active_media(
        app, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST, 0
    );
    if ((failed_mask & MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA) != 0U) {
        if (!stop_camera_bounded(app, false)) {
            enter_terminal(
                app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_CAMERA_CLEANUP, 0
            );
            return false;
        }
        app->camera_frame_pending = false;
        app->camera_frame_submitted = false;
        memset(&app->camera_frame, 0, sizeof(app->camera_frame));
    }
    for (media_index = 0U;
         media_index < MEISO_COMPOSITION_EDGE_APP_MEDIA_ROUTE_COUNT;
         ++media_index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << media_index);
        const size_t actual_index = media_index == 0U ?
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA :
            MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA;
        EdgeActualChannel *actual;
        if ((failed_mask & bit) == 0U) {
            continue;
        }
        actual = &app->actual[actual_index];
        actual->snapshot.apply_result = MEISO_RUNTIME_APPLY_FAILED;
        actual->snapshot.availability = unavailable ?
            MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE :
            MEISO_RUNTIME_AVAILABILITY_PRESENT;
        actual->snapshot.effective_enabled = false;
        actual->snapshot.production_status =
            MEISO_RUNTIME_PRODUCTION_FAILED;
        actual->snapshot.reason = unavailable ?
            MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE :
            MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED;
        actual->snapshot.production_epoch = 0U;
        if (media_index == 0U) {
            app->render_media_actual_present = false;
            app->render_media_sequence = 0U;
            cadence_reset(&app->render_media_cadence);
        } else {
            app->camera_media_actual_present = false;
            app->camera_media_sequence = 0U;
            cadence_reset(&app->camera_media_cadence);
        }
        if (!publish_actual(app, actual_index)) {
            return false;
        }
    }
    return true;
}

MeisoCompositionEdgeAppStatus meiso_composition_edge_app_bind_media_fd(
    MeisoCompositionEdgeApp *app,
    int connected_fd,
    MeisoCompositionEdgeAppMediaBindReceipt *out_receipt
)
{
    MeisoCompositionMediaTransportConfig transport_config;
    MeisoCompositionMediaPosixTcpConfig adapter_config;
    EdgeMediaSlot *slot;
    EdgeMediaSlot slot_before;
    MeisoCompositionEdgeAppMediaBindReceipt receipt;
    uint8_t *slot_bytes;
    size_t transport_bytes;
    size_t transport_alignment;
    size_t adapter_bytes;
    if (!app_valid(app) || out_receipt == NULL ||
        app_range_overlap(app, out_receipt, sizeof(*out_receipt)) ||
        connected_fd == app->config.control_connected_fd) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_ARGUMENT;
    }
    if (app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_EDGE_APP_TERMINAL;
    }
    if (app->mutable_active) {
        return MEISO_COMPOSITION_EDGE_APP_BUSY;
    }
    if (app->active_media != NULL) {
        return MEISO_COMPOSITION_EDGE_APP_BUSY;
    }
    if (app->media_slots_consumed >= app->config.media_connection_slots) {
        return MEISO_COMPOSITION_EDGE_APP_CAPACITY;
    }
    if (!build_media_config(app, true, &transport_config)) {
        return MEISO_COMPOSITION_EDGE_APP_NOT_READY;
    }
    if (!fd_preflight(connected_fd) || !fd_prepare_send_safety(connected_fd)) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_ARGUMENT;
    }
    if (meiso_composition_media_transport_memory_required(
            &transport_config, &transport_bytes, &transport_alignment
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        transport_bytes > app->media_transport_bytes ||
        transport_alignment > alignof(max_align_t)) {
        return MEISO_COMPOSITION_EDGE_APP_CAPACITY;
    }
    slot = &app->media_slots[app->media_slots_consumed];
    slot_before = *slot;
    slot_bytes = app->arena_begin + slot->offset;
    if (meiso_composition_media_transport_init(
            &transport_config,
            (MeisoMutableBuffer){slot_bytes, app->media_transport_bytes},
            &slot->transport
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        memset(slot_bytes, 0, app->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_EDGE_APP_INTERNAL_FAILURE;
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
        adapter_bytes > EDGE_MEDIA_ADAPTER_RESERVE_BYTES ||
        meiso_composition_media_posix_tcp_init(
            slot_bytes + app->media_transport_bytes,
            EDGE_MEDIA_ADAPTER_RESERVE_BYTES, &adapter_config,
            &slot->adapter
        ) != MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK) {
        (void)meiso_composition_media_transport_terminate(
            slot->transport, &transport_config.scope,
            MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST
        );
        memset(slot_bytes, 0, app->media_slot_bytes);
        *slot = slot_before;
        return adapter_bytes == 0U ||
                adapter_bytes > EDGE_MEDIA_ADAPTER_RESERVE_BYTES ?
            MEISO_COMPOSITION_EDGE_APP_CAPACITY :
            MEISO_COMPOSITION_EDGE_APP_INTERNAL_FAILURE;
    }
    slot->consumed = true;
    slot->fd = connected_fd;
    slot->active_route_mask = transport_config.active_route_mask;
    slot->transport_config = transport_config;
    ++app->media_slots_consumed;
    app->active_media = slot;
    memset(&receipt, 0, sizeof(receipt));
    receipt.slot_index = (uint8_t)(slot - app->media_slots);
    receipt.active_route_mask = transport_config.active_route_mask;
    receipt.session_id = transport_config.scope.session_id;
    receipt.catalog_generation = transport_config.scope.catalog_generation;
    *out_receipt = receipt;
    return MEISO_COMPOSITION_EDGE_APP_OK;
}

MeisoCompositionEdgeAppStatus meiso_composition_edge_app_unbind_media_fd(
    MeisoCompositionEdgeApp *app,
    int error_number
)
{
    (void)error_number;
    if (!app_valid(app)) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_ARGUMENT;
    }
    if (app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_EDGE_APP_TERMINAL;
    }
    if (app->mutable_active) {
        return MEISO_COMPOSITION_EDGE_APP_BUSY;
    }
    if (app->active_media == NULL) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_STATE;
    }
    terminate_active_media(
        app, MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE, error_number
    );
    return MEISO_COMPOSITION_EDGE_APP_OK;
}

MeisoCompositionEdgeAppStatus
meiso_composition_edge_app_take_thermal_request(
    MeisoCompositionEdgeApp *app,
    MeisoCompositionEdgeAppThermalRequest *out_request
)
{
    if (!app_valid(app) || out_request == NULL ||
        app_range_overlap(app, out_request, sizeof(*out_request))) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_ARGUMENT;
    }
    if (!app->thermal_request_pending || app->thermal_request_dispatched) {
        return MEISO_COMPOSITION_EDGE_APP_NO_WORK;
    }
    *out_request = app->thermal_request;
    app->thermal_request_dispatched = true;
    return MEISO_COMPOSITION_EDGE_APP_OK;
}

MeisoCompositionEdgeAppThermalReadStatus
meiso_composition_edge_app_run_thermal_request(
    const MeisoCompositionEdgeAppThermalRequest *request,
    MeisoCompositionEdgeAppThermalResult *out_result
)
{
    MeisoCompositionEdgeAppThermalResult value;
    MeisoDeviceTelemetryLinuxAdapterStatus status;
    if (request == NULL || out_result == NULL ||
        ranges_overlap(request, sizeof(*request), out_result,
                       sizeof(*out_result)) || request->request_id == 0U) {
        return MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_INVALID_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.request_id = request->request_id;
    value.observed_desired = request->observed_desired;
    value.production_epoch = request->production_epoch;
    status = meiso_device_telemetry_linux_adapter_read_temperature_direct(
        &request->reader, &value.temperature
    );
    if (status == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK) {
        value.read_status = MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_READY;
    } else if (status == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE) {
        value.read_status = MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_UNAVAILABLE;
    } else {
        return MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_INVALID_ARGUMENT;
    }
    *out_result = value;
    return value.read_status;
}

MeisoCompositionEdgeAppStatus
meiso_composition_edge_app_submit_thermal_result(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionEdgeAppThermalResult *result,
    MeisoTimePoint completed_at
)
{
    if (!app_valid(app) || result == NULL ||
        app_range_overlap(app, result, sizeof(*result)) ||
        !meiso_time_point_valid(completed_at)) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_ARGUMENT;
    }
    if (!app->thermal_request_pending ||
        result->request_id != app->thermal_request.request_id ||
        !desired_reference_equal(
            result->observed_desired,
            app->thermal_request.observed_desired
        ) ||
        result->production_epoch != app->thermal_request.production_epoch ||
        (result->read_status !=
             MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_READY &&
         result->read_status !=
             MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_UNAVAILABLE) ||
        (result->read_status ==
             MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_UNAVAILABLE &&
         (result->temperature.present ||
          result->temperature.temperature_millic != 0 ||
          result->temperature.uncertainty_millic != 0U)) ||
        completed_at.clock_epoch != app->thermal_request.deadline.clock_epoch ||
        completed_at.monotonic_ns >
            app->thermal_request.deadline.monotonic_ns) {
        return MEISO_COMPOSITION_EDGE_APP_STALE;
    }
    app->thermal_result = *result;
    app->thermal_result_present = true;
    return MEISO_COMPOSITION_EDGE_APP_OK;
}

static MeisoCompositionEdgeAppPumpStatus pump_media(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionEdgeAppIoReady *ready,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoCompositionMediaPosixTcpReadStatus read_status;
    MeisoCompositionMediaPosixTcpWriteStatus write_status;
    MeisoCompositionMediaPosixTcpPrepareStatus prepare_status;
    MeisoCompositionMediaPosixTcpSnapshot before;
    if (app->active_media == NULL) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
    }
    if (app_source_work_outstanding(app)) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
    }
    if (!active_media_scope_current(app)) {
        terminate_active_media(
            app, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
        return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
    }
    if (ready->media_readable) {
        read_status = meiso_composition_media_posix_tcp_on_readable(
            app->active_media->adapter
        );
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK ||
            read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_WOULD_BLOCK;
        }
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST ||
            read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INVALID_ARGUMENT) {
            if (!fail_active_media_production(
                    app,
                    read_status ==
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST
                )) {
                return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
            return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_MEDIA_PROGRESS;
        return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
    }
    if (!meiso_composition_media_posix_tcp_snapshot(
            app->active_media->adapter, &before
        )) {
        (void)fail_active_media_production(app, false);
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    prepare_status = meiso_composition_media_posix_tcp_prepare_outbound(
        app->active_media->adapter
    );
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TRANSPORT_INVALID ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_INVALID_ARGUMENT) {
        if (!fail_active_media_production(app, false)) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
        return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
    }
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY) {
        if (!ready->media_writable) {
            if (!before.tx_packet_borrowed) {
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_COMPOSITION_EDGE_APP_PUMP_MEDIA_PROGRESS;
                return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
            }
            return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
        }
        write_status = meiso_composition_media_posix_tcp_on_writable(
            app->active_media->adapter
        );
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED) {
            return MEISO_COMPOSITION_EDGE_APP_PUMP_WOULD_BLOCK;
        }
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INVALID_ARGUMENT) {
            if (!fail_active_media_production(
                    app,
                    write_status ==
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST
                )) {
                return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_BOUNDARY;
            return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_EDGE_APP_PUMP_MEDIA_PROGRESS;
        return MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS;
    }
    return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
}

MeisoCompositionEdgeAppPumpStatus meiso_composition_edge_app_pump(
    MeisoCompositionEdgeApp *app,
    const MeisoCompositionEdgeAppIoReady *ready,
    MeisoCompositionEdgeAppPumpResult *out_result
)
{
    MeisoCompositionEdgeAppPumpResult result;
    MeisoCompositionEdgeAppPumpStatus status;
    uint8_t scan;
    if (!app_valid(app) || ready == NULL || out_result == NULL ||
        app_range_overlap(app, ready, sizeof(*ready)) ||
        app_range_overlap(app, out_result, sizeof(*out_result)) ||
        ranges_overlap(ready, sizeof(*ready), out_result,
                       sizeof(*out_result))) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INVALID_ARGUMENT;
    }
    if (app->mutable_active || !meiso_time_point_valid(ready->now) ||
        ready->now.clock_epoch != app->config.edge_clock_epoch) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INVALID_ARGUMENT;
    }
    if (app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_EDGE_APP_PUMP_TERMINAL;
    }
    if (!refresh_phase(app)) {
        enter_terminal(
            app, MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE, 0
        );
        return MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE;
    }
    for (scan = 0U; scan < 3U; ++scan) {
        const uint8_t domain = (uint8_t)((app->fairness_cursor + scan) % 3U);
        status = domain == 0U ? pump_local(app, ready, &result) :
            domain == 1U ? pump_media(app, ready, &result) :
                           pump_control(app, ready, &result);
        if (status == MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS) {
            app->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            *out_result = result;
            return status;
        }
        if (status == MEISO_COMPOSITION_EDGE_APP_PUMP_TERMINAL ||
            status == MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE) {
            if (status == MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE &&
                app->phase != MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
                enter_terminal(
                    app,
                    MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE,
                    0
                );
            }
            return status;
        }
        if (status == MEISO_COMPOSITION_EDGE_APP_PUMP_WOULD_BLOCK) {
            app->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            return status;
        }
    }
    return MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE;
}

bool meiso_composition_edge_app_readiness(
    const MeisoCompositionEdgeApp *app,
    MeisoCompositionEdgeAppIoInterest *out_interest
)
{
    MeisoCompositionEdgeAppIoInterest value;
    if (!app_valid(app) || out_interest == NULL ||
        app_range_overlap(app, out_interest, sizeof(*out_interest))) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    if (app->phase != MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
        value.control_read = !app_source_work_outstanding(app) &&
            meiso_composition_debug_peer_wants_read(app->peer);
        value.control_write =
            meiso_composition_debug_peer_wants_write(app->peer);
    }
    if (app->active_media != NULL) {
        value.media_read = meiso_composition_media_posix_tcp_wants_read(
            app->active_media->adapter
        );
        value.media_write = meiso_composition_media_posix_tcp_wants_write(
            app->active_media->adapter
        );
    }
    value.camera_poll_useful =
        app->camera_media_actual_present && app->active_media != NULL &&
        (app->active_media->active_route_mask &
         MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA) != 0U;
    value.thermal_request_ready = app->thermal_request_pending &&
        !app->thermal_request_dispatched;
    *out_interest = value;
    return true;
}

bool meiso_composition_edge_app_snapshot(
    const MeisoCompositionEdgeApp *app,
    MeisoCompositionEdgeAppSnapshot *out_snapshot
)
{
    MeisoCompositionEdgeAppSnapshot value;
    MeisoCompositionDebugPeerSnapshot peer;
    MeisoBlobStats blob_stats;
    size_t index;
    if (!app_valid(app) || out_snapshot == NULL ||
        app_range_overlap(app, out_snapshot, sizeof(*out_snapshot)) ||
        !meiso_composition_debug_peer_snapshot(app->peer, &peer) ||
        !meiso_blob_store_stats(app->blob_store, &blob_stats)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.phase = app->phase;
    value.terminal_reason = app->terminal_reason;
    value.control_session_state = peer.session.state;
    value.control_session_id = peer.session.session_id;
    value.active_catalog_generation = peer.runtime.active_catalog_generation;
    value.control_ready =
        peer.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY;
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_APP_ACTUAL_COUNT;
         ++index) {
        value.actual[index] = app->actual[index].snapshot;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_EDGE_APP_INLINE_STREAM_COUNT; ++index) {
        value.inline_streams[index] = app->inline_streams[index].snapshot;
    }
    value.wifi_effective_present = app->wifi_effective_present;
    value.wifi_effective = app->wifi_effective;
    value.wifi_sample_present = app->wifi_sample_present;
    value.wifi_sample = app->wifi_sample;
    value.device_effective_present = app->device_effective_present;
    value.device_effective = app->device_effective;
    value.device_sample_present = app->device_sample_present;
    value.device_sample = app->device_sample;
    value.render_media_actual_present = app->render_media_actual_present;
    value.render_media_actual = app->render_media_actual;
    value.camera_media_actual_present = app->camera_media_actual_present;
    value.camera_media_actual = app->camera_media_actual;
    value.presentation_status_present = app->presentation_status_present;
    value.presentation_status = app->presentation_status;
    value.scene_status_present = app->scene_status_present;
    value.scene_status = app->scene_status;
    (void)meiso_composition_scene_store_control_scope(
        app->scene_store, &value.scene_control_scope
    );
    value.active_world_present = meiso_composition_scene_store_active(
        app->scene_store, &(MeisoCompositionSceneWorldView){0}
    );
    if (value.active_world_present) {
        MeisoCompositionSceneWorldView world;
        if (meiso_composition_scene_store_active(app->scene_store, &world)) {
            value.active_world = world.ref;
        }
    }
    (void)meiso_composition_cpu_raster_renderer_snapshot(
        app->renderer, &value.renderer
    );
    (void)meiso_camera_v4l2_snapshot(app->camera, &value.camera);
    if (app->active_media != NULL) {
        MeisoCompositionMediaTransportSnapshot transport;
        value.media.bound = true;
        value.media.active_route_mask = app->active_media->active_route_mask;
        value.media.slot_index = (uint8_t)(app->active_media - app->media_slots);
        if (meiso_composition_media_transport_snapshot(
                app->active_media->transport, &transport
            ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            value.media.transport_phase = transport.phase;
            value.media.render_submitted_sequence =
                transport.routes[0].submitted_sequence_high_water;
            value.media.camera_submitted_sequence =
                transport.routes[1].submitted_sequence_high_water;
            value.media.render_whole_frames_dropped =
                transport.routes[0].transport_whole_frames_dropped;
            value.media.camera_whole_frames_dropped =
                transport.routes[1].transport_whole_frames_dropped;
        }
    }
    value.blob_published_count = (uint32_t)meiso_blob_store_count(
        app->blob_store
    );
    value.blob_published_bytes = app->blob_published_bytes;
    value.blob_transfer_id_high_water = app->blob_transfer_id_high_water;
    value.blob_transfer = app->blob_result_pending ?
        app->blob_result_transfer : app->blob_receive_transfer;
    value.blob_next_offset = app->blob_receive_next_offset;
    value.blob_result_operation = app->blob_result_operation;
    value.blob_transfer_active = app->blob_receive_active;
    value.blob_begin_ready = app->blob_receive_begin_ready;
    value.blob_result_pending = app->blob_result_pending;
    value.blob_result_queued = app->blob_result_queued;
    value.next_thermal_request_id = app->next_thermal_request_id;
    value.thermal_request_pending = app->thermal_request_pending;
    value.media_slots_consumed = app->media_slots_consumed;
    value.media_slots_capacity = app->config.media_connection_slots;
    value.arena_bytes = app->arena_bytes;
    value.control_peer_bytes = app->control_peer_bytes;
    value.blob_store_bytes = app->blob_store_bytes;
    value.scene_store_bytes = app->scene_store_bytes;
    value.coordinator_bytes = app->coordinator_bytes;
    value.renderer_bytes = app->renderer_bytes;
    value.camera_bytes = app->camera_bytes;
    value.core_state_bytes = app->core_state_bytes;
    value.core_stream_bytes = app->core_stream_bytes;
    value.media_slot_bytes = app->media_slot_bytes;
    *out_snapshot = value;
    return true;
}

MeisoCompositionEdgeAppStatus meiso_composition_edge_app_abort(
    MeisoCompositionEdgeApp *app,
    MeisoCompositionEdgeAppTerminalReason reason,
    int error_number
)
{
    if (!app_valid(app) || reason == MEISO_COMPOSITION_EDGE_APP_TERMINAL_NONE ||
        reason > MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_COMPOSITION_EDGE_APP_INVALID_ARGUMENT;
    }
    if (app->phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_EDGE_APP_TERMINAL;
    }
    enter_terminal(app, reason, error_number);
    return MEISO_COMPOSITION_EDGE_APP_OK;
}
