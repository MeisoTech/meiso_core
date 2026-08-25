#include "composition_virtual_edge_host.h"
#include "composition_virtual_edge_host_internal.h"
#include "meiso_composition_edge_simulator.h"

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
    SimulatorMediaPending
        simulator_media_pending[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    SimulatorMediaRoute
        simulator_media[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
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

static void simulator_apply_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
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
        desired->enabled,
        desired->enabled ? MEISO_RUNTIME_PRODUCTION_RUNNING :
            MEISO_RUNTIME_PRODUCTION_STOPPED,
        desired->enabled ? MEISO_RUNTIME_PRODUCTION_REASON_NONE :
            MEISO_RUNTIME_PRODUCTION_REASON_DISABLED,
        desired->enabled ? production_epoch : 0U,
        true
    };
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
        preflight = simulator_command_preflight(handle);
        return preflight == MEISO_EDGE_OK ? MEISO_EDGE_UNSUPPORTED : preflight;
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
        preflight = simulator_command_preflight(handle);
        return preflight == MEISO_EDGE_OK ? MEISO_EDGE_UNSUPPORTED : preflight;
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
        preflight = simulator_command_preflight(handle);
        return preflight == MEISO_EDGE_OK ? MEISO_EDGE_UNSUPPORTED : preflight;
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
                     preflight = simulator_command_preflight(handle);
                     return preflight == MEISO_EDGE_OK ?
                         MEISO_EDGE_UNSUPPORTED : preflight)
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

size_t meiso_composition_edge_simulator_alignment_required(void)
{
    return _Alignof(MeisoEdgeHandle);
}

size_t meiso_composition_edge_simulator_memory_required(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    size_t required;

    if (config == NULL ||
        !checked_add_size(sizeof(MeisoEdgeHandle),
                          config->catalog.render_media_capability
                              .max_access_unit_bytes,
                          &required) ||
        !checked_add_size(required,
                          config->catalog.camera_media_capability
                              .max_access_unit_bytes,
                          &required)) {
        return 0U;
    }
    return required;
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
    size_t i;
    size_t wifi_sample_wire_size = 0U;
    uint8_t wifi_sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t *render_buffer;
    uint8_t *camera_buffer;

    required = meiso_composition_edge_simulator_memory_required(config);
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
    if ((arena_begin % _Alignof(MeisoEdgeHandle)) != 0U) {
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
    render_buffer = (uint8_t *)arena + sizeof(MeisoEdgeHandle);
    camera_buffer = render_buffer +
        config->catalog.render_media_capability.max_access_unit_bytes;
    candidate.simulator_media[0].buffer = render_buffer;
    candidate.simulator_media[0].capacity =
        config->catalog.render_media_capability.max_access_unit_bytes;
    candidate.simulator_media[1].buffer = camera_buffer;
    candidate.simulator_media[1].capacity =
        config->catalog.camera_media_capability.max_access_unit_bytes;
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
    edge->phase = MEISO_EDGE_PHASE_READY;
    edge->online = true;
    edge->resource_graph_live = true;
    edge->catalog_generation = handle->catalog_generation;
    edge->operations = SIMULATOR_SUPPORTED_OPERATIONS;
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
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    if (frame->dependency_sequence != 0U) {
        return false;
    }
    if (frame->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return meiso_owner_scope_valid(frame->applied_scene_owner) &&
            frame->applied_scene_owner.role == MEISO_OWNER_ROLE_HOST &&
            frame->applied_scene_state_version != 0U &&
            frame->active_plan_generation != 0U &&
            frame->world_generation != 0U;
    }
    return frame->applied_scene_owner.role == 0U &&
        frame->applied_scene_owner.incarnation == 0U &&
        frame->applied_scene_state_version == 0U &&
        frame->active_plan_generation == 0U &&
        frame->world_generation == 0U;
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
    if (handle->simulator_wifi_pending) {
        if (handle->simulator_wifi_enabled &&
            handle->simulator_snapshot.inline_streams[
                MEISO_COMPOSITION_EDGE_INLINE_WIFI
            ].received_count == UINT64_MAX) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        simulator_apply_wifi(handle, now);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED;
    } else if (handle->simulator_media_pending[0].pending) {
        simulator_apply_media(handle, 0U);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED;
    } else if (handle->simulator_media_pending[1].pending) {
        simulator_apply_media(handle, 1U);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED;
    }
    *out_transition = transition;
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_simulator_publish_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    SimulatorMediaRoute *route;
    MeisoCompositionEdgeMediaRouteView *snapshot_route;
    const MeisoCompositionEdgeMediaActual *actual;
    uint64_t gap = 0U;
    int index;

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
    index = media_index(frame->source);
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
        frame->frame_sequence <= snapshot_route->published_sequence_high_water ||
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
        !simulator_frame_scene_fields_valid(frame) ||
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
        handle->simulator_media_epoch[0] !=
            config->render_media_stream_epoch ||
        handle->simulator_media_epoch[1] !=
            config->camera_media_stream_epoch ||
        !handle->simulator_wifi_epoch_available ||
        !handle->simulator_media_epoch_available[0] ||
        !handle->simulator_media_epoch_available[1] ||
        handle->simulator_next_command_id != 0U ||
        handle->simulator_wifi_pending ||
        handle->simulator_snapshot.edge.phase !=
            MEISO_EDGE_PHASE_BOOTSTRAPPING ||
        handle->simulator_snapshot.edge.online ||
        handle->simulator_snapshot.edge.resource_graph_live ||
        handle->simulator_snapshot.wifi_effective_present ||
        handle->simulator_snapshot.wifi_sample_present ||
        handle->simulator_snapshot.render_media_actual_present ||
        handle->simulator_snapshot.camera_media_actual_present ||
        !simulator_catalog_equal(&handle->simulator_catalog,
                                 &config->catalog) ||
        !simulator_wifi_sample_equal(&handle->simulator_wifi_sample,
                                     &config->wifi_sample)) {
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
