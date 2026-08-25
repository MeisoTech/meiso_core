#include "composition_virtual_edge_host.h"
#include "composition_virtual_edge_host_internal.h"
#include "composition_coordinator.h"
#include "composition_cpu_raster_renderer.h"
#include "meiso_composition_edge_simulator.h"
#include "scene_store.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define EDGE_HANDLE_MAGIC UINT64_C(0x4d4549534f454447)
#define EDGE_SUPPORTED_OPERATIONS                                             \
    (MEISO_EDGE_OPERATION_STATE_READ | MEISO_EDGE_OPERATION_STATE_SUBMIT |   \
     MEISO_EDGE_OPERATION_STREAM_READ | MEISO_EDGE_OPERATION_BLOB_PUT |      \
     MEISO_EDGE_OPERATION_COMPOSITION | MEISO_EDGE_OPERATION_MEDIA_READ)
#define SIMULATOR_SUPPORTED_OPERATIONS                                        \
    (MEISO_EDGE_OPERATION_STATE_READ | MEISO_EDGE_OPERATION_STATE_SUBMIT |   \
     MEISO_EDGE_OPERATION_STREAM_READ | MEISO_EDGE_OPERATION_MEDIA_READ)

enum {
    EDGE_BACKEND_HOST = 1,
    EDGE_BACKEND_SIMULATOR = 2
};

_Static_assert(MEISO_COMPOSITION_EDGE_DESIRED_COUNT ==
                   MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT,
               "desired channel count drift");
_Static_assert(MEISO_COMPOSITION_EDGE_ACTUAL_COUNT ==
                   MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT,
               "actual channel count drift");
_Static_assert(MEISO_COMPOSITION_EDGE_INLINE_STREAM_COUNT ==
                   MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT,
               "inline channel count drift");
_Static_assert(MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT ==
                   MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT,
               "media route count drift");
_Static_assert(MEISO_EDGE_COMMAND_PUT_MESH ==
                   MEISO_COMPOSITION_HOST_APP_COMMAND_PUT_MESH &&
                   MEISO_EDGE_COMMAND_SET_SCENE ==
                       MEISO_COMPOSITION_HOST_APP_COMMAND_SET_SCENE &&
                   MEISO_EDGE_COMMAND_SET_PRESENTATION ==
                       MEISO_COMPOSITION_HOST_APP_COMMAND_SET_PRESENTATION &&
                   MEISO_EDGE_COMMAND_SET_WIFI ==
                       MEISO_COMPOSITION_HOST_APP_COMMAND_SET_WIFI &&
                   MEISO_EDGE_COMMAND_SET_DEVICE ==
                       MEISO_COMPOSITION_HOST_APP_COMMAND_SET_DEVICE &&
                   MEISO_EDGE_COMMAND_SET_RENDER_MEDIA ==
                       MEISO_COMPOSITION_HOST_APP_COMMAND_SET_RENDER_MEDIA &&
                   MEISO_EDGE_COMMAND_SET_CAMERA_MEDIA ==
                       MEISO_COMPOSITION_HOST_APP_COMMAND_SET_CAMERA_MEDIA,
               "command mapping drift");
_Static_assert(MEISO_EDGE_LOCAL_QUEUED ==
                   MEISO_COMPOSITION_HOST_APP_LOCAL_QUEUED &&
                   MEISO_EDGE_LOCAL_REPLACED_PENDING ==
                       MEISO_COMPOSITION_HOST_APP_LOCAL_REPLACED_PENDING,
               "local receipt mapping drift");
_Static_assert(MEISO_COMPOSITION_EDGE_DESIRED_WIFI ==
                   MEISO_COMPOSITION_HOST_APP_DESIRED_WIFI &&
                   MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION ==
                       MEISO_COMPOSITION_HOST_APP_DESIRED_PRESENTATION &&
                   MEISO_COMPOSITION_EDGE_DESIRED_SCENE ==
                       MEISO_COMPOSITION_HOST_APP_DESIRED_SCENE &&
                   MEISO_COMPOSITION_EDGE_DESIRED_DEVICE ==
                       MEISO_COMPOSITION_HOST_APP_DESIRED_DEVICE &&
                   MEISO_COMPOSITION_EDGE_DESIRED_RENDER_MEDIA ==
                       MEISO_COMPOSITION_HOST_APP_DESIRED_RENDER_MEDIA &&
                   MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA ==
                       MEISO_COMPOSITION_HOST_APP_DESIRED_CAMERA_MEDIA,
               "desired channel mapping drift");
_Static_assert(MEISO_COMPOSITION_EDGE_ACTUAL_WIFI ==
                   MEISO_COMPOSITION_HOST_APP_ACTUAL_WIFI &&
                   MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION ==
                       MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION &&
                   MEISO_COMPOSITION_EDGE_ACTUAL_SCENE ==
                       MEISO_COMPOSITION_HOST_APP_ACTUAL_SCENE &&
                   MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE ==
                       MEISO_COMPOSITION_HOST_APP_ACTUAL_DEVICE &&
                   MEISO_COMPOSITION_EDGE_ACTUAL_RENDER_MEDIA ==
                       MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA &&
                   MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA ==
                       MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA,
               "actual channel mapping drift");
_Static_assert(MEISO_COMPOSITION_EDGE_INLINE_WIFI ==
                   MEISO_COMPOSITION_HOST_APP_INLINE_WIFI &&
                   MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT ==
                       MEISO_COMPOSITION_HOST_APP_INLINE_RENDER_RESULT &&
                   MEISO_COMPOSITION_EDGE_INLINE_DEVICE ==
                       MEISO_COMPOSITION_HOST_APP_INLINE_DEVICE,
               "inline channel mapping drift");

typedef struct {
    bool active;
    MeisoCompositionMediaFrameRef ref;
} MediaBorrow;

typedef struct {
    bool pending;
    bool enabled;
    MeisoCompositionMediaDesired desired;
} SimulatorMediaPending;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    bool present;
    bool borrowed;
    MeisoEdgeMediaFrameView frame;
} SimulatorMediaRoute;

typedef struct {
    bool pending;
    bool enabled;
    MeisoCompositionSceneDesired value;
} SimulatorScenePending;

typedef struct {
    bool pending;
    bool enabled;
    MeisoCompositionEdgePresentationSettings settings;
    MeisoCompositionScenePresentationPlan plan;
} SimulatorPresentationPending;

typedef struct {
    size_t required;
    size_t render_offset;
    size_t camera_offset;
    size_t mesh_offset;
    size_t blob_offset;
    size_t blob_size;
    size_t scene_offset;
    size_t scene_size;
    size_t coordinator_offset;
    size_t coordinator_size;
    size_t renderer_offset;
    size_t renderer_size;
} SimulatorLayout;

struct MeisoEdgeHandle {
    uint64_t magic;
    uint8_t backend_kind;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t host_arena_begin;
    uintptr_t host_arena_end;
    MeisoCompositionHostApp *host_app;
    uint64_t catalog_generation;
    uint64_t continuity_serial;
    MeisoProfileRef profile;
    uint8_t dynamic_capability[
        MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES
    ];
    MeisoRuntimeEndpointDescriptor
        endpoints[MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT];
    MeisoCompositionSessionV3RouteDescriptor
        routes[MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT];
    MediaBorrow media_borrows[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    MeisoCompositionSessionV3CatalogConfig simulator_catalog;
    MeisoCompositionEdgeSnapshot simulator_snapshot;
    MeisoOwnerScope simulator_desired_owner;
    uint64_t simulator_next_command_id;
    uint64_t simulator_wifi_epoch;
    bool simulator_wifi_epoch_available;
    uint64_t simulator_media_epoch[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    bool simulator_media_epoch_available[
        MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT
    ];
    bool simulator_wifi_pending;
    bool simulator_wifi_enabled;
    MeisoWifiTelemetryDesiredParameters simulator_wifi_desired;
    MeisoWifiTelemetrySample simulator_wifi_sample;
    bool simulator_device_supported;
    uint64_t simulator_device_epoch;
    bool simulator_device_epoch_available;
    bool simulator_device_pending;
    bool simulator_device_enabled;
    MeisoDeviceTelemetryParameters simulator_device_desired;
    MeisoDeviceTelemetrySample simulator_device_sample;
    SimulatorMediaPending
        simulator_media_pending[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    SimulatorMediaRoute
        simulator_media[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    bool simulator_composition_supported;
    bool simulator_composition_initialized;
    uint32_t simulator_blob_slot_count;
    uint64_t simulator_composition_clock_epoch;
    uint64_t simulator_presentation_epoch;
    bool simulator_presentation_epoch_available;
    uint64_t simulator_scene_epoch;
    bool simulator_scene_epoch_available;
    uint64_t simulator_next_plan_generation;
    bool simulator_plan_generation_available;
    uint64_t simulator_published_blob_bytes;
    uint64_t simulator_render_result_sequence;
    uint8_t *simulator_mesh_buffer;
    size_t simulator_mesh_buffer_capacity;
    bool simulator_mesh_pending;
    size_t simulator_mesh_size;
    MeisoContentId simulator_mesh_content_id;
    void *simulator_blob_arena;
    size_t simulator_blob_arena_size;
    MeisoBlobStore *simulator_blob_store;
    void *simulator_scene_arena;
    size_t simulator_scene_arena_size;
    MeisoCompositionSceneStore *simulator_scene_store;
    void *simulator_coordinator_arena;
    size_t simulator_coordinator_arena_size;
    MeisoCompositionCoordinator *simulator_coordinator;
    bool simulator_cpu_raster_enabled;
    void *simulator_renderer_arena;
    size_t simulator_renderer_arena_size;
    MeisoCompositionCpuRasterRenderer *simulator_renderer;
    bool simulator_raster_preview_pending;
    SimulatorScenePending simulator_scene_pending;
    bool simulator_scene_retry_pending;
    bool simulator_scene_retry_ready;
    bool simulator_scene_value_present;
    bool simulator_scene_enabled;
    MeisoCompositionSceneDesired simulator_scene_value;
    SimulatorPresentationPending simulator_presentation_pending;
    bool simulator_presentation_plan_present;
    bool simulator_presentation_activation_pending;
    MeisoCompositionScenePresentationPlan simulator_presentation_plan;
    bool simulator_render_result_pending;
};

static bool checked_range(
    const void *pointer,
    size_t size,
    uintptr_t *out_begin,
    uintptr_t *out_end
)
{
    uintptr_t begin;

    if (pointer == NULL || size == 0U || out_begin == NULL || out_end == NULL) {
        return false;
    }
    begin = (uintptr_t)pointer;
    if (begin > UINTPTR_MAX - size) {
        return false;
    }
    *out_begin = begin;
    *out_end = begin + size;
    return true;
}

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool checked_align_size(size_t value, size_t alignment, size_t *out)
{
    size_t remainder;
    size_t padding;

    if (alignment == 0U || out == NULL) {
        return false;
    }
    remainder = value % alignment;
    padding = remainder == 0U ? 0U : alignment - remainder;
    return checked_add_size(value, padding, out);
}

static bool simulator_layout(
    const MeisoCompositionEdgeSimulatorConfig *config,
    SimulatorLayout *out_layout
)
{
    const MeisoResourceDescriptor *blob_descriptor;
    MeisoBlobConfig blob_config;
    SimulatorLayout layout;
    size_t cursor;

    if (config == NULL || out_layout == NULL) {
        return false;
    }
    memset(&layout, 0, sizeof(layout));
    cursor = sizeof(MeisoEdgeHandle);
    layout.render_offset = cursor;
    if (!checked_add_size(
            cursor,
            config->catalog.render_media_capability.max_access_unit_bytes,
            &cursor
        )) {
        return false;
    }
    layout.camera_offset = cursor;
    if (!checked_add_size(
            cursor,
            config->catalog.camera_media_capability.max_access_unit_bytes,
            &cursor
        )) {
        return false;
    }
    if (!config->composition_enabled) {
        if (config->cpu_raster_preview_enabled) {
            return false;
        }
        layout.required = cursor;
        *out_layout = layout;
        return true;
    }
    if (config->composition_blob_slot_count == 0U ||
        config->composition_blob_slot_count >
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS) {
        return false;
    }
    layout.mesh_offset = cursor;
    if (!checked_add_size(
            cursor, MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE, &cursor
        )) {
        return false;
    }
    blob_descriptor = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    blob_config.published_slot_count = config->composition_blob_slot_count;
    layout.blob_size = meiso_blob_store_memory_required(
        blob_descriptor, &blob_config
    );
    layout.scene_size = meiso_composition_scene_store_memory_required();
    layout.coordinator_size =
        meiso_composition_coordinator_memory_required();
    if (blob_descriptor == NULL || layout.blob_size == 0U ||
        layout.scene_size == 0U || layout.coordinator_size == 0U ||
        !checked_align_size(
            cursor, meiso_blob_alignment_required(), &layout.blob_offset
        ) ||
        !checked_add_size(
            layout.blob_offset, layout.blob_size, &cursor
        ) ||
        !checked_align_size(
            cursor, meiso_composition_scene_store_alignment_required(),
            &layout.scene_offset
        ) ||
        !checked_add_size(
            layout.scene_offset, layout.scene_size, &cursor
        ) ||
        !checked_align_size(
            cursor, meiso_composition_coordinator_alignment_required(),
            &layout.coordinator_offset
        ) ||
        !checked_add_size(
            layout.coordinator_offset, layout.coordinator_size, &cursor
        )) {
        return false;
    }
    if (config->cpu_raster_preview_enabled) {
        layout.renderer_size =
            meiso_composition_cpu_raster_renderer_memory_required();
        if (layout.renderer_size == 0U ||
            !checked_align_size(
                cursor,
                meiso_composition_cpu_raster_renderer_alignment_required(),
                &layout.renderer_offset
            ) ||
            !checked_add_size(
                layout.renderer_offset, layout.renderer_size, &cursor
            )) {
            return false;
        }
    }
    layout.required = cursor;
    *out_layout = layout;
    return true;
}

static bool ranges_overlap(
    uintptr_t lhs_begin,
    uintptr_t lhs_end,
    uintptr_t rhs_begin,
    uintptr_t rhs_end
)
{
    return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

static bool range_inside(
    uintptr_t outer_begin,
    uintptr_t outer_end,
    uintptr_t inner_begin,
    uintptr_t inner_end
)
{
    return inner_begin >= outer_begin && inner_end <= outer_end;
}

static bool handle_valid(const MeisoEdgeHandle *handle)
{
    uintptr_t app_begin;
    uintptr_t app_end;

    if (handle == NULL || handle->magic != EDGE_HANDLE_MAGIC ||
        (handle->backend_kind != EDGE_BACKEND_HOST &&
         handle->backend_kind != EDGE_BACKEND_SIMULATOR)) {
        return false;
    }
    if (handle->arena_begin != (uintptr_t)handle ||
        handle->arena_end <= handle->arena_begin ||
        handle->arena_end - handle->arena_begin < sizeof(*handle) ||
        handle->catalog_generation == 0U ||
        handle->continuity_serial == 0U) {
        return false;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        return handle->host_app == NULL && handle->host_arena_begin == 0U &&
            handle->host_arena_end == 0U;
    }
    return handle->host_arena_end > handle->host_arena_begin &&
        handle->host_app != NULL &&
        checked_range(handle->host_app, 1U, &app_begin, &app_end) &&
        range_inside(handle->host_arena_begin, handle->host_arena_end,
                     app_begin, app_end);
}

static bool disjoint_from_handle_storage(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
)
{
    uintptr_t begin;
    uintptr_t end;

    return checked_range(pointer, size, &begin, &end) &&
        !ranges_overlap(begin, end, handle->arena_begin, handle->arena_end) &&
        (handle->backend_kind != EDGE_BACKEND_HOST ||
         !ranges_overlap(begin, end, handle->host_arena_begin,
                         handle->host_arena_end));
}

static bool optional_input_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
)
{
    if (pointer == NULL && size == 0U) {
        return true;
    }
    return disjoint_from_handle_storage(handle, pointer, size);
}

static bool disjoint_pair(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    uintptr_t lhs_begin;
    uintptr_t lhs_end;
    uintptr_t rhs_begin;
    uintptr_t rhs_end;

    if ((lhs == NULL && lhs_size == 0U) || (rhs == NULL && rhs_size == 0U)) {
        return true;
    }
    return checked_range(lhs, lhs_size, &lhs_begin, &lhs_end) &&
        checked_range(rhs, rhs_size, &rhs_begin, &rhs_end) &&
        !ranges_overlap(lhs_begin, lhs_end, rhs_begin, rhs_end);
}

static MeisoEdgeTerminalReason terminal_reason_from_host(
    MeisoCompositionHostAppTerminalReason reason
)
{
    switch (reason) {
    case MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_TRANSPORT:
        return MEISO_EDGE_TERMINAL_TRANSPORT;
    case MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_SESSION:
        return MEISO_EDGE_TERMINAL_SESSION;
    case MEISO_COMPOSITION_HOST_APP_TERMINAL_CATALOG_CHANGED:
        return MEISO_EDGE_TERMINAL_CATALOG_DISCONTINUITY;
    case MEISO_COMPOSITION_HOST_APP_TERMINAL_LOCAL_ABORT:
        return MEISO_EDGE_TERMINAL_LOCAL_ABORT;
    case MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE:
        return MEISO_EDGE_TERMINAL_INTERNAL_FAILURE;
    case MEISO_COMPOSITION_HOST_APP_TERMINAL_NONE:
    default:
        return MEISO_EDGE_TERMINAL_INTERNAL_FAILURE;
    }
}

static bool base_snapshot_from_host(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionHostAppSnapshot *host,
    MeisoEdgeSnapshot *out
)
{
    bool catalog_mismatch;

    if (handle == NULL || host == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->api_version = MEISO_VIRTUAL_EDGE_API_VERSION;
    out->profile = handle->profile;
    out->continuity_serial = handle->continuity_serial;
    catalog_mismatch = host->control_ready &&
        host->active_catalog_generation != handle->catalog_generation;
    if (host->phase == MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL ||
        catalog_mismatch) {
        out->phase = MEISO_EDGE_PHASE_TERMINAL;
        out->terminal_reason = catalog_mismatch ?
            MEISO_EDGE_TERMINAL_CATALOG_DISCONTINUITY :
            terminal_reason_from_host(host->terminal_reason);
        return true;
    }
    out->online = host->control_session_state == MEISO_SESSION_STATE_READY;
    if (host->control_ready &&
        host->active_catalog_generation == handle->catalog_generation) {
        out->phase = MEISO_EDGE_PHASE_READY;
        out->resource_graph_live = true;
        out->catalog_generation = handle->catalog_generation;
        out->operations = EDGE_SUPPORTED_OPERATIONS;
        out->endpoint_count = MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT;
        out->resource_count = MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT;
    } else {
        out->phase = MEISO_EDGE_PHASE_BOOTSTRAPPING;
    }
    return true;
}

static bool read_host_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionHostAppSnapshot *out_host,
    MeisoEdgeSnapshot *out_edge
)
{
    return handle_valid(handle) && out_host != NULL && out_edge != NULL &&
        meiso_composition_host_app_snapshot(handle->host_app, out_host) &&
        base_snapshot_from_host(handle, out_host, out_edge);
}

static MeisoEdgeStatus command_preflight(
    const MeisoEdgeHandle *handle,
    MeisoCompositionHostAppSnapshot *out_host
)
{
    MeisoEdgeSnapshot edge;

    if (!handle_valid(handle) || out_host == NULL) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_host_app_snapshot(handle->host_app, out_host) ||
        !base_snapshot_from_host(handle, out_host, &edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus status_from_host(MeisoCompositionHostAppStatus status)
{
    switch (status) {
    case MEISO_COMPOSITION_HOST_APP_OK:
        return MEISO_EDGE_OK;
    case MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT:
        return MEISO_EDGE_INVALID_ARGUMENT;
    case MEISO_COMPOSITION_HOST_APP_INVALID_STATE:
    case MEISO_COMPOSITION_HOST_APP_FRAME_MISMATCH:
        return MEISO_EDGE_STALE;
    case MEISO_COMPOSITION_HOST_APP_CAPACITY:
        return MEISO_EDGE_CAPACITY;
    case MEISO_COMPOSITION_HOST_APP_BUSY:
        return MEISO_EDGE_BUSY;
    case MEISO_COMPOSITION_HOST_APP_CONFLICT:
        return MEISO_EDGE_CONFLICT;
    case MEISO_COMPOSITION_HOST_APP_NOT_READY:
        return MEISO_EDGE_NOT_READY;
    case MEISO_COMPOSITION_HOST_APP_NO_FRAME:
        return MEISO_EDGE_NO_DATA;
    case MEISO_COMPOSITION_HOST_APP_COUNTER_EXHAUSTED:
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    case MEISO_COMPOSITION_HOST_APP_TERMINAL:
        return MEISO_EDGE_TERMINAL;
    case MEISO_COMPOSITION_HOST_APP_INTERNAL_FAILURE:
    default:
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
}

static bool receipt_from_host(
    const MeisoCompositionHostAppCommandReceipt *host,
    MeisoEdgeCommandReceipt *out
)
{
    if (host == NULL || out == NULL ||
        host->kind < MEISO_COMPOSITION_HOST_APP_COMMAND_PUT_MESH ||
        host->kind > MEISO_COMPOSITION_HOST_APP_COMMAND_SET_CAMERA_MEDIA ||
        (host->local_result != MEISO_COMPOSITION_HOST_APP_LOCAL_QUEUED &&
         host->local_result !=
             MEISO_COMPOSITION_HOST_APP_LOCAL_REPLACED_PENDING) ||
        (host->desired_state_present &&
         host->desired_channel >= MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->command_id = host->command_id;
    out->kind = host->kind;
    out->local_result = host->local_result;
    out->replaced_command_present = host->replaced_command_present;
    out->replaced_command_id = host->replaced_command_id;
    out->desired_state_present = host->desired_state_present;
    out->desired_channel = host->desired_channel;
    out->desired_state_version = host->desired_state_version;
    out->plan_generation_present = host->plan_generation_present;
    out->plan_generation = host->plan_generation;
    out->mesh_content_id_present = host->mesh_content_id_present;
    out->mesh_content_id = host->mesh_content_id;
    return true;
}

static bool binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
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

static int media_index(MeisoCompositionMediaSource source)
{
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return 0;
    }
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return 1;
    }
    return -1;
}

static void refresh_media_borrows(
    MeisoEdgeHandle *handle,
    const MeisoCompositionHostAppSnapshot *host,
    const MeisoEdgeSnapshot *edge
)
{
    size_t i;

    if (handle == NULL || host == NULL || edge == NULL) {
        return;
    }
    if (!edge->resource_graph_live) {
        memset(handle->media_borrows, 0, sizeof(handle->media_borrows));
        return;
    }
    for (i = 0U; i < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT; ++i) {
        MediaBorrow *borrow = &handle->media_borrows[i];
        const MeisoCompositionHostAppMediaRouteSnapshot *route =
            &host->media[i];
        if (borrow->active &&
            (!route->latest_present ||
             route->latest_frame_sequence != borrow->ref.frame_sequence ||
             !binding_equal(&route->binding, &borrow->ref.binding))) {
            memset(borrow, 0, sizeof(*borrow));
        }
    }
}

size_t meiso_composition_virtual_edge_host_alignment_required(void)
{
    return _Alignof(MeisoEdgeHandle);
}

size_t meiso_composition_virtual_edge_host_memory_required(void)
{
    return sizeof(MeisoEdgeHandle);
}

MeisoCompositionVirtualEdgeHostInitStatus
meiso_composition_virtual_edge_host_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionVirtualEdgeHostConfig *config,
    MeisoEdgeHandle **out_handle
)
{
    MeisoEdgeHandle candidate;
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot edge;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t owned_arena_end;
    uintptr_t host_begin;
    uintptr_t host_end;
    uintptr_t config_begin;
    uintptr_t config_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    uintptr_t app_begin;
    uintptr_t app_end;
    size_t i;

    if (arena == NULL || config == NULL || out_handle == NULL ||
        config->host_app == NULL || config->host_app_arena == NULL ||
        config->host_app_arena_size == 0U ||
        config->catalog_generation == 0U ||
        config->continuity_serial == 0U ||
        !checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !checked_range(config->host_app_arena,
                       config->host_app_arena_size, &host_begin, &host_end) ||
        !checked_range(config, sizeof(*config), &config_begin, &config_end) ||
        !checked_range(out_handle, sizeof(*out_handle), &output_begin,
                       &output_end) ||
        !checked_range(config->host_app, 1U, &app_begin, &app_end) ||
        !range_inside(host_begin, host_end, app_begin, app_end) ||
        ranges_overlap(host_begin, host_end, config_begin, config_end) ||
        ranges_overlap(host_begin, host_end, output_begin, output_end) ||
        ranges_overlap(config_begin, config_end, output_begin, output_end)) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INVALID_ARGUMENT;
    }
    if (((uintptr_t)arena % _Alignof(MeisoEdgeHandle)) != 0U) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(MeisoEdgeHandle)) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_ARENA_TOO_SMALL;
    }
    owned_arena_end = arena_begin + sizeof(MeisoEdgeHandle);
    if (ranges_overlap(arena_begin, owned_arena_end, host_begin, host_end) ||
        ranges_overlap(arena_begin, owned_arena_end, config_begin,
                       config_end) ||
        ranges_overlap(arena_begin, owned_arena_end, output_begin,
                       output_end)) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INVALID_ARGUMENT;
    }

    memset(&candidate, 0, sizeof(candidate));
    if (meiso_composition_session_v3_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){
                candidate.dynamic_capability,
                sizeof(candidate.dynamic_capability)
            },
            candidate.endpoints,
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            candidate.routes,
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) != MEISO_COMPOSITION_SESSION_V3_OK) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INVALID_CONFIG;
    }
    if (!meiso_composition_host_app_snapshot(config->host_app, &host)) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INTERNAL_FAILURE;
    }
    candidate.magic = EDGE_HANDLE_MAGIC;
    candidate.backend_kind = EDGE_BACKEND_HOST;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = owned_arena_end;
    candidate.host_arena_begin = host_begin;
    candidate.host_arena_end = host_end;
    candidate.host_app = config->host_app;
    candidate.catalog_generation = config->catalog_generation;
    candidate.continuity_serial = config->continuity_serial;
    candidate.profile = *meiso_composition_scene_profile_v3();
    if (!base_snapshot_from_host(&candidate, &host, &edge)) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INTERNAL_FAILURE;
    }
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_BACKEND_TERMINAL;
    }
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT; ++i) {
        const uintptr_t capability_begin =
            (uintptr_t)candidate.endpoints[i].capability.data;
        const uintptr_t dynamic_begin =
            (uintptr_t)candidate.dynamic_capability;
        const uintptr_t dynamic_end =
            dynamic_begin + sizeof(candidate.dynamic_capability);
        if (candidate.endpoints[i].capability.size != 0U &&
            capability_begin >= dynamic_begin && capability_begin < dynamic_end) {
            const size_t offset = (size_t)(capability_begin - dynamic_begin);
            if (offset > sizeof(candidate.dynamic_capability) ||
                candidate.endpoints[i].capability.size >
                    sizeof(candidate.dynamic_capability) - offset) {
                return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_INTERNAL_FAILURE;
            }
            candidate.endpoints[i].capability.data =
                (const uint8_t *)arena +
                offsetof(MeisoEdgeHandle, dynamic_capability) + offset;
        }
    }
    memset(arena, 0, sizeof(candidate));
    memmove(arena, &candidate, sizeof(candidate));
    *out_handle = (MeisoEdgeHandle *)arena;
    return MEISO_COMPOSITION_VIRTUAL_EDGE_HOST_INIT_OK;
}

MeisoEdgeStatus meiso_edge_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoEdgeSnapshot *out_snapshot
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot candidate;

    if (!handle_valid(handle) || out_snapshot == NULL ||
        !disjoint_from_handle_storage(handle, out_snapshot,
                                      sizeof(*out_snapshot))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        candidate = handle->simulator_snapshot.edge;
    } else if (!read_host_snapshot(handle, &host, &candidate)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    memmove(out_snapshot, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_edge_operation_supported(
    const MeisoEdgeHandle *handle,
    MeisoEdgeOperationMask operation,
    bool *out_supported
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot edge;
    bool candidate;

    if (!handle_valid(handle) || out_supported == NULL || operation == 0U ||
        (operation & MEISO_EDGE_OPERATION_ALL) != operation ||
        (operation & (operation - 1U)) != 0U ||
        !disjoint_from_handle_storage(handle, out_supported,
                                      sizeof(*out_supported))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        edge = handle->simulator_snapshot.edge;
    } else if (!read_host_snapshot(handle, &host, &edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    candidate = (edge.operations & operation) != 0U;
    *out_supported = candidate;
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_edge_endpoint_at(
    const MeisoEdgeHandle *handle,
    size_t index,
    MeisoEdgeEndpointView *out_endpoint
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot edge;
    MeisoEdgeEndpointView candidate;

    if (!handle_valid(handle) || out_endpoint == NULL ||
        !disjoint_from_handle_storage(handle, out_endpoint,
                                      sizeof(*out_endpoint))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        edge = handle->simulator_snapshot.edge;
    } else if (!read_host_snapshot(handle, &host, &edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    if (index >= MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT) {
        return MEISO_EDGE_CAPACITY;
    }
    candidate.id = handle->endpoints[index].id;
    candidate.incarnation = handle->endpoints[index].incarnation;
    candidate.capability_schema = handle->endpoints[index].capability_schema;
    candidate.capability = handle->endpoints[index].capability;
    memmove(out_endpoint, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_edge_resource_at(
    const MeisoEdgeHandle *handle,
    size_t index,
    MeisoEdgeResourceView *out_resource
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot edge;
    MeisoEdgeResourceView candidate;
    const MeisoCompositionSessionV3RouteDescriptor *route;

    if (!handle_valid(handle) || out_resource == NULL ||
        !disjoint_from_handle_storage(handle, out_resource,
                                      sizeof(*out_resource))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        edge = handle->simulator_snapshot.edge;
    } else if (!read_host_snapshot(handle, &host, &edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    if (index >= MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT) {
        return MEISO_EDGE_CAPACITY;
    }
    route = &handle->routes[index];
    memset(&candidate, 0, sizeof(candidate));
    candidate.endpoint_id = route->route.endpoint_id;
    candidate.endpoint_incarnation = route->route.endpoint_incarnation;
    candidate.resource = route->route.resource;
    if (route->route.facet == MEISO_RUNTIME_FACET_DESIRED ||
        route->route.facet == MEISO_RUNTIME_FACET_IMMUTABLE) {
        candidate.access = MEISO_EDGE_RESOURCE_HOST_SUBMIT;
    } else if (route->route.facet == MEISO_RUNTIME_FACET_ACTUAL ||
               route->route.facet == MEISO_RUNTIME_FACET_DATA) {
        candidate.access = MEISO_EDGE_RESOURCE_EDGE_READ;
    } else {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (route->delivery != MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME &&
        route->delivery != MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    candidate.payload_path =
        route->delivery == MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA &&
            route->route.resource.kind == MEISO_RESOURCE_KIND_STREAM ?
        MEISO_EDGE_PAYLOAD_EXTERNAL_MEDIA : MEISO_EDGE_PAYLOAD_INLINE;
    memmove(out_resource, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus simulator_command_preflight(
    const MeisoEdgeHandle *handle
)
{
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (handle->simulator_next_command_id == UINT64_MAX) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus simulator_queue_desired(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    MeisoEdgeCommandKind kind,
    bool enabled,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionEdgeDesiredView *desired;
    MeisoEdgeCommandReceipt receipt;
    uint64_t next_version;
    MeisoEdgeStatus status = simulator_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    desired = &handle->simulator_snapshot.desired[channel];
    if (desired->state_version == UINT64_MAX ||
        (desired->delivery_pending &&
         desired->replaced_pending_count == UINT64_MAX)) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    next_version = desired->state_version + 1U;
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = ++handle->simulator_next_command_id;
    receipt.kind = kind;
    receipt.local_result = desired->delivery_pending ?
        MEISO_EDGE_LOCAL_REPLACED_PENDING : MEISO_EDGE_LOCAL_QUEUED;
    receipt.replaced_command_present = desired->delivery_pending;
    receipt.replaced_command_id = desired->delivery_pending ?
        desired->command_id : 0U;
    receipt.desired_state_present = true;
    receipt.desired_channel = channel;
    receipt.desired_state_version = next_version;
    *desired = (MeisoCompositionEdgeDesiredView){
        true,
        enabled,
        receipt.command_id,
        handle->simulator_desired_owner,
        next_version,
        false,
        true,
        false,
        false,
        desired->replaced_pending_count +
            (receipt.replaced_command_present ? 1U : 0U),
        false,
        0U,
        {0},
        0U,
        false,
        false,
        false
    };
    memmove(out_receipt, &receipt, sizeof(receipt));
    return MEISO_EDGE_OK;
}

static void simulator_apply_actual_state(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    bool enabled,
    bool running,
    uint64_t production_epoch
)
{
    MeisoCompositionEdgeDesiredView *desired =
        &handle->simulator_snapshot.desired[channel];
    MeisoCompositionEdgeActualView *actual =
        &handle->simulator_snapshot.actual[channel];
    MeisoRuntimeDesiredReference reference = {
        true, desired->owner, desired->state_version
    };

    desired->delivery_pending = false;
    desired->delivery_accepted = true;
    *actual = (MeisoCompositionEdgeActualView){
        true,
        desired->owner,
        desired->state_version,
        reference,
        reference,
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        enabled && running,
        enabled ? (running ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                         MEISO_RUNTIME_PRODUCTION_STARTING) :
            MEISO_RUNTIME_PRODUCTION_STOPPED,
        enabled ? (running ? MEISO_RUNTIME_PRODUCTION_REASON_NONE :
                         MEISO_RUNTIME_PRODUCTION_REASON_APPLYING) :
            MEISO_RUNTIME_PRODUCTION_REASON_DISABLED,
        enabled && running ? production_epoch : 0U,
        true
    };
}

static void simulator_apply_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    uint64_t production_epoch
)
{
    const bool enabled =
        handle->simulator_snapshot.desired[channel].enabled;

    simulator_apply_actual_state(
        handle, channel, enabled, enabled, production_epoch
    );
}

static void simulator_reject_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    MeisoRuntimeProductionReason reason
)
{
    MeisoCompositionEdgeDesiredView *desired =
        &handle->simulator_snapshot.desired[channel];
    MeisoCompositionEdgeActualView *actual =
        &handle->simulator_snapshot.actual[channel];

    desired->delivery_pending = false;
    desired->delivery_accepted = true;
    if (!actual->present) {
        memset(actual, 0, sizeof(*actual));
        actual->present = true;
        actual->availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        actual->production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    }
    actual->state_owner = desired->owner;
    actual->state_version = desired->state_version;
    actual->observed_desired = (MeisoRuntimeDesiredReference){
        true, desired->owner, desired->state_version
    };
    actual->apply_result = MEISO_RUNTIME_APPLY_REJECTED;
    actual->reason = reason;
    actual->profile_decoded = true;
}

static MeisoEdgeStatus finish_command(
    MeisoCompositionHostAppStatus host_status,
    const MeisoCompositionHostAppCommandReceipt *host_receipt,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeCommandReceipt candidate;
    MeisoEdgeStatus status = status_from_host(host_status);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!receipt_from_host(host_receipt, &candidate)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    memmove(out_receipt, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus simulator_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoContentId content_id;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status = simulator_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_composition_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if (handle->simulator_mesh_pending) {
        return MEISO_EDGE_BUSY;
    }
    if (canonical_mesh.size == 0U ||
        canonical_mesh.size > handle->simulator_mesh_buffer_capacity) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_scene_mesh_content_id(
            canonical_mesh, &content_id
        )) {
        return MEISO_EDGE_CONFLICT;
    }
    memmove(handle->simulator_mesh_buffer, canonical_mesh.data,
            canonical_mesh.size);
    handle->simulator_mesh_pending = true;
    handle->simulator_mesh_size = canonical_mesh.size;
    handle->simulator_mesh_content_id = content_id;
    memset(&handle->simulator_snapshot.mesh, 0,
           sizeof(handle->simulator_snapshot.mesh));
    handle->simulator_snapshot.mesh.queued = true;
    handle->simulator_snapshot.mesh.command_id =
        ++handle->simulator_next_command_id;
    handle->simulator_snapshot.mesh.content_id = content_id;
    handle->simulator_snapshot.mesh.total_bytes = canonical_mesh.size;
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = handle->simulator_snapshot.mesh.command_id;
    receipt.kind = MEISO_EDGE_COMMAND_PUT_MESH;
    receipt.local_result = MEISO_EDGE_LOCAL_QUEUED;
    receipt.mesh_content_id_present = true;
    receipt.mesh_content_id = content_id;
    memmove(out_receipt, &receipt, sizeof(receipt));
    return MEISO_EDGE_OK;
}

static bool simulator_scene_value_valid(
    const MeisoCompositionSceneDesired *value
)
{
    uint8_t wire[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    size_t wire_size = 0U;

    return value != NULL &&
        meiso_composition_scene_desired_encode(
            value, (MeisoMutableBuffer){wire, sizeof(wire)}, &wire_size
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK && wire_size != 0U;
}

static MeisoEdgeStatus simulator_set_scene(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeStatus status = simulator_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_composition_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if (enabled != (value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !simulator_scene_value_valid(value)) {
        return MEISO_EDGE_CONFLICT;
    }
    if (enabled && !handle->simulator_scene_epoch_available) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = simulator_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
        MEISO_EDGE_COMMAND_SET_SCENE, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_scene_pending.pending = true;
        handle->simulator_scene_pending.enabled = enabled;
        handle->simulator_scene_pending.value = enabled ? *value :
            (MeisoCompositionSceneDesired){0};
    }
    return status;
}

static bool simulator_presentation_plan(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgePresentationSettings *settings,
    uint64_t generation,
    MeisoCompositionScenePresentationPlan *out_plan
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    if (handle == NULL || settings == NULL || out_plan == NULL ||
        generation == 0U) {
        return false;
    }
    memset(&plan, 0, sizeof(plan));
    plan.flags = settings->flags;
    plan.plan_generation = generation;
    plan.capability_generation = handle->catalog_generation;
    plan.scene_binding = *meiso_composition_scene_scene_endpoint();
    plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    plan.target_width = settings->target_width;
    plan.target_height = settings->target_height;
    plan.color_format = settings->color_format;
    plan.depth_format = settings->depth_format;
    plan.max_result_hz = settings->max_result_hz;
    plan.offline_lease_ms = settings->offline_lease_ms;
    if (!meiso_composition_coordinator_plan_digest(&plan, digest)) {
        return false;
    }
    memmove(plan.candidate_digest, digest, sizeof(digest));
    *out_plan = plan;
    return true;
}

static MeisoEdgeStatus simulator_set_presentation(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionEdgePresentationSettings *settings,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint64_t generation = 0U;
    MeisoEdgeStatus status = simulator_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_composition_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if ((enabled && settings == NULL) || (!enabled && settings != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    memset(&plan, 0, sizeof(plan));
    if (enabled) {
        if (!handle->simulator_presentation_epoch_available ||
            !handle->simulator_scene_epoch_available ||
            !handle->simulator_plan_generation_available ||
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
            ].state_version >= UINT64_MAX - 1U) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        generation = handle->simulator_next_plan_generation;
        if (!simulator_presentation_plan(
                handle, settings, generation, &plan
            )) {
            return MEISO_EDGE_CONFLICT;
        }
    }
    status = simulator_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
        MEISO_EDGE_COMMAND_SET_PRESENTATION, enabled, out_receipt
    );
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (enabled) {
        out_receipt->plan_generation_present = true;
        out_receipt->plan_generation = generation;
        if (generation == UINT64_MAX) {
            handle->simulator_plan_generation_available = false;
        } else {
            ++handle->simulator_next_plan_generation;
        }
    }
    handle->simulator_presentation_pending.pending = true;
    handle->simulator_presentation_pending.enabled = enabled;
    handle->simulator_presentation_pending.settings = enabled ? *settings :
        (MeisoCompositionEdgePresentationSettings){0};
    handle->simulator_presentation_pending.plan = plan;
    handle->simulator_presentation_activation_pending = false;
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoEdgeStatus preflight;

    if (!handle_valid(handle) || out_receipt == NULL ||
        !meiso_byte_view_valid(canonical_mesh) ||
        !optional_input_disjoint(handle, canonical_mesh.data,
                                 canonical_mesh.size) ||
        !disjoint_from_handle_storage(handle, out_receipt,
                                      sizeof(*out_receipt)) ||
        !disjoint_pair(canonical_mesh.data, canonical_mesh.size, out_receipt,
                       sizeof(*out_receipt))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        return simulator_put_mesh(handle, canonical_mesh, out_receipt);
    }
    preflight = command_preflight(handle, &host);
    if (preflight != MEISO_EDGE_OK) {
        return preflight;
    }
    return finish_command(meiso_composition_host_app_put_mesh(
                              handle->host_app, canonical_mesh, &receipt),
                          &receipt, out_receipt);
}

static bool typed_command_ranges_valid(
    const MeisoEdgeHandle *handle,
    const void *input,
    size_t input_size,
    const MeisoEdgeCommandReceipt *out_receipt
)
{
    return handle_valid(handle) && out_receipt != NULL &&
        optional_input_disjoint(handle, input, input_size) &&
        disjoint_from_handle_storage(handle, out_receipt,
                                     sizeof(*out_receipt)) &&
        disjoint_pair(input, input_size, out_receipt, sizeof(*out_receipt));
}

MeisoEdgeStatus meiso_composition_edge_set_scene(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoEdgeStatus preflight;
    size_t input_size = value == NULL ? 0U : sizeof(*value);

    if (!typed_command_ranges_valid(handle, value, input_size, out_receipt)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        return simulator_set_scene(handle, enabled, value, out_receipt);
    }
    preflight = command_preflight(handle, &host);
    if (preflight != MEISO_EDGE_OK) {
        return preflight;
    }
    return finish_command(meiso_composition_host_app_set_scene(
                              handle->host_app, enabled, value, &receipt),
                          &receipt, out_receipt);
}

MeisoEdgeStatus meiso_composition_edge_set_presentation(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionEdgePresentationSettings *settings,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoCompositionHostAppPresentationSettings mapped;
    const MeisoCompositionHostAppPresentationSettings *mapped_pointer = NULL;
    MeisoEdgeStatus preflight;
    size_t input_size = settings == NULL ? 0U : sizeof(*settings);

    if (!typed_command_ranges_valid(handle, settings, input_size,
                                    out_receipt)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        return simulator_set_presentation(
            handle, enabled, settings, out_receipt
        );
    }
    if (settings != NULL) {
        mapped = (MeisoCompositionHostAppPresentationSettings){
            settings->flags,
            settings->target_width,
            settings->target_height,
            settings->color_format,
            settings->depth_format,
            settings->max_result_hz,
            settings->offline_lease_ms
        };
        mapped_pointer = &mapped;
    }
    preflight = command_preflight(handle, &host);
    if (preflight != MEISO_EDGE_OK) {
        return preflight;
    }
    return finish_command(meiso_composition_host_app_set_presentation(
                              handle->host_app, enabled, mapped_pointer,
                              &receipt),
                          &receipt, out_receipt);
}

static MeisoEdgeStatus simulator_set_wifi(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeStatus status;

    if ((enabled && (value == NULL ||
                     !meiso_wifi_telemetry_parameters_supported(
                         &handle->simulator_catalog.wifi_capability, value))) ||
        (!enabled && value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !handle->simulator_wifi_epoch_available) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = simulator_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_WIFI,
        MEISO_EDGE_COMMAND_SET_WIFI, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_wifi_pending = true;
        handle->simulator_wifi_enabled = enabled;
        handle->simulator_wifi_desired = enabled ? *value :
            (MeisoWifiTelemetryDesiredParameters){0U, 0U};
    }
    return status;
}

static MeisoEdgeStatus simulator_set_media(
    MeisoEdgeHandle *handle,
    size_t index,
    bool enabled,
    const MeisoCompositionMediaDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    const MeisoCompositionMediaCapability *capability = index == 0U ?
        &handle->simulator_catalog.render_media_capability :
        &handle->simulator_catalog.camera_media_capability;
    MeisoCompositionEdgeDesiredChannel channel = index == 0U ?
        MEISO_COMPOSITION_EDGE_DESIRED_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA;
    MeisoEdgeCommandKind kind = index == 0U ?
        MEISO_EDGE_COMMAND_SET_RENDER_MEDIA :
        MEISO_EDGE_COMMAND_SET_CAMERA_MEDIA;
    MeisoEdgeStatus status;

    if ((enabled && (value == NULL ||
                     !meiso_composition_media_desired_supported(
                         capability, value))) || (!enabled && value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !handle->simulator_media_epoch_available[index]) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = simulator_queue_desired(
        handle, channel, kind, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_media_pending[index].pending = true;
        handle->simulator_media_pending[index].enabled = enabled;
        handle->simulator_media_pending[index].desired = enabled ? *value :
            (MeisoCompositionMediaDesired){0};
    }
    return status;
}

static MeisoEdgeStatus simulator_set_device(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoDeviceTelemetryParameters *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeStatus status;

    status = simulator_command_preflight(handle);
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_device_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if ((enabled &&
         (value == NULL ||
          !meiso_device_telemetry_parameters_supported(
              &handle->simulator_catalog.device_telemetry_capability, value
          ))) ||
        (!enabled && value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !handle->simulator_device_epoch_available) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = simulator_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_DEVICE,
        MEISO_EDGE_COMMAND_SET_DEVICE, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_device_pending = true;
        handle->simulator_device_enabled = enabled;
        handle->simulator_device_desired = enabled ? *value :
            (MeisoDeviceTelemetryParameters){0U, 0U};
    }
    return status;
}

#define DEFINE_TYPED_COMMAND(function_name, host_function, value_type, sim_body) \
MeisoEdgeStatus function_name(                                                  \
    MeisoEdgeHandle *handle, bool enabled, const value_type *value,             \
    MeisoEdgeCommandReceipt *out_receipt                                        \
)                                                                               \
{                                                                               \
    MeisoCompositionHostAppSnapshot host;                                       \
    MeisoCompositionHostAppCommandReceipt receipt;                              \
    MeisoEdgeStatus preflight;                                                  \
    const size_t input_size = value == NULL ? 0U : sizeof(*value);              \
    if (!typed_command_ranges_valid(handle, value, input_size, out_receipt)) {   \
        return MEISO_EDGE_INVALID_ARGUMENT;                                     \
    }                                                                           \
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {                       \
        sim_body;                                                               \
    }                                                                           \
    preflight = command_preflight(handle, &host);                               \
    if (preflight != MEISO_EDGE_OK) {                                           \
        return preflight;                                                       \
    }                                                                           \
    return finish_command(host_function(handle->host_app, enabled, value,       \
                                        &receipt),                              \
                          &receipt, out_receipt);                               \
}

DEFINE_TYPED_COMMAND(meiso_composition_edge_set_wifi,
                     meiso_composition_host_app_set_wifi,
                     MeisoWifiTelemetryDesiredParameters,
                     return simulator_set_wifi(handle, enabled, value,
                                               out_receipt))
DEFINE_TYPED_COMMAND(meiso_composition_edge_set_device,
                     meiso_composition_host_app_set_device,
                     MeisoDeviceTelemetryParameters,
                     return simulator_set_device(handle, enabled, value,
                                                 out_receipt))
DEFINE_TYPED_COMMAND(meiso_composition_edge_set_render_media,
                     meiso_composition_host_app_set_render_media,
                     MeisoCompositionMediaDesired,
                     return simulator_set_media(handle, 0U, enabled, value,
                                                out_receipt))
DEFINE_TYPED_COMMAND(meiso_composition_edge_set_camera_media,
                     meiso_composition_host_app_set_camera_media,
                     MeisoCompositionMediaDesired,
                     return simulator_set_media(handle, 1U, enabled, value,
                                                out_receipt))

#undef DEFINE_TYPED_COMMAND

static void copy_desired(
    const MeisoCompositionHostAppDesiredSnapshot *source,
    MeisoCompositionEdgeDesiredView *destination
)
{
    *destination = (MeisoCompositionEdgeDesiredView){
        source->requested,
        source->enabled,
        source->command_id,
        source->owner,
        source->state_version,
        source->delivery_active,
        source->delivery_pending,
        source->delivery_accepted,
        source->delivery_failed,
        source->replaced_pending_count,
        source->inflight_present,
        source->inflight_command_id,
        source->inflight_owner,
        source->inflight_state_version,
        source->inflight_source_completed,
        source->inflight_offer_accepted,
        source->inflight_actual_observed
    };
}

static void copy_actual(
    const MeisoCompositionHostAppActualSnapshot *source,
    MeisoCompositionEdgeActualView *destination
)
{
    *destination = (MeisoCompositionEdgeActualView){
        source->present,
        source->state_owner,
        source->state_version,
        source->observed_desired,
        source->applied_desired,
        source->apply_result,
        source->availability,
        source->effective_enabled,
        source->production_status,
        source->reason,
        source->production_epoch,
        source->profile_decoded
    };
}

static void copy_inline(
    const MeisoCompositionHostAppInlineSnapshot *source,
    MeisoCompositionEdgeInlineView *destination
)
{
    *destination = (MeisoCompositionEdgeInlineView){
        source->present,
        source->key,
        source->source_time,
        source->receive_flags,
        source->missing_before,
        source->received_count,
        source->replaced_count
    };
}

static void copy_media_actual(
    const MeisoCompositionMediaActual *source,
    MeisoCompositionEdgeMediaActual *destination
)
{
    *destination = (MeisoCompositionEdgeMediaActual){
        source->source,
        source->effective,
        source->media_stream_epoch,
        source->producer_dropped_frames,
        source->queue_depth,
        source->recovery_generation
    };
}

MeisoEdgeStatus meiso_composition_edge_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoCompositionEdgeSnapshot candidate;
    size_t i;

    if (!handle_valid(handle) || out_snapshot == NULL ||
        !disjoint_from_handle_storage(handle, out_snapshot,
                                      sizeof(*out_snapshot))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        memmove(out_snapshot, &handle->simulator_snapshot,
                sizeof(*out_snapshot));
        return MEISO_EDGE_OK;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (!meiso_composition_host_app_snapshot(handle->host_app, &host) ||
        !base_snapshot_from_host(handle, &host, &candidate.edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (candidate.edge.phase != MEISO_EDGE_PHASE_TERMINAL) {
        for (i = 0U; i < MEISO_COMPOSITION_EDGE_DESIRED_COUNT; ++i) {
            copy_desired(&host.desired[i], &candidate.desired[i]);
        }
        candidate.mesh = (MeisoCompositionEdgeMeshView){
            host.mesh.queued,
            host.mesh.transfer_active,
            host.mesh.begin_ready,
            host.mesh.waiting_result,
            host.mesh.terminal_result_present,
            host.mesh.command_id,
            host.mesh.content_id,
            host.mesh.total_bytes,
            host.mesh.next_offset,
            host.mesh.terminal_result,
            host.mesh.edge_core_published
        };
    }
    if (candidate.edge.resource_graph_live) {
        for (i = 0U; i < MEISO_COMPOSITION_EDGE_ACTUAL_COUNT; ++i) {
            copy_actual(&host.actual[i], &candidate.actual[i]);
        }
        for (i = 0U; i < MEISO_COMPOSITION_EDGE_INLINE_STREAM_COUNT; ++i) {
            copy_inline(&host.inline_streams[i],
                        &candidate.inline_streams[i]);
        }
        for (i = 0U; i < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT; ++i) {
            const MeisoCompositionHostAppMediaRouteSnapshot *route =
                &host.media[i];
            candidate.media[i] = (MeisoCompositionEdgeMediaRouteView){
                route->actual_binding_ready,
                route->latest_present,
                route->source,
                route->latest_frame_sequence,
                route->latest_source_time,
                route->latest_access_unit_bytes,
                route->published_sequence_high_water,
                route->sequence_gap_frames,
                route->producer_drop_high_water,
                route->reassembly_frames_dropped,
                route->unread_latest_frames_dropped
            };
        }
        candidate.wifi_effective_present = host.wifi_effective_present;
        candidate.wifi_effective = host.wifi_effective;
        candidate.wifi_sample_present = host.wifi_sample_present;
        candidate.wifi_sample = host.wifi_sample;
        candidate.presentation_status_present =
            host.presentation_status_present;
        candidate.presentation_status =
            (MeisoCompositionEdgePresentationStatus){
                host.presentation_status.capability_generation,
                host.presentation_status.candidate_state,
                host.presentation_status.candidate_plan_generation,
                {0},
                host.presentation_status.active_state,
                host.presentation_status.active_plan_generation,
                {0},
                host.presentation_status.reason,
                host.presentation_status.target_width,
                host.presentation_status.target_height,
                host.presentation_status.color_format,
                host.presentation_status.depth_format
            };
        memmove(candidate.presentation_status.candidate_digest,
                host.presentation_status.candidate_digest,
                MEISO_SHA256_DIGEST_SIZE);
        memmove(candidate.presentation_status.active_digest,
                host.presentation_status.active_digest,
                MEISO_SHA256_DIGEST_SIZE);
        candidate.scene_status_present = host.scene_status_present;
        candidate.scene_status = host.scene_status;
        candidate.render_result_present = host.render_result_present;
        candidate.render_result = host.render_result;
        candidate.device_effective_present = host.device_effective_present;
        candidate.device_effective = host.device_effective;
        candidate.device_sample_present = host.device_sample_present;
        candidate.device_sample = host.device_sample;
        candidate.render_media_actual_present =
            host.render_media_actual_present;
        if (host.render_media_actual_present) {
            copy_media_actual(&host.render_media_actual,
                              &candidate.render_media_actual);
        }
        candidate.camera_media_actual_present =
            host.camera_media_actual_present;
        if (host.camera_media_actual_present) {
            copy_media_actual(&host.camera_media_actual,
                              &candidate.camera_media_actual);
        }
    }
    memmove(out_snapshot, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_media_latest(
    MeisoEdgeHandle *handle,
    MeisoCompositionMediaSource source,
    MeisoEdgeMediaFrameView *out_frame
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot edge;
    MeisoCompositionMediaLatestView latest;
    MeisoEdgeMediaFrameView candidate;
    MeisoCompositionHostAppStatus host_status;
    int index;

    if (!handle_valid(handle) || out_frame == NULL ||
        !disjoint_from_handle_storage(handle, out_frame, sizeof(*out_frame))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    index = media_index(source);
    if (index < 0) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        SimulatorMediaRoute *route =
            &handle->simulator_media[(size_t)index];
        const MeisoEdgeSnapshot *sim_edge =
            &handle->simulator_snapshot.edge;
        if (sim_edge->phase == MEISO_EDGE_PHASE_TERMINAL) {
            return MEISO_EDGE_TERMINAL;
        }
        if (!sim_edge->resource_graph_live ||
            !handle->simulator_snapshot.media[(size_t)index]
                 .actual_binding_ready) {
            return MEISO_EDGE_NOT_READY;
        }
        if (!route->present) {
            return MEISO_EDGE_NO_DATA;
        }
        if (route->borrowed) {
            return MEISO_EDGE_BUSY;
        }
        route->borrowed = true;
        memmove(out_frame, &route->frame, sizeof(*out_frame));
        return MEISO_EDGE_OK;
    }
    if (!read_host_snapshot(handle, &host, &edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    refresh_media_borrows(handle, &host, &edge);
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    host_status = meiso_composition_host_app_media_latest(
        handle->host_app, source, &latest);
    if (host_status == MEISO_COMPOSITION_HOST_APP_INVALID_STATE) {
        return MEISO_EDGE_NOT_READY;
    }
    if (host_status != MEISO_COMPOSITION_HOST_APP_OK) {
        return status_from_host(host_status);
    }
    candidate = (MeisoEdgeMediaFrameView){
        {
            handle->continuity_serial,
            source,
            latest.unit.binding.media_stream_epoch,
            latest.ref.frame_sequence
        },
        latest.unit.source_time,
        latest.unit.applied_scene_owner,
        latest.unit.applied_scene_state_version,
        latest.unit.active_plan_generation,
        latest.unit.world_generation,
        latest.unit.dependency_sequence,
        latest.unit.producer_dropped_frames,
        latest.unit.pixel_format,
        latest.unit.codec,
        latest.unit.width,
        latest.unit.height,
        latest.unit.flags,
        latest.unit.access_unit
    };
    handle->media_borrows[(size_t)index].active = true;
    handle->media_borrows[(size_t)index].ref = latest.ref;
    memmove(out_frame, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
)
{
    MeisoCompositionHostAppSnapshot host;
    MeisoEdgeSnapshot edge;
    MeisoCompositionHostAppStatus host_status;
    MediaBorrow *borrow;
    int index;

    if (!handle_valid(handle) || expected_frame == NULL ||
        !disjoint_from_handle_storage(handle, expected_frame,
                                      sizeof(*expected_frame))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    index = media_index(expected_frame->source);
    if (index < 0) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->backend_kind == EDGE_BACKEND_SIMULATOR) {
        SimulatorMediaRoute *route =
            &handle->simulator_media[(size_t)index];
        const MeisoEdgeSnapshot *sim_edge =
            &handle->simulator_snapshot.edge;
        if (sim_edge->phase == MEISO_EDGE_PHASE_TERMINAL) {
            return MEISO_EDGE_TERMINAL;
        }
        if (!sim_edge->resource_graph_live) {
            return MEISO_EDGE_NOT_READY;
        }
        if (!route->borrowed ||
            expected_frame->continuity_serial !=
                handle->continuity_serial ||
            expected_frame->source != route->frame.key.source ||
            expected_frame->media_stream_epoch !=
                route->frame.key.media_stream_epoch ||
            expected_frame->frame_sequence !=
                route->frame.key.frame_sequence) {
            return MEISO_EDGE_STALE;
        }
        route->borrowed = false;
        route->present = false;
        memset(&route->frame, 0, sizeof(route->frame));
        handle->simulator_snapshot.media[(size_t)index].latest_present = false;
        handle->simulator_snapshot.media[(size_t)index]
            .latest_frame_sequence = 0U;
        handle->simulator_snapshot.media[(size_t)index]
            .latest_source_time = (MeisoTimePoint){0U, 0U};
        handle->simulator_snapshot.media[(size_t)index]
            .latest_access_unit_bytes = 0U;
        return MEISO_EDGE_OK;
    }
    if (!read_host_snapshot(handle, &host, &edge)) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    refresh_media_borrows(handle, &host, &edge);
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    borrow = &handle->media_borrows[(size_t)index];
    if (!borrow->active ||
        expected_frame->continuity_serial != handle->continuity_serial ||
        expected_frame->frame_sequence != borrow->ref.frame_sequence ||
        expected_frame->media_stream_epoch !=
            borrow->ref.binding.media_stream_epoch) {
        return MEISO_EDGE_STALE;
    }
    host_status = meiso_composition_host_app_media_release(
        handle->host_app, &borrow->ref);
    memset(borrow, 0, sizeof(*borrow));
    return status_from_host(host_status);
}

static bool simulator_device_sample_zero(
    const MeisoDeviceTelemetrySample *sample
)
{
    return sample != NULL && sample->selected_fields == 0U &&
        sample->present_fields == 0U && sample->temperature_millic == 0 &&
        sample->temperature_uncertainty_millic == 0U &&
        sample->cpu_load_ppm == 0U && sample->memory_total_bytes == 0U &&
        sample->memory_available_bytes == 0U;
}

static bool simulator_device_sample_within_capability(
    const MeisoDeviceTelemetryCapability *capability,
    const MeisoDeviceTelemetrySample *sample
)
{
    const bool temperature_present = sample != NULL &&
        (sample->present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U;

    return capability != NULL && sample != NULL &&
        (sample->selected_fields & ~capability->supported_fields) == 0U &&
        (!temperature_present ||
         (sample->temperature_millic >= capability->min_temperature_millic &&
          sample->temperature_millic <= capability->max_temperature_millic &&
          sample->temperature_uncertainty_millic <=
              capability->max_temperature_uncertainty_millic));
}

static bool simulator_composition_config_valid(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    const MeisoCompositionMediaCapability *render;

    if (config == NULL) {
        return false;
    }
    if (!config->composition_enabled) {
        return config->composition_blob_slot_count == 0U &&
            config->composition_clock_epoch == 0U &&
            config->presentation_production_epoch == 0U &&
            config->scene_production_epoch == 0U &&
            !config->cpu_raster_preview_enabled;
    }
    render = &config->catalog.render_media_capability;
    return config->composition_blob_slot_count != 0U &&
        config->composition_blob_slot_count <=
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS &&
        config->composition_clock_epoch != 0U &&
        config->presentation_production_epoch != 0U &&
        config->scene_production_epoch != 0U &&
        (!config->cpu_raster_preview_enabled ||
         (render->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
          render->pixel_formats ==
              MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 &&
          render->codecs == MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW &&
          render->min_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
          render->max_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
          render->min_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
          render->max_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
          render->max_access_unit_bytes >=
              MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES));
}

size_t meiso_composition_edge_simulator_alignment_required(void)
{
    size_t alignment = _Alignof(MeisoEdgeHandle);
    const size_t blob_alignment = meiso_blob_alignment_required();
    const size_t scene_alignment =
        meiso_composition_scene_store_alignment_required();
    const size_t coordinator_alignment =
        meiso_composition_coordinator_alignment_required();
    const size_t renderer_alignment =
        meiso_composition_cpu_raster_renderer_alignment_required();

    if (blob_alignment > alignment) {
        alignment = blob_alignment;
    }
    if (scene_alignment > alignment) {
        alignment = scene_alignment;
    }
    if (coordinator_alignment > alignment) {
        alignment = coordinator_alignment;
    }
    if (renderer_alignment > alignment) {
        alignment = renderer_alignment;
    }
    return alignment;
}

size_t meiso_composition_edge_simulator_memory_required(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    SimulatorLayout layout;

    if (!simulator_layout(config, &layout)) {
        return 0U;
    }
    return layout.required;
}

MeisoCompositionEdgeSimulatorInitStatus meiso_composition_edge_simulator_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoEdgeHandle **out_handle
)
{
    MeisoEdgeHandle candidate;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t config_begin;
    uintptr_t config_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    size_t required;
    SimulatorLayout layout;
    size_t i;
    size_t wifi_sample_wire_size = 0U;
    size_t device_sample_wire_size = 0U;
    uint8_t wifi_sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t device_sample_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t *arena_bytes;

    if (!simulator_layout(config, &layout)) {
        return config == NULL ?
            MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT :
            MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    required = layout.required;
    if (arena == NULL || config == NULL || out_handle == NULL ||
        required == 0U || config->catalog_generation == 0U ||
        config->continuity_serial == 0U ||
        !meiso_owner_scope_valid(config->desired_owner) ||
        config->wifi_production_epoch == 0U ||
        config->render_media_stream_epoch == 0U ||
        config->camera_media_stream_epoch == 0U ||
        !checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !checked_range(config, sizeof(*config), &config_begin, &config_end) ||
        !checked_range(out_handle, sizeof(*out_handle), &output_begin,
                       &output_end) ||
        ranges_overlap(config_begin, config_end, output_begin, output_end)) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT;
    }
    if (meiso_wifi_telemetry_sample_encode(
            &config->wifi_sample,
            (MeisoMutableBuffer){wifi_sample_wire, sizeof(wifi_sample_wire)},
            &wifi_sample_wire_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        wifi_sample_wire_size != MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE ||
        (config->wifi_sample.selected_fields &
         ~config->catalog.wifi_capability.supported_fields) != 0U) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    if ((!config->device_telemetry_enabled &&
         (config->device_production_epoch != 0U ||
          !simulator_device_sample_zero(&config->device_sample))) ||
        (config->device_telemetry_enabled &&
         (config->device_production_epoch == 0U ||
          meiso_device_telemetry_sample_encode(
              &config->device_sample,
              (MeisoMutableBuffer){device_sample_wire,
                                   sizeof(device_sample_wire)},
              &device_sample_wire_size
          ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
          device_sample_wire_size != MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE ||
          !simulator_device_sample_within_capability(
              &config->catalog.device_telemetry_capability,
              &config->device_sample
          ))) || !simulator_composition_config_valid(config)) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    if ((arena_begin %
         meiso_composition_edge_simulator_alignment_required()) != 0U) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < required) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_ARENA_TOO_SMALL;
    }
    if (ranges_overlap(arena_begin, arena_begin + required,
                       config_begin, config_end) ||
        ranges_overlap(arena_begin, arena_begin + required,
                       output_begin, output_end)) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT;
    }

    memset(&candidate, 0, sizeof(candidate));
    if (meiso_composition_session_v3_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){
                candidate.dynamic_capability,
                sizeof(candidate.dynamic_capability)
            },
            candidate.endpoints,
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            candidate.routes,
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) != MEISO_COMPOSITION_SESSION_V3_OK) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    candidate.magic = EDGE_HANDLE_MAGIC;
    candidate.backend_kind = EDGE_BACKEND_SIMULATOR;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = arena_begin + required;
    candidate.catalog_generation = config->catalog_generation;
    candidate.continuity_serial = config->continuity_serial;
    candidate.profile = *meiso_composition_scene_profile_v3();
    candidate.simulator_catalog = config->catalog;
    candidate.simulator_desired_owner = config->desired_owner;
    candidate.simulator_wifi_epoch = config->wifi_production_epoch;
    candidate.simulator_wifi_epoch_available = true;
    candidate.simulator_media_epoch[0] =
        config->render_media_stream_epoch;
    candidate.simulator_media_epoch[1] =
        config->camera_media_stream_epoch;
    candidate.simulator_media_epoch_available[0] = true;
    candidate.simulator_media_epoch_available[1] = true;
    candidate.simulator_wifi_sample = config->wifi_sample;
    candidate.simulator_device_supported = config->device_telemetry_enabled;
    candidate.simulator_device_epoch = config->device_production_epoch;
    candidate.simulator_device_epoch_available =
        config->device_telemetry_enabled;
    candidate.simulator_device_sample = config->device_sample;
    candidate.simulator_composition_supported =
        config->composition_enabled;
    candidate.simulator_cpu_raster_enabled =
        config->cpu_raster_preview_enabled;
    candidate.simulator_blob_slot_count =
        config->composition_blob_slot_count;
    candidate.simulator_composition_clock_epoch =
        config->composition_clock_epoch;
    candidate.simulator_presentation_epoch =
        config->presentation_production_epoch;
    candidate.simulator_presentation_epoch_available =
        config->composition_enabled;
    candidate.simulator_scene_epoch = config->scene_production_epoch;
    candidate.simulator_scene_epoch_available =
        config->composition_enabled;
    candidate.simulator_next_plan_generation = 1U;
    candidate.simulator_plan_generation_available =
        config->composition_enabled;
    candidate.simulator_snapshot.edge = (MeisoEdgeSnapshot){
        MEISO_VIRTUAL_EDGE_API_VERSION,
        candidate.profile,
        MEISO_EDGE_PHASE_BOOTSTRAPPING,
        MEISO_EDGE_TERMINAL_NONE,
        false,
        false,
        config->continuity_serial,
        0U,
        0U,
        0U,
        0U
    };
    arena_bytes = (uint8_t *)arena;
    candidate.simulator_media[0].buffer =
        arena_bytes + layout.render_offset;
    candidate.simulator_media[0].capacity =
        config->catalog.render_media_capability.max_access_unit_bytes;
    candidate.simulator_media[1].buffer =
        arena_bytes + layout.camera_offset;
    candidate.simulator_media[1].capacity =
        config->catalog.camera_media_capability.max_access_unit_bytes;
    if (config->composition_enabled) {
        candidate.simulator_mesh_buffer = arena_bytes + layout.mesh_offset;
        candidate.simulator_mesh_buffer_capacity =
            MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
        candidate.simulator_blob_arena = arena_bytes + layout.blob_offset;
        candidate.simulator_blob_arena_size = layout.blob_size;
        candidate.simulator_scene_arena = arena_bytes + layout.scene_offset;
        candidate.simulator_scene_arena_size = layout.scene_size;
        candidate.simulator_coordinator_arena =
            arena_bytes + layout.coordinator_offset;
        candidate.simulator_coordinator_arena_size =
            layout.coordinator_size;
        if (config->cpu_raster_preview_enabled) {
            candidate.simulator_renderer_arena =
                arena_bytes + layout.renderer_offset;
            candidate.simulator_renderer_arena_size = layout.renderer_size;
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT; ++i) {
        const uintptr_t capability_begin =
            (uintptr_t)candidate.endpoints[i].capability.data;
        const uintptr_t dynamic_begin =
            (uintptr_t)candidate.dynamic_capability;
        const uintptr_t dynamic_end =
            dynamic_begin + sizeof(candidate.dynamic_capability);
        if (candidate.endpoints[i].capability.size != 0U &&
            capability_begin >= dynamic_begin && capability_begin < dynamic_end) {
            const size_t offset = (size_t)(capability_begin - dynamic_begin);
            if (offset > sizeof(candidate.dynamic_capability) ||
                candidate.endpoints[i].capability.size >
                    sizeof(candidate.dynamic_capability) - offset) {
                return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INTERNAL_FAILURE;
            }
            candidate.endpoints[i].capability.data =
                (const uint8_t *)arena +
                offsetof(MeisoEdgeHandle, dynamic_capability) + offset;
        }
    }
    memset(arena, 0, required);
    memmove(arena, &candidate, sizeof(candidate));
    *out_handle = (MeisoEdgeHandle *)arena;
    return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK;
}

static bool simulator_composition_initialize(MeisoEdgeHandle *handle)
{
    const MeisoResourceDescriptor *blob_descriptor;
    MeisoBlobConfig blob_config;
    MeisoCompositionCoordinatorConfig coordinator_config;
    MeisoBlobStore *blob_store = NULL;
    MeisoCompositionSceneStore *scene_store = NULL;
    MeisoCompositionCoordinator *coordinator = NULL;
    MeisoCompositionCpuRasterRenderer *renderer = NULL;
    MeisoCompositionSceneControlScope scope;

    if (handle == NULL || !handle->simulator_composition_supported) {
        return handle != NULL;
    }
    if (handle->simulator_composition_initialized) {
        return true;
    }
    blob_descriptor = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    blob_config.published_slot_count = handle->simulator_blob_slot_count;
    if (blob_descriptor == NULL ||
        meiso_blob_store_init(
            handle->simulator_blob_arena,
            handle->simulator_blob_arena_size,
            blob_descriptor, &blob_config, &blob_store
        ) != MEISO_BLOB_INIT_OK ||
        meiso_composition_scene_store_init(
            handle->simulator_scene_arena,
            handle->simulator_scene_arena_size,
            blob_store, &scene_store
        ) != MEISO_COMPOSITION_SCENE_STORE_INIT_OK) {
        return false;
    }
    memset(&coordinator_config, 0, sizeof(coordinator_config));
    coordinator_config.scene_store = scene_store;
    coordinator_config.session_id.host_nonce = (MeisoSessionNonce){
        handle->continuity_serial,
        handle->simulator_desired_owner.incarnation
    };
    coordinator_config.session_id.edge_nonce = (MeisoSessionNonce){
        handle->catalog_generation,
        handle->simulator_catalog.presentation_incarnation
    };
    coordinator_config.edge_incarnation =
        handle->simulator_catalog.presentation_incarnation;
    coordinator_config.capability_generation = handle->catalog_generation;
    coordinator_config.edge_clock_epoch =
        handle->simulator_composition_clock_epoch;
    if (meiso_composition_coordinator_init(
            handle->simulator_coordinator_arena,
            handle->simulator_coordinator_arena_size,
            &coordinator_config, &coordinator
        ) != MEISO_COMPOSITION_COORDINATOR_INIT_OK ||
        meiso_composition_coordinator_begin_owner(
            coordinator, handle->simulator_desired_owner.incarnation
        ) != MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED) {
        return false;
    }
    if (handle->simulator_cpu_raster_enabled &&
        (meiso_composition_cpu_raster_renderer_init(
             handle->simulator_renderer_arena,
             handle->simulator_renderer_arena_size,
             blob_store, &renderer
         ) != MEISO_COMPOSITION_CPU_RASTER_INIT_OK ||
         !meiso_composition_scene_store_control_scope(scene_store, &scope) ||
         meiso_composition_cpu_raster_renderer_begin_control_scope(
             renderer, &scope
         ) != MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED)) {
        return false;
    }
    handle->simulator_blob_store = blob_store;
    handle->simulator_scene_store = scene_store;
    handle->simulator_coordinator = coordinator;
    handle->simulator_renderer = renderer;
    handle->simulator_composition_initialized = true;
    return true;
}

MeisoEdgeStatus meiso_composition_edge_simulator_activate(
    MeisoEdgeHandle *handle
)
{
    MeisoEdgeSnapshot *edge;

    if (!handle_valid(handle) ||
        handle->backend_kind != EDGE_BACKEND_SIMULATOR) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    edge = &handle->simulator_snapshot.edge;
    if (edge->phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (edge->phase == MEISO_EDGE_PHASE_READY) {
        return MEISO_EDGE_CONFLICT;
    }
    if (!simulator_composition_initialize(handle)) {
        (void)meiso_composition_edge_simulator_terminate(
            handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
        );
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    edge->phase = MEISO_EDGE_PHASE_READY;
    edge->online = true;
    edge->resource_graph_live = true;
    edge->catalog_generation = handle->catalog_generation;
    edge->operations = SIMULATOR_SUPPORTED_OPERATIONS |
        (handle->simulator_composition_supported ?
         (MEISO_EDGE_OPERATION_BLOB_PUT |
          MEISO_EDGE_OPERATION_COMPOSITION) : 0U);
    edge->endpoint_count = MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT;
    edge->resource_count = MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT;
    return MEISO_EDGE_OK;
}

static void simulator_apply_wifi(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionEdgeSnapshot *snapshot = &handle->simulator_snapshot;
    MeisoCompositionEdgeInlineView *stream =
        &snapshot->inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI];

    simulator_apply_actual(handle, MEISO_COMPOSITION_EDGE_DESIRED_WIFI,
                           handle->simulator_wifi_epoch);
    handle->simulator_wifi_pending = false;
    snapshot->wifi_effective_present = handle->simulator_wifi_enabled;
    snapshot->wifi_effective = handle->simulator_wifi_enabled ?
        (MeisoWifiTelemetryEffectiveParameters){
            handle->simulator_wifi_desired.selected_fields,
            handle->simulator_wifi_desired.sample_period_ms
        } : (MeisoWifiTelemetryEffectiveParameters){0U, 0U};
    snapshot->wifi_sample_present = handle->simulator_wifi_enabled;
    if (!handle->simulator_wifi_enabled) {
        memset(&snapshot->wifi_sample, 0, sizeof(snapshot->wifi_sample));
        memset(stream, 0, sizeof(*stream));
        return;
    }
    snapshot->wifi_sample = handle->simulator_wifi_sample;
    snapshot->wifi_sample.selected_fields =
        handle->simulator_wifi_desired.selected_fields;
    snapshot->wifi_sample.present_fields &=
        handle->simulator_wifi_desired.selected_fields;
    if ((snapshot->wifi_sample.present_fields &
         MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE) == 0U) {
        snapshot->wifi_sample.link_state = false;
    }
    if ((snapshot->wifi_sample.present_fields &
         MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) == 0U) {
        snapshot->wifi_sample.signal_mbm = 0;
    }
#define CLEAR_WIFI_COUNTER(field, mask)                                        \
    do {                                                                       \
        if ((snapshot->wifi_sample.present_fields & (mask)) == 0U) {           \
            snapshot->wifi_sample.field = 0U;                                  \
        }                                                                      \
    } while (0)
    CLEAR_WIFI_COUNTER(rx_bytes, MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES);
    CLEAR_WIFI_COUNTER(tx_bytes, MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES);
    CLEAR_WIFI_COUNTER(rx_packets, MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS);
    CLEAR_WIFI_COUNTER(tx_packets, MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS);
    CLEAR_WIFI_COUNTER(rx_dropped, MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED);
    CLEAR_WIFI_COUNTER(tx_dropped, MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED);
    CLEAR_WIFI_COUNTER(rx_errors, MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS);
    CLEAR_WIFI_COUNTER(tx_errors, MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS);
#undef CLEAR_WIFI_COUNTER
    *stream = (MeisoCompositionEdgeInlineView){
        true,
        {handle->simulator_desired_owner, handle->simulator_wifi_epoch, 1U},
        now,
        MEISO_STREAM_RECEIVE_FLAG_NONE,
        0U,
        stream->received_count + 1U,
        0U
    };
    if (handle->simulator_wifi_epoch == UINT64_MAX) {
        handle->simulator_wifi_epoch_available = false;
    } else {
        ++handle->simulator_wifi_epoch;
    }
}

static void simulator_apply_device(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionEdgeSnapshot *snapshot = &handle->simulator_snapshot;
    MeisoCompositionEdgeInlineView *stream =
        &snapshot->inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE];

    simulator_apply_actual(handle, MEISO_COMPOSITION_EDGE_DESIRED_DEVICE,
                           handle->simulator_device_epoch);
    handle->simulator_device_pending = false;
    snapshot->device_effective_present = handle->simulator_device_enabled;
    snapshot->device_effective = handle->simulator_device_enabled ?
        handle->simulator_device_desired :
        (MeisoDeviceTelemetryParameters){0U, 0U};
    snapshot->device_sample_present = handle->simulator_device_enabled;
    if (!handle->simulator_device_enabled) {
        memset(&snapshot->device_sample, 0, sizeof(snapshot->device_sample));
        memset(stream, 0, sizeof(*stream));
        return;
    }
    snapshot->device_sample = handle->simulator_device_sample;
    snapshot->device_sample.selected_fields =
        handle->simulator_device_desired.selected_fields;
    snapshot->device_sample.present_fields &=
        handle->simulator_device_desired.selected_fields;
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) == 0U) {
        snapshot->device_sample.temperature_millic = 0;
        snapshot->device_sample.temperature_uncertainty_millic = 0U;
    }
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM) == 0U) {
        snapshot->device_sample.cpu_load_ppm = 0U;
    }
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES) == 0U) {
        snapshot->device_sample.memory_total_bytes = 0U;
    }
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES) == 0U) {
        snapshot->device_sample.memory_available_bytes = 0U;
    }
    *stream = (MeisoCompositionEdgeInlineView){
        true,
        {handle->simulator_desired_owner, handle->simulator_device_epoch, 1U},
        now,
        MEISO_STREAM_RECEIVE_FLAG_NONE,
        0U,
        stream->received_count + 1U,
        0U
    };
    if (handle->simulator_device_epoch == UINT64_MAX) {
        handle->simulator_device_epoch_available = false;
    } else {
        ++handle->simulator_device_epoch;
    }
}

static void simulator_apply_media(MeisoEdgeHandle *handle, size_t index)
{
    SimulatorMediaPending *pending =
        &handle->simulator_media_pending[index];
    MeisoCompositionEdgeSnapshot *snapshot = &handle->simulator_snapshot;
    const MeisoCompositionEdgeDesiredChannel channel = index == 0U ?
        MEISO_COMPOSITION_EDGE_DESIRED_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA;
    const MeisoCompositionMediaSource source = index == 0U ?
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    const uint64_t epoch = handle->simulator_media_epoch[index];
    MeisoCompositionEdgeMediaActual *actual = index == 0U ?
        &snapshot->render_media_actual : &snapshot->camera_media_actual;

    simulator_apply_actual(handle, channel, epoch);
    pending->pending = false;
    memset(&handle->simulator_media[index].frame, 0,
           sizeof(handle->simulator_media[index].frame));
    handle->simulator_media[index].present = false;
    handle->simulator_media[index].borrowed = false;
    snapshot->media[index] = (MeisoCompositionEdgeMediaRouteView){
        pending->enabled, false, source, 0U, {0U, 0U}, 0U, 0U, 0U, 0U, 0U,
        0U
    };
    if (index == 0U) {
        snapshot->render_media_actual_present = pending->enabled;
        handle->simulator_raster_preview_pending =
            handle->simulator_cpu_raster_enabled && pending->enabled &&
            snapshot->render_result_present;
    } else {
        snapshot->camera_media_actual_present = pending->enabled;
    }
    if (pending->enabled) {
        *actual = (MeisoCompositionEdgeMediaActual){
            source, pending->desired, epoch, 0U, 0U, 0U
        };
        if (handle->simulator_media_epoch[index] == UINT64_MAX) {
            handle->simulator_media_epoch_available[index] = false;
        } else {
            ++handle->simulator_media_epoch[index];
        }
    } else {
        memset(actual, 0, sizeof(*actual));
    }
}

static bool simulator_take_epoch(
    uint64_t *next_epoch,
    bool *available,
    uint64_t *out_epoch
)
{
    if (next_epoch == NULL || available == NULL || out_epoch == NULL ||
        !*available || *next_epoch == 0U) {
        return false;
    }
    *out_epoch = *next_epoch;
    if (*next_epoch == UINT64_MAX) {
        *available = false;
    } else {
        ++*next_epoch;
    }
    return true;
}

static bool simulator_candidate_nonce(
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

static bool simulator_refresh_presentation_status(MeisoEdgeHandle *handle)
{
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionEdgePresentationStatus public_status;

    if (!handle->simulator_composition_initialized ||
        !meiso_composition_coordinator_status(
            handle->simulator_coordinator, &status
        )) {
        return false;
    }
    memset(&public_status, 0, sizeof(public_status));
    public_status.capability_generation = status.capability_generation;
    public_status.candidate_state = status.candidate_state;
    public_status.candidate_plan_generation =
        status.candidate_plan_generation;
    memmove(public_status.candidate_digest, status.candidate_digest,
            sizeof(public_status.candidate_digest));
    public_status.active_state = status.active_state;
    public_status.active_plan_generation = status.active_plan_generation;
    memmove(public_status.active_digest, status.active_digest,
            sizeof(public_status.active_digest));
    public_status.reason = status.reason;
    public_status.target_width = status.target_width;
    public_status.target_height = status.target_height;
    public_status.color_format = status.color_format;
    public_status.depth_format = status.depth_format;
    handle->simulator_snapshot.presentation_status_present = true;
    handle->simulator_snapshot.presentation_status = public_status;
    return true;
}

static bool simulator_refresh_scene_status(MeisoEdgeHandle *handle)
{
    MeisoCompositionSceneResourceUsage usage;
    MeisoCompositionSceneStatus status;
    MeisoCompositionCpuRasterSnapshot renderer_snapshot;
    size_t blob_count;

    if (!handle->simulator_composition_initialized) {
        return false;
    }
    blob_count = meiso_blob_store_count(handle->simulator_blob_store);
    if (blob_count > UINT32_MAX) {
        return false;
    }
    memset(&usage, 0, sizeof(usage));
    usage.published_blob_count = (uint32_t)blob_count;
    usage.published_blob_bytes = handle->simulator_published_blob_bytes;
    if (handle->simulator_cpu_raster_enabled) {
        if (!meiso_composition_cpu_raster_renderer_snapshot(
                handle->simulator_renderer, &renderer_snapshot
            )) {
            return false;
        }
        usage.target_slots_used = renderer_snapshot.target_slots_allocated;
        usage.target_bytes = renderer_snapshot.target_bytes;
        usage.render_jobs_in_flight = renderer_snapshot.queued_jobs;
    }
    usage.result_queue_depth =
        handle->simulator_snapshot.inline_streams[
            MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
        ].present ? 1U : 0U;
    if (!meiso_composition_scene_store_status(
            handle->simulator_scene_store, &usage, &status
        )) {
        return false;
    }
    handle->simulator_snapshot.scene_status_present = true;
    handle->simulator_snapshot.scene_status = status;
    return true;
}

static bool simulator_refresh_composition_status(MeisoEdgeHandle *handle)
{
    return simulator_refresh_presentation_status(handle) &&
        simulator_refresh_scene_status(handle);
}

static bool simulator_clear_render_truth(MeisoEdgeHandle *handle)
{
    MeisoCompositionEdgeInlineView *stream =
        &handle->simulator_snapshot.inline_streams[
            MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
        ];
    const uint64_t received_count = stream->received_count;
    const uint64_t replaced_count = stream->replaced_count;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionSceneControlScope scope;

    if (handle->simulator_cpu_raster_enabled &&
        handle->simulator_renderer != NULL &&
        meiso_composition_cpu_raster_renderer_latest_complete(
            handle->simulator_renderer, &complete
        )) {
        const MeisoCompositionCpuRasterRetireStatus status =
            meiso_composition_scene_store_control_scope(
                handle->simulator_scene_store, &scope
            ) ? meiso_composition_cpu_raster_renderer_retire_world(
                    handle->simulator_renderer, &scope, &complete.job.world
                ) : MEISO_COMPOSITION_CPU_RASTER_RETIRE_INVALID_ARGUMENT;
        if (status != MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED &&
            status != MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED) {
            return false;
        }
    }

    memset(stream, 0, sizeof(*stream));
    stream->received_count = received_count;
    stream->replaced_count = replaced_count;
    handle->simulator_snapshot.render_result_present = false;
    memset(&handle->simulator_snapshot.render_result, 0,
           sizeof(handle->simulator_snapshot.render_result));
    handle->simulator_render_result_sequence = 0U;
    handle->simulator_render_result_pending = false;
    handle->simulator_raster_preview_pending = false;
    return true;
}

typedef int SimulatorPrepareResult;
enum {
    SIMULATOR_PREPARE_INTERNAL = -1,
    SIMULATOR_PREPARE_WAITING = 0,
    SIMULATOR_PREPARE_PREPARED = 1,
    SIMULATOR_PREPARE_REJECTED = 2
};

static SimulatorPrepareResult simulator_prepare_candidate(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionScenePrepareStatus prepare =
        MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB;
    MeisoCompositionCoordinatorReportStatus report;
    MeisoCompositionCoordinatorBackendOutcome outcome =
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY;
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE] = {1U};
    const uint8_t *nonce_pointer = nonce;

    if (!meiso_composition_coordinator_candidate(
            handle->simulator_coordinator, &candidate
        ) || candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING) {
        return SIMULATOR_PREPARE_WAITING;
    }
    if (handle->simulator_scene_value_present &&
        handle->simulator_scene_enabled) {
        prepare = meiso_composition_scene_store_prepare(
            handle->simulator_scene_store,
            handle->simulator_desired_owner.incarnation,
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].state_version,
            candidate.ref.plan_generation,
            &handle->simulator_scene_value
        );
    }
    if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED ||
        prepare == MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED ||
        prepare == MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE) {
        if (!simulator_candidate_nonce(&candidate.ref, nonce)) {
            return SIMULATOR_PREPARE_INTERNAL;
        }
    } else if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
        /* READY without a matching Scene candidate yields WAITING_SCENE. */
    } else if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY) {
        outcome = MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY;
        nonce_pointer = NULL;
    } else if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED ||
               prepare == MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH) {
        outcome = MEISO_COMPOSITION_COORDINATOR_BACKEND_PREPARE_FAILED;
        nonce_pointer = NULL;
    } else {
        return SIMULATOR_PREPARE_INTERNAL;
    }
    report = meiso_composition_coordinator_report_prepare_outcome(
        handle->simulator_coordinator, &candidate.ref, outcome, now,
        nonce_pointer
    );
    if (report == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED) {
        handle->simulator_presentation_activation_pending = true;
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
        return SIMULATOR_PREPARE_PREPARED;
    }
    if (report == MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE) {
        handle->simulator_scene_retry_pending =
            handle->simulator_scene_value_present &&
            handle->simulator_scene_enabled;
        handle->simulator_scene_retry_ready = false;
        return SIMULATOR_PREPARE_WAITING;
    }
    if (report == MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED ||
        report == MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED) {
        simulator_reject_actual(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
            outcome == MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY ?
                MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED :
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
        );
        if (handle->simulator_scene_value_present &&
            handle->simulator_scene_enabled) {
            simulator_reject_actual(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                outcome == MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY ?
                    MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED :
                    MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
            );
        }
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
        return SIMULATOR_PREPARE_REJECTED;
    }
    return SIMULATOR_PREPARE_INTERNAL;
}

static bool simulator_publish_mesh(MeisoEdgeHandle *handle)
{
    MeisoMutableBuffer staging;
    MeisoBlobStageStatus stage;
    MeisoBlobCommitStatus commit = MEISO_BLOB_COMMIT_INVALID;
    MeisoRuntimeBlobResult result;
    bool published = false;
    bool duplicate = false;

    stage = meiso_blob_store_stage_begin(
        handle->simulator_blob_store,
        &handle->simulator_mesh_content_id,
        handle->simulator_mesh_size
    );
    if (stage == MEISO_BLOB_STAGE_STARTED) {
        if (!meiso_blob_store_staging_buffer(
                handle->simulator_blob_store, &staging
            ) || staging.data == NULL ||
            staging.capacity < handle->simulator_mesh_size) {
            return false;
        }
        memmove(staging.data, handle->simulator_mesh_buffer,
                handle->simulator_mesh_size);
        commit = meiso_blob_store_commit_verified(
            handle->simulator_blob_store,
            handle->simulator_mesh_size,
            meiso_composition_scene_mesh_blob_verify, NULL
        );
        published = commit == MEISO_BLOB_COMMIT_PUBLISHED;
        duplicate = commit == MEISO_BLOB_COMMIT_DUPLICATE;
        if (published) {
            result = MEISO_RUNTIME_BLOB_RESULT_PUBLISHED;
        } else if (duplicate) {
            result = MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
        } else if (commit == MEISO_BLOB_COMMIT_CAPACITY) {
            result = MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
        } else if (commit == MEISO_BLOB_COMMIT_INTEGRITY_FAILED) {
            result = MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED;
        } else if (commit == MEISO_BLOB_COMMIT_LENGTH_MISMATCH) {
            result = MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH;
        } else {
            return false;
        }
        if (!published && !duplicate &&
            meiso_blob_store_abort_staging(
                handle->simulator_blob_store,
                MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY
            ) != MEISO_BLOB_ABORTED) {
            return false;
        }
    } else if (stage == MEISO_BLOB_STAGE_UNCHANGED) {
        duplicate = true;
        result = MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
    } else if (stage == MEISO_BLOB_STAGE_CAPACITY) {
        result = MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
    } else if (stage == MEISO_BLOB_STAGE_CONFLICT) {
        result = MEISO_RUNTIME_BLOB_RESULT_CONFLICT;
    } else if (stage == MEISO_BLOB_STAGE_INVALID_CONTENT_ID) {
        result = MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID;
    } else {
        return false;
    }
    if (published || duplicate) {
        const MeisoCompositionSceneBlobVisibilityStatus visibility =
            meiso_composition_scene_store_mark_blob_visible(
                handle->simulator_scene_store,
                handle->simulator_desired_owner.incarnation,
                &handle->simulator_mesh_content_id,
                handle->simulator_mesh_size
            );
        if (visibility != MEISO_COMPOSITION_SCENE_BLOB_VISIBLE &&
            visibility !=
                MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED) {
            return false;
        }
        if (published) {
            if (handle->simulator_published_blob_bytes >
                UINT64_MAX - handle->simulator_mesh_size) {
                return false;
            }
            handle->simulator_published_blob_bytes +=
                handle->simulator_mesh_size;
        }
    }
    handle->simulator_mesh_pending = false;
    handle->simulator_snapshot.mesh.queued = false;
    handle->simulator_snapshot.mesh.transfer_active = false;
    handle->simulator_snapshot.mesh.begin_ready = published || duplicate;
    handle->simulator_snapshot.mesh.waiting_result = false;
    handle->simulator_snapshot.mesh.terminal_result_present = true;
    handle->simulator_snapshot.mesh.terminal_result = result;
    handle->simulator_snapshot.mesh.edge_core_published =
        published || duplicate;
    handle->simulator_snapshot.mesh.next_offset =
        published || duplicate ? handle->simulator_mesh_size : 0U;
    if ((published || duplicate) &&
        handle->simulator_scene_retry_pending) {
        handle->simulator_scene_retry_ready = true;
    }
    return simulator_refresh_scene_status(handle);
}

static bool simulator_runtime_plan(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE],
    MeisoRuntimeDesiredState *out_desired
)
{
    size_t wire_size = 0U;

    if (plan == NULL || wire == NULL || out_desired == NULL ||
        meiso_composition_scene_presentation_plan_encode(
            plan,
            (MeisoMutableBuffer){
                wire, MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE
            }, &wire_size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        wire_size != MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE) {
        return false;
    }
    out_desired->enabled = true;
    out_desired->profile = (MeisoByteView){wire, wire_size};
    return true;
}

static bool simulator_apply_scene_command(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    SimulatorScenePending *pending = &handle->simulator_scene_pending;
    MeisoCompositionCoordinatorPlanView plan;
    MeisoCompositionScenePrepareStatus prepare;
    MeisoCompositionCoordinatorSwapStatus swap;
    MeisoCompositionSceneWorldRef world;
    MeisoCompositionSceneWorldView active_world;
    const bool command_pending = pending->pending;
    const bool requested_enabled = command_pending ? pending->enabled :
        handle->simulator_scene_enabled;
    bool plan_active = false;
    uint64_t epoch;
    MeisoCompositionSceneDisableStatus disable_status;

    if (command_pending) {
        pending->pending = false;
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
    }
    if (!requested_enabled) {
        if (meiso_composition_scene_store_active(
                handle->simulator_scene_store, &active_world
            )) {
            simulator_reject_actual(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
            );
            return simulator_refresh_composition_status(handle);
        }
        handle->simulator_scene_enabled = false;
        handle->simulator_scene_value_present = false;
        memset(&handle->simulator_scene_value, 0,
               sizeof(handle->simulator_scene_value));
        disable_status = meiso_composition_scene_store_disable(
            handle->simulator_scene_store, NULL
        );
        if (disable_status != MEISO_COMPOSITION_SCENE_DISABLE_DISABLED &&
            disable_status !=
                MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED) {
            return false;
        }
        simulator_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, false, false, 0U
        );
        return simulator_clear_render_truth(handle) &&
            simulator_refresh_composition_status(handle);
    }
    if (command_pending) {
        handle->simulator_scene_enabled = true;
        handle->simulator_scene_value_present = true;
        handle->simulator_scene_value = pending->value;
    }
    if (meiso_composition_coordinator_active(
            handle->simulator_coordinator, &plan
        )) {
        plan_active = true;
    } else if (!meiso_composition_coordinator_candidate(
                   handle->simulator_coordinator, &plan
               )) {
        simulator_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, false, 0U
        );
        return simulator_refresh_composition_status(handle);
    }
    prepare = meiso_composition_scene_store_prepare(
        handle->simulator_scene_store,
        handle->simulator_desired_owner.incarnation,
        handle->simulator_snapshot.desired[
            MEISO_COMPOSITION_EDGE_DESIRED_SCENE
        ].state_version,
        plan.ref.plan_generation,
        &handle->simulator_scene_value
    );
    if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
        handle->simulator_scene_retry_pending = true;
        handle->simulator_scene_retry_ready = false;
        simulator_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, false, 0U
        );
        if (!plan_active &&
            simulator_prepare_candidate(handle, now) ==
                SIMULATOR_PREPARE_INTERNAL) {
            return false;
        }
        return simulator_refresh_composition_status(handle);
    }
    if (prepare != MEISO_COMPOSITION_SCENE_PREPARE_PREPARED &&
        prepare != MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED &&
        prepare != MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE) {
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
        simulator_reject_actual(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
            prepare == MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY ?
                MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED :
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
        );
        return simulator_refresh_composition_status(handle);
    }
    if (!plan_active) {
        const SimulatorPrepareResult result =
            simulator_prepare_candidate(handle, now);
        if (result == SIMULATOR_PREPARE_INTERNAL) {
            return false;
        }
        simulator_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, false, 0U
        );
        return simulator_refresh_composition_status(handle);
    }
    if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE) {
        if (!meiso_composition_scene_store_active(
                handle->simulator_scene_store, &active_world
            )) {
            return false;
        }
        epoch = handle->simulator_snapshot.actual[
            MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
        ].production_epoch;
        if (epoch == 0U) {
            return false;
        }
    } else {
        swap = meiso_composition_coordinator_swap_scene_frame_boundary(
            handle->simulator_coordinator,
            handle->simulator_desired_owner.incarnation,
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].state_version,
            now, &world
        );
        if (swap != MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED ||
            !simulator_take_epoch(
                &handle->simulator_scene_epoch,
                &handle->simulator_scene_epoch_available, &epoch
            )) {
            return false;
        }
    }
    handle->simulator_scene_retry_pending = false;
    handle->simulator_scene_retry_ready = false;
    simulator_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, true, epoch
    );
    if (!simulator_clear_render_truth(handle)) {
        return false;
    }
    handle->simulator_render_result_pending = true;
    return simulator_refresh_composition_status(handle);
}

static bool simulator_apply_presentation_command(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    SimulatorPresentationPending *pending =
        &handle->simulator_presentation_pending;
    MeisoRuntimeDesiredState desired;
    MeisoCompositionCoordinatorObserveStatus observe;
    MeisoCompositionCoordinatorDisableStatus disable;
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];

    pending->pending = false;
    if (!pending->enabled) {
        desired = (MeisoRuntimeDesiredState){false, {NULL, 0U}};
        observe = meiso_composition_coordinator_observe_desired(
            handle->simulator_coordinator,
            handle->simulator_desired_owner.incarnation,
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
            ].state_version,
            &desired, now
        );
        if (observe !=
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING &&
            observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE) {
            return false;
        }
        disable = meiso_composition_coordinator_disable_frame_boundary(
            handle->simulator_coordinator, true, now
        );
        if (disable != MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED &&
            disable !=
                MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED) {
            return false;
        }
        handle->simulator_presentation_plan_present = false;
        handle->simulator_presentation_activation_pending = false;
        simulator_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
            false, false, 0U
        );
        if (!simulator_clear_render_truth(handle)) {
            return false;
        }
        if (handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].requested && !handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].enabled) {
            handle->simulator_scene_enabled = false;
            handle->simulator_scene_value_present = false;
            memset(&handle->simulator_scene_value, 0,
                   sizeof(handle->simulator_scene_value));
            simulator_apply_actual_state(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                false, false, 0U
            );
        } else if (handle->simulator_scene_value_present &&
                   handle->simulator_scene_enabled) {
            simulator_apply_actual_state(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                true, false, 0U
            );
        }
        return simulator_refresh_composition_status(handle);
    }
    handle->simulator_presentation_plan = pending->plan;
    handle->simulator_presentation_plan_present = true;
    if (!simulator_runtime_plan(
            &handle->simulator_presentation_plan, wire, &desired
        )) {
        return false;
    }
    observe = meiso_composition_coordinator_observe_desired(
        handle->simulator_coordinator,
        handle->simulator_desired_owner.incarnation,
        handle->simulator_snapshot.desired[
            MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
        ].state_version,
        &desired, now
    );
    if (observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING &&
        observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE &&
        observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE) {
        simulator_reject_actual(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
            MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
        );
        return simulator_refresh_composition_status(handle);
    }
    simulator_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
        true, false, 0U
    );
    if (observe == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING &&
        simulator_prepare_candidate(handle, now) ==
            SIMULATOR_PREPARE_INTERNAL) {
        return false;
    }
    return simulator_refresh_composition_status(handle);
}

static bool simulator_activate_presentation(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionScenePresentationPlan echoed;
    MeisoRuntimeDesiredState desired;
    MeisoCompositionCoordinatorObserveStatus observe;
    MeisoCompositionCoordinatorActivateStatus activate;
    MeisoCompositionSceneWorldRef world;
    MeisoCompositionEdgeDesiredView *presentation_desired;
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint64_t presentation_epoch;
    uint64_t scene_epoch;
    uint64_t echo_version;

    if (!meiso_composition_coordinator_candidate(
            handle->simulator_coordinator, &candidate
        ) || candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
        !candidate.prepared_token_present) {
        return false;
    }
    presentation_desired = &handle->simulator_snapshot.desired[
        MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
    ];
    if (presentation_desired->state_version == UINT64_MAX ||
        !handle->simulator_presentation_epoch_available ||
        !handle->simulator_scene_epoch_available) {
        return false;
    }
    echo_version = presentation_desired->state_version + 1U;
    echoed = handle->simulator_presentation_plan;
    echoed.prepared_token_echo_present = true;
    memmove(echoed.prepared_token_echo, candidate.prepared_token,
            sizeof(echoed.prepared_token_echo));
    if (!simulator_runtime_plan(&echoed, wire, &desired)) {
        return false;
    }
    observe = meiso_composition_coordinator_observe_desired(
        handle->simulator_coordinator,
        handle->simulator_desired_owner.incarnation,
        echo_version, &desired, now
    );
    if (observe !=
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING) {
        return false;
    }
    activate = meiso_composition_coordinator_activate_frame_boundary(
        handle->simulator_coordinator, candidate.prepared_token,
        now, &world
    );
    if (activate != MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED ||
        !simulator_take_epoch(
            &handle->simulator_presentation_epoch,
            &handle->simulator_presentation_epoch_available,
            &presentation_epoch
        ) ||
        !simulator_take_epoch(
            &handle->simulator_scene_epoch,
            &handle->simulator_scene_epoch_available, &scene_epoch
        )) {
        return false;
    }
    presentation_desired->state_version = echo_version;
    handle->simulator_presentation_plan = echoed;
    handle->simulator_presentation_activation_pending = false;
    simulator_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
        true, true, presentation_epoch
    );
    simulator_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
        true, true, scene_epoch
    );
    if (!simulator_clear_render_truth(handle)) {
        return false;
    }
    handle->simulator_render_result_pending = true;
    return simulator_refresh_composition_status(handle);
}

static bool simulator_publish_render_result(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionSceneWorldView world;
    MeisoCompositionSceneRenderResult result;
    MeisoCompositionCpuRasterJobRef job;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionEdgeInlineView stream;
    MeisoCompositionEdgeInlineView *current =
        &handle->simulator_snapshot.inline_streams[
            MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
        ];
    const uint64_t epoch = handle->simulator_snapshot.actual[
        MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
    ].production_epoch;
    uint64_t sequence;

    if (now.monotonic_ns == UINT64_MAX || epoch == 0U ||
        handle->simulator_render_result_sequence == UINT64_MAX ||
        current->received_count == UINT64_MAX ||
        (current->present && current->replaced_count == UINT64_MAX) ||
        !meiso_composition_scene_store_active(
            handle->simulator_scene_store, &world
        )) {
        return false;
    }
    sequence = handle->simulator_render_result_sequence + 1U;
    if (handle->simulator_cpu_raster_enabled) {
        if (!meiso_composition_scene_store_control_scope(
                handle->simulator_scene_store, &scope
            ) ||
            meiso_composition_cpu_raster_renderer_submit(
                handle->simulator_renderer, &scope, &world,
                handle->simulator_composition_clock_epoch, &job
            ) != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED ||
            meiso_composition_cpu_raster_renderer_render_next(
                handle->simulator_renderer, &job, now.monotonic_ns,
                now.monotonic_ns + 1U, &complete
            ) != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
            return false;
        }
        result = complete.result;
    } else if (meiso_composition_scene_store_fake_render(
                   handle->simulator_scene_store,
                   handle->simulator_composition_clock_epoch,
                   now.monotonic_ns, now.monotonic_ns + 1U, &result
               ) != MEISO_COMPOSITION_SCENE_RENDER_COMPLETE) {
        return false;
    }
    if (!meiso_composition_scene_store_record_result_published(
            handle->simulator_scene_store, &world.ref, epoch, sequence
        )) {
        return false;
    }
    stream = (MeisoCompositionEdgeInlineView){
        true,
        {handle->simulator_desired_owner, epoch, sequence},
        now,
        MEISO_STREAM_RECEIVE_FLAG_NONE,
        0U,
        current->received_count + 1U,
        current->replaced_count + (current->present ? 1U : 0U)
    };
    handle->simulator_render_result_sequence = sequence;
    handle->simulator_snapshot.render_result_present = true;
    handle->simulator_snapshot.render_result = result;
    *current = stream;
    handle->simulator_render_result_pending = false;
    handle->simulator_raster_preview_pending =
        handle->simulator_cpu_raster_enabled;
    return simulator_refresh_scene_status(handle);
}

static bool simulator_media_size_exact(
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    uint64_t pixels = (uint64_t)frame->width * frame->height;
    uint64_t bytes;

    switch (frame->pixel_format) {
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888:
        bytes = pixels * 4U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565:
    case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV:
        bytes = pixels * 2U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_NV12:
        if ((frame->width & 1U) != 0U || (frame->height & 1U) != 0U) {
            return false;
        }
        bytes = pixels + pixels / 2U;
        break;
    default:
        return false;
    }
    return bytes == frame->access_unit.size;
}

static bool simulator_frame_scene_fields_valid(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    if (frame->dependency_sequence != 0U) {
        return false;
    }
    if (frame->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        const bool fields_valid =
            meiso_owner_scope_valid(frame->applied_scene_owner) &&
            frame->applied_scene_owner.role == MEISO_OWNER_ROLE_HOST &&
            frame->applied_scene_state_version != 0U &&
            frame->active_plan_generation != 0U &&
            frame->world_generation != 0U;
        if (!fields_valid || handle == NULL ||
            !handle->simulator_composition_supported) {
            return fields_valid;
        }
        return handle->simulator_snapshot.actual[
                   MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
               ].production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
            meiso_owner_scope_equal(
                frame->applied_scene_owner,
                handle->simulator_desired_owner
            ) &&
            frame->applied_scene_state_version ==
                handle->simulator_snapshot.actual[
                    MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
                ].applied_desired.version &&
            handle->simulator_snapshot.presentation_status_present &&
            frame->active_plan_generation ==
                handle->simulator_snapshot.presentation_status
                    .active_plan_generation &&
            handle->simulator_snapshot.scene_status_present &&
            frame->world_generation ==
                handle->simulator_snapshot.scene_status.world_generation;
    }
    return frame->applied_scene_owner.role == 0U &&
        frame->applied_scene_owner.incarnation == 0U &&
        frame->applied_scene_state_version == 0U &&
        frame->active_plan_generation == 0U &&
        frame->world_generation == 0U;
}

static MeisoEdgeStatus simulator_commit_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    SimulatorMediaRoute *route;
    MeisoCompositionEdgeMediaRouteView *snapshot_route;
    const MeisoCompositionEdgeMediaActual *actual;
    uint64_t gap = 0U;
    const int index = media_index(frame->source);

    if (index < 0) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    route = &handle->simulator_media[(size_t)index];
    snapshot_route = &handle->simulator_snapshot.media[(size_t)index];
    actual = index == 0 ? &handle->simulator_snapshot.render_media_actual :
        &handle->simulator_snapshot.camera_media_actual;
    if (!handle->simulator_snapshot.edge.resource_graph_live ||
        !snapshot_route->actual_binding_ready ||
        frame->media_stream_epoch != actual->media_stream_epoch) {
        return MEISO_EDGE_NOT_READY;
    }
    if (route->borrowed) {
        return MEISO_EDGE_BUSY;
    }
    if (frame->frame_sequence == 0U ||
        frame->frame_sequence <=
            snapshot_route->published_sequence_high_water ||
        frame->producer_dropped_frames <
            snapshot_route->producer_drop_high_water) {
        return MEISO_EDGE_STALE;
    }
    if (frame->pixel_format != actual->effective.pixel_format ||
        frame->codec != actual->effective.codec ||
        frame->width != actual->effective.width ||
        frame->height != actual->effective.height ||
        (frame->flags & MEISO_COMPOSITION_MEDIA_FRAME_KEY) == 0U ||
        (frame->flags &
         (MeisoCompositionMediaFrameFlags)~
             (MEISO_COMPOSITION_MEDIA_FRAME_KEY |
              MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U ||
        !simulator_frame_scene_fields_valid(handle, frame) ||
        !simulator_media_size_exact(frame)) {
        return MEISO_EDGE_CONFLICT;
    }
    if (frame->access_unit.size > route->capacity) {
        return MEISO_EDGE_CAPACITY;
    }
    if (snapshot_route->published_sequence_high_water != 0U) {
        gap = frame->frame_sequence -
            snapshot_route->published_sequence_high_water - 1U;
    }
    if (gap > UINT64_MAX - snapshot_route->sequence_gap_frames ||
        (route->present &&
         snapshot_route->unread_latest_frames_dropped == UINT64_MAX)) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    if (route->present) {
        ++snapshot_route->unread_latest_frames_dropped;
    }
    memmove(route->buffer, frame->access_unit.data, frame->access_unit.size);
    route->frame = (MeisoEdgeMediaFrameView){
        {handle->continuity_serial, frame->source,
         frame->media_stream_epoch, frame->frame_sequence},
        frame->source_time,
        frame->applied_scene_owner,
        frame->applied_scene_state_version,
        frame->active_plan_generation,
        frame->world_generation,
        frame->dependency_sequence,
        frame->producer_dropped_frames,
        frame->pixel_format,
        frame->codec,
        frame->width,
        frame->height,
        frame->flags,
        {route->buffer, frame->access_unit.size}
    };
    route->present = true;
    snapshot_route->latest_present = true;
    snapshot_route->latest_frame_sequence = frame->frame_sequence;
    snapshot_route->latest_source_time = frame->source_time;
    snapshot_route->latest_access_unit_bytes =
        (uint32_t)frame->access_unit.size;
    snapshot_route->published_sequence_high_water = frame->frame_sequence;
    snapshot_route->sequence_gap_frames += gap;
    snapshot_route->producer_drop_high_water =
        frame->producer_dropped_frames;
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus simulator_publish_raster_preview(
    MeisoEdgeHandle *handle
)
{
    MeisoCompositionCpuRasterCompleteView complete;
    const MeisoCompositionEdgeMediaActual *actual =
        &handle->simulator_snapshot.render_media_actual;
    MeisoCompositionEdgeMediaRouteView *route =
        &handle->simulator_snapshot.media[0];
    MeisoCompositionEdgeSimulatorFrame frame;
    MeisoEdgeStatus status;

    if (!handle->simulator_cpu_raster_enabled ||
        !handle->simulator_raster_preview_pending ||
        !route->actual_binding_ready ||
        !handle->simulator_snapshot.render_media_actual_present) {
        return MEISO_EDGE_NOT_READY;
    }
    if (route->published_sequence_high_water == UINT64_MAX) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    if (!meiso_composition_cpu_raster_renderer_latest_complete(
            handle->simulator_renderer, &complete
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    memset(&frame, 0, sizeof(frame));
    frame.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    frame.media_stream_epoch = actual->media_stream_epoch;
    frame.frame_sequence = route->published_sequence_high_water + 1U;
    frame.source_time = (MeisoTimePoint){
        complete.result.source_clock_epoch,
        complete.result.render_end_monotonic_ns
    };
    frame.applied_scene_owner = handle->simulator_desired_owner;
    frame.applied_scene_state_version =
        complete.job.world.desired_state_version;
    frame.active_plan_generation = complete.job.world.plan_generation;
    frame.world_generation = complete.job.world.world_generation;
    frame.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    frame.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    frame.width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    frame.height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    frame.access_unit = complete.color_rgba8888;
    status = simulator_commit_frame(handle, &frame);
    if (status == MEISO_EDGE_OK) {
        handle->simulator_raster_preview_pending = false;
    }
    return status;
}

MeisoEdgeStatus meiso_composition_edge_simulator_step(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now,
    MeisoCompositionEdgeSimulatorTransition *out_transition
)
{
    MeisoCompositionEdgeSimulatorTransition transition =
        MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE;

    if (!handle_valid(handle) ||
        handle->backend_kind != EDGE_BACKEND_SIMULATOR ||
        out_transition == NULL || !meiso_time_point_valid(now) ||
        (handle->simulator_composition_supported &&
         now.clock_epoch != handle->simulator_composition_clock_epoch) ||
        !disjoint_from_handle_storage(handle, out_transition,
                                      sizeof(*out_transition))) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!handle->simulator_snapshot.edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    if (handle->simulator_mesh_pending) {
        if (!simulator_publish_mesh(handle)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED;
    } else if (handle->simulator_wifi_pending) {
        if (handle->simulator_wifi_enabled &&
            handle->simulator_snapshot.inline_streams[
                MEISO_COMPOSITION_EDGE_INLINE_WIFI
            ].received_count == UINT64_MAX) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        simulator_apply_wifi(handle, now);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED;
    } else if (handle->simulator_device_pending) {
        if (handle->simulator_device_enabled &&
            handle->simulator_snapshot.inline_streams[
                MEISO_COMPOSITION_EDGE_INLINE_DEVICE
            ].received_count == UINT64_MAX) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        simulator_apply_device(handle, now);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED;
    } else if (handle->simulator_scene_pending.pending) {
        if (!simulator_apply_scene_command(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED;
    } else if (handle->simulator_presentation_pending.pending) {
        if (!simulator_apply_presentation_command(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition =
            MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED;
    } else if (handle->simulator_scene_retry_ready) {
        if (!simulator_apply_scene_command(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED;
    } else if (handle->simulator_presentation_activation_pending) {
        if (!simulator_activate_presentation(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition =
            MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_ACTIVATED;
    } else if (handle->simulator_render_result_pending) {
        if (now.monotonic_ns == UINT64_MAX ||
            handle->simulator_render_result_sequence == UINT64_MAX) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        if (!simulator_publish_render_result(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition =
            MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED;
    } else if (handle->simulator_media_pending[0].pending) {
        simulator_apply_media(handle, 0U);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED;
    } else if (handle->simulator_media_pending[1].pending) {
        simulator_apply_media(handle, 1U);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED;
    } else if (handle->simulator_raster_preview_pending &&
               handle->simulator_snapshot.media[0].actual_binding_ready) {
        const MeisoEdgeStatus preview_status =
            simulator_publish_raster_preview(handle);
        if (preview_status == MEISO_EDGE_OK) {
            transition =
                MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_PREVIEW_PUBLISHED;
        } else if (preview_status != MEISO_EDGE_BUSY &&
                   preview_status != MEISO_EDGE_NOT_READY) {
            if (preview_status == MEISO_EDGE_COUNTER_EXHAUSTED) {
                return preview_status;
            }
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
    }
    *out_transition = transition;
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_simulator_publish_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    if (!handle_valid(handle) ||
        handle->backend_kind != EDGE_BACKEND_SIMULATOR || frame == NULL ||
        !optional_input_disjoint(handle, frame, sizeof(*frame)) ||
        !meiso_byte_view_valid(frame->access_unit) ||
        !optional_input_disjoint(handle, frame->access_unit.data,
                                 frame->access_unit.size) ||
        !disjoint_pair(frame, sizeof(*frame), frame->access_unit.data,
                       frame->access_unit.size) ||
        !meiso_time_point_valid(frame->source_time) ||
        frame->access_unit.size == 0U ||
        (frame->flags &
         ~(MEISO_COMPOSITION_MEDIA_FRAME_BEGIN |
           MEISO_COMPOSITION_MEDIA_FRAME_END |
           MEISO_COMPOSITION_MEDIA_FRAME_KEY |
           MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (handle->simulator_cpu_raster_enabled &&
        frame->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return MEISO_EDGE_CONFLICT;
    }
    return simulator_commit_frame(handle, frame);
}

MeisoEdgeStatus meiso_composition_edge_simulator_terminate(
    MeisoEdgeHandle *handle,
    MeisoEdgeTerminalReason reason
)
{
    MeisoEdgeSnapshot edge;

    if (!handle_valid(handle) ||
        handle->backend_kind != EDGE_BACKEND_SIMULATOR ||
        reason == MEISO_EDGE_TERMINAL_NONE ||
        reason > MEISO_EDGE_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    edge = (MeisoEdgeSnapshot){
        MEISO_VIRTUAL_EDGE_API_VERSION,
        handle->profile,
        MEISO_EDGE_PHASE_TERMINAL,
        reason,
        false,
        false,
        handle->continuity_serial,
        0U,
        0U,
        0U,
        0U
    };
    memset(&handle->simulator_snapshot, 0,
           sizeof(handle->simulator_snapshot));
    handle->simulator_snapshot.edge = edge;
    handle->simulator_wifi_pending = false;
    handle->simulator_device_pending = false;
    handle->simulator_mesh_pending = false;
    handle->simulator_mesh_size = 0U;
    memset(&handle->simulator_mesh_content_id, 0,
           sizeof(handle->simulator_mesh_content_id));
    memset(&handle->simulator_scene_pending, 0,
           sizeof(handle->simulator_scene_pending));
    handle->simulator_scene_retry_pending = false;
    handle->simulator_scene_retry_ready = false;
    handle->simulator_scene_value_present = false;
    handle->simulator_scene_enabled = false;
    memset(&handle->simulator_scene_value, 0,
           sizeof(handle->simulator_scene_value));
    memset(&handle->simulator_presentation_pending, 0,
           sizeof(handle->simulator_presentation_pending));
    handle->simulator_presentation_plan_present = false;
    handle->simulator_presentation_activation_pending = false;
    memset(&handle->simulator_presentation_plan, 0,
           sizeof(handle->simulator_presentation_plan));
    handle->simulator_render_result_pending = false;
    handle->simulator_raster_preview_pending = false;
    memset(handle->simulator_media_pending, 0,
           sizeof(handle->simulator_media_pending));
    memset(handle->simulator_media, 0, sizeof(handle->simulator_media));
    return MEISO_EDGE_OK;
}

static bool simulator_catalog_equal(
    const MeisoCompositionSessionV3CatalogConfig *lhs,
    const MeisoCompositionSessionV3CatalogConfig *rhs
)
{
    uint8_t lhs_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    uint8_t rhs_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    size_t lhs_size;
    size_t rhs_size;

    if (lhs == NULL || rhs == NULL ||
        lhs->wifi_incarnation != rhs->wifi_incarnation ||
        lhs->presentation_incarnation != rhs->presentation_incarnation ||
        lhs->scene_incarnation != rhs->scene_incarnation ||
        lhs->device_telemetry_incarnation !=
            rhs->device_telemetry_incarnation ||
        lhs->render_media_incarnation != rhs->render_media_incarnation ||
        lhs->camera_media_incarnation != rhs->camera_media_incarnation) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    if (meiso_wifi_telemetry_capability_encode(
            &lhs->wifi_capability,
            (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        meiso_wifi_telemetry_capability_encode(
            &rhs->wifi_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK || lhs_size != rhs_size ||
        memcmp(lhs_wire, rhs_wire, lhs_size) != 0) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    if (meiso_device_telemetry_capability_encode(
            &lhs->device_telemetry_capability,
            (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        meiso_device_telemetry_capability_encode(
            &rhs->device_telemetry_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK || lhs_size != rhs_size ||
        memcmp(lhs_wire, rhs_wire, lhs_size) != 0) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    if (meiso_composition_media_capability_encode(
            &lhs->render_media_capability,
            (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        meiso_composition_media_capability_encode(
            &rhs->render_media_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK || lhs_size != rhs_size ||
        memcmp(lhs_wire, rhs_wire, lhs_size) != 0) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    return meiso_composition_media_capability_encode(
               &lhs->camera_media_capability,
               (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
           ) == MEISO_COMPOSITION_SESSION_V3_OK &&
        meiso_composition_media_capability_encode(
            &rhs->camera_media_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) == MEISO_COMPOSITION_SESSION_V3_OK && lhs_size == rhs_size &&
        memcmp(lhs_wire, rhs_wire, lhs_size) == 0;
}

static bool simulator_wifi_sample_equal(
    const MeisoWifiTelemetrySample *lhs,
    const MeisoWifiTelemetrySample *rhs
)
{
    uint8_t lhs_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t rhs_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t lhs_size = 0U;
    size_t rhs_size = 0U;

    return lhs != NULL && rhs != NULL &&
        meiso_wifi_telemetry_sample_encode(
            lhs, (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        meiso_wifi_telemetry_sample_encode(
            rhs, (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK && lhs_size == rhs_size &&
        memcmp(lhs_wire, rhs_wire, lhs_size) == 0;
}

static bool simulator_device_sample_equal(
    const MeisoDeviceTelemetrySample *lhs,
    const MeisoDeviceTelemetrySample *rhs
)
{
    uint8_t lhs_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t rhs_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t lhs_size = 0U;
    size_t rhs_size = 0U;

    return lhs != NULL && rhs != NULL &&
        meiso_device_telemetry_sample_encode(
            lhs, (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) == MEISO_DEVICE_TELEMETRY_CODEC_OK &&
        meiso_device_telemetry_sample_encode(
            rhs, (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) == MEISO_DEVICE_TELEMETRY_CODEC_OK && lhs_size == rhs_size &&
        memcmp(lhs_wire, rhs_wire, lhs_size) == 0;
}

bool meiso_composition_edge_internal_simulator_storage_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
)
{
    return handle_valid(handle) &&
        handle->backend_kind == EDGE_BACKEND_SIMULATOR &&
        disjoint_from_handle_storage(handle, pointer, size);
}

bool meiso_composition_edge_internal_simulator_fresh_matches(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    size_t i;

    if (!handle_valid(handle) ||
        handle->backend_kind != EDGE_BACKEND_SIMULATOR || config == NULL ||
        handle->catalog_generation != config->catalog_generation ||
        handle->continuity_serial != config->continuity_serial ||
        !meiso_owner_scope_equal(handle->simulator_desired_owner,
                                 config->desired_owner) ||
        handle->simulator_wifi_epoch != config->wifi_production_epoch ||
        handle->simulator_device_supported !=
            config->device_telemetry_enabled ||
        handle->simulator_device_epoch != config->device_production_epoch ||
        handle->simulator_device_epoch_available !=
            config->device_telemetry_enabled ||
        handle->simulator_composition_supported !=
            config->composition_enabled ||
        handle->simulator_cpu_raster_enabled !=
            config->cpu_raster_preview_enabled ||
        handle->simulator_blob_slot_count !=
            config->composition_blob_slot_count ||
        handle->simulator_composition_clock_epoch !=
            config->composition_clock_epoch ||
        handle->simulator_presentation_epoch !=
            config->presentation_production_epoch ||
        handle->simulator_scene_epoch != config->scene_production_epoch ||
        handle->simulator_presentation_epoch_available !=
            config->composition_enabled ||
        handle->simulator_scene_epoch_available !=
            config->composition_enabled ||
        handle->simulator_next_plan_generation != 1U ||
        handle->simulator_plan_generation_available !=
            config->composition_enabled ||
        handle->simulator_composition_initialized ||
        handle->simulator_mesh_pending ||
        handle->simulator_scene_pending.pending ||
        handle->simulator_scene_retry_pending ||
        handle->simulator_scene_retry_ready ||
        handle->simulator_scene_value_present ||
        handle->simulator_presentation_pending.pending ||
        handle->simulator_presentation_plan_present ||
        handle->simulator_presentation_activation_pending ||
        handle->simulator_render_result_pending ||
        handle->simulator_raster_preview_pending ||
        handle->simulator_published_blob_bytes != 0U ||
        handle->simulator_render_result_sequence != 0U ||
        handle->simulator_media_epoch[0] !=
            config->render_media_stream_epoch ||
        handle->simulator_media_epoch[1] !=
            config->camera_media_stream_epoch ||
        !handle->simulator_wifi_epoch_available ||
        !handle->simulator_media_epoch_available[0] ||
        !handle->simulator_media_epoch_available[1] ||
        handle->simulator_next_command_id != 0U ||
        handle->simulator_wifi_pending ||
        handle->simulator_device_pending ||
        handle->simulator_snapshot.edge.phase !=
            MEISO_EDGE_PHASE_BOOTSTRAPPING ||
        handle->simulator_snapshot.edge.online ||
        handle->simulator_snapshot.edge.resource_graph_live ||
        handle->simulator_snapshot.wifi_effective_present ||
        handle->simulator_snapshot.wifi_sample_present ||
        handle->simulator_snapshot.device_effective_present ||
        handle->simulator_snapshot.device_sample_present ||
        handle->simulator_snapshot.render_media_actual_present ||
        handle->simulator_snapshot.camera_media_actual_present ||
        !simulator_catalog_equal(&handle->simulator_catalog,
                                 &config->catalog) ||
        !simulator_wifi_sample_equal(&handle->simulator_wifi_sample,
                                     &config->wifi_sample) ||
        (config->device_telemetry_enabled &&
         !simulator_device_sample_equal(&handle->simulator_device_sample,
                                        &config->device_sample))) {
        return false;
    }
    for (i = 0U; i < MEISO_COMPOSITION_EDGE_DESIRED_COUNT; ++i) {
        if (handle->simulator_snapshot.desired[i].requested ||
            handle->simulator_snapshot.actual[i].present) {
            return false;
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT; ++i) {
        if (handle->simulator_media_pending[i].pending ||
            handle->simulator_media[i].present ||
            handle->simulator_media[i].borrowed ||
            handle->simulator_snapshot.media[i].latest_present) {
            return false;
        }
    }
    return true;
}
