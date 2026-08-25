#include "composition_virtual_edge_host.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define EDGE_HANDLE_MAGIC UINT64_C(0x4d4549534f454447)
#define EDGE_SUPPORTED_OPERATIONS                                             \
    (MEISO_EDGE_OPERATION_STATE_READ | MEISO_EDGE_OPERATION_STATE_SUBMIT |   \
     MEISO_EDGE_OPERATION_STREAM_READ | MEISO_EDGE_OPERATION_BLOB_PUT |      \
     MEISO_EDGE_OPERATION_COMPOSITION | MEISO_EDGE_OPERATION_MEDIA_READ)

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

struct MeisoEdgeHandle {
    uint64_t magic;
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

    return handle != NULL && handle->magic == EDGE_HANDLE_MAGIC &&
        handle->arena_begin == (uintptr_t)handle &&
        handle->arena_end > handle->arena_begin &&
        handle->arena_end - handle->arena_begin >= sizeof(*handle) &&
        handle->host_arena_end > handle->host_arena_begin &&
        handle->host_app != NULL &&
        checked_range(handle->host_app, 1U, &app_begin, &app_end) &&
        range_inside(handle->host_arena_begin, handle->host_arena_end,
                     app_begin, app_end) &&
        handle->catalog_generation != 0U &&
        handle->continuity_serial != 0U;
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
        !ranges_overlap(begin, end, handle->host_arena_begin,
                        handle->host_arena_end);
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
    if (!read_host_snapshot(handle, &host, &candidate)) {
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
    if (!read_host_snapshot(handle, &host, &edge)) {
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
    if (!read_host_snapshot(handle, &host, &edge)) {
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
    if (!read_host_snapshot(handle, &host, &edge)) {
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

#define DEFINE_TYPED_COMMAND(function_name, host_function, value_type)           \
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
                     MeisoWifiTelemetryDesiredParameters)
DEFINE_TYPED_COMMAND(meiso_composition_edge_set_device,
                     meiso_composition_host_app_set_device,
                     MeisoDeviceTelemetryParameters)
DEFINE_TYPED_COMMAND(meiso_composition_edge_set_render_media,
                     meiso_composition_host_app_set_render_media,
                     MeisoCompositionMediaDesired)
DEFINE_TYPED_COMMAND(meiso_composition_edge_set_camera_media,
                     meiso_composition_host_app_set_camera_media,
                     MeisoCompositionMediaDesired)

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
