#include "meiso_composition_edge_private.h"

/* Portable facade owns argument/borrow validation and delegates all state. */

#include <limits.h>
#include <stdint.h>
#include <string.h>

bool meiso_composition_edge_private_checked_range(
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

bool meiso_composition_edge_private_checked_add_size(
    size_t lhs,
    size_t rhs,
    size_t *out
)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool meiso_composition_edge_private_checked_align_size(
    size_t value,
    size_t alignment,
    size_t *out
)
{
    size_t remainder;
    size_t padding;

    if (alignment == 0U || out == NULL) {
        return false;
    }
    remainder = value % alignment;
    padding = remainder == 0U ? 0U : alignment - remainder;
    return meiso_composition_edge_private_checked_add_size(
        value, padding, out
    );
}

bool meiso_composition_edge_private_ranges_overlap(
    uintptr_t lhs_begin,
    uintptr_t lhs_end,
    uintptr_t rhs_begin,
    uintptr_t rhs_end
)
{
    return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

bool meiso_composition_edge_private_range_inside(
    uintptr_t outer_begin,
    uintptr_t outer_end,
    uintptr_t inner_begin,
    uintptr_t inner_end
)
{
    return inner_begin >= outer_begin && inner_end <= outer_end;
}

bool meiso_composition_edge_private_handle_valid(
    const MeisoEdgeHandle *handle
)
{
    uintptr_t runtime_begin;
    uintptr_t runtime_end;

    if (handle == NULL ||
        handle->magic != MEISO_COMPOSITION_EDGE_HANDLE_MAGIC ||
        handle->backend_ops == NULL ||
        (handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_HOST &&
         handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR)) {
        return false;
    }
    if (handle->arena_begin != (uintptr_t)handle ||
        handle->arena_end <= handle->arena_begin ||
        handle->arena_end - handle->arena_begin < sizeof(*handle) ||
        handle->catalog_generation == 0U ||
        handle->continuity_serial == 0U) {
        return false;
    }
    if (handle->backend_kind == MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR) {
        return handle->host_arena_begin == 0U &&
            handle->host_arena_end == 0U;
    }
    return handle->host_arena_end > handle->host_arena_begin &&
        handle->backend_runtime.remote_host_runtime != NULL &&
        meiso_composition_edge_private_checked_range(
            handle->backend_runtime.remote_host_runtime,
            1U,
            &runtime_begin,
            &runtime_end
        ) &&
        meiso_composition_edge_private_range_inside(
            handle->host_arena_begin,
            handle->host_arena_end,
            runtime_begin,
            runtime_end
        );
}

bool meiso_composition_edge_private_storage_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
)
{
    uintptr_t begin;
    uintptr_t end;

    return meiso_composition_edge_private_checked_range(
               pointer, size, &begin, &end
           ) &&
        !meiso_composition_edge_private_ranges_overlap(
            begin, end, handle->arena_begin, handle->arena_end
        ) &&
        (handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_HOST ||
         !meiso_composition_edge_private_ranges_overlap(
             begin,
             end,
             handle->host_arena_begin,
             handle->host_arena_end
         ));
}

bool meiso_composition_edge_private_optional_input_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
)
{
    if (pointer == NULL && size == 0U) {
        return true;
    }
    return meiso_composition_edge_private_storage_disjoint(
        handle, pointer, size
    );
}

bool meiso_composition_edge_private_disjoint_pair(
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

    if ((lhs == NULL && lhs_size == 0U) ||
        (rhs == NULL && rhs_size == 0U)) {
        return true;
    }
    return meiso_composition_edge_private_checked_range(
               lhs, lhs_size, &lhs_begin, &lhs_end
           ) &&
        meiso_composition_edge_private_checked_range(
            rhs, rhs_size, &rhs_begin, &rhs_end
        ) &&
        !meiso_composition_edge_private_ranges_overlap(
            lhs_begin, lhs_end, rhs_begin, rhs_end
        );
}

int meiso_composition_edge_private_media_index(
    MeisoCompositionMediaSource source
)
{
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return 0;
    }
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return 1;
    }
    return -1;
}

static MeisoEdgeStatus meiso_composition_edge_private_read_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
)
{
    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_snapshot == NULL || handle->backend_ops->snapshot == NULL) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    return handle->backend_ops->snapshot(handle, out_snapshot);
}

MeisoEdgeStatus meiso_edge_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoEdgeSnapshot *out_snapshot
)
{
    MeisoCompositionEdgeSnapshot composition;
    MeisoEdgeStatus status;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_snapshot == NULL ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_snapshot, sizeof(*out_snapshot)
        )) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    status = meiso_composition_edge_private_read_snapshot(
        handle, &composition
    );
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    memmove(out_snapshot, &composition.edge, sizeof(*out_snapshot));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_edge_operation_supported(
    const MeisoEdgeHandle *handle,
    MeisoEdgeOperationMask operation,
    bool *out_supported
)
{
    MeisoCompositionEdgeSnapshot composition;
    MeisoEdgeStatus status;
    bool candidate;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_supported == NULL || operation == 0U ||
        (operation & MEISO_EDGE_OPERATION_ALL) != operation ||
        (operation & (operation - 1U)) != 0U ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_supported, sizeof(*out_supported)
        )) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    status = meiso_composition_edge_private_read_snapshot(
        handle, &composition
    );
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (composition.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!composition.edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    candidate = (composition.edge.operations & operation) != 0U;
    *out_supported = candidate;
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_edge_endpoint_at(
    const MeisoEdgeHandle *handle,
    size_t index,
    MeisoEdgeEndpointView *out_endpoint
)
{
    MeisoCompositionEdgeSnapshot composition;
    MeisoEdgeEndpointView candidate;
    MeisoEdgeStatus status;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_endpoint == NULL ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    status = meiso_composition_edge_private_read_snapshot(
        handle, &composition
    );
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (composition.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!composition.edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    if (index >= MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT) {
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
    MeisoCompositionEdgeSnapshot composition;
    MeisoEdgeResourceView candidate;
    const MeisoCompositionProfileRouteDescriptor *route;
    MeisoEdgeStatus status;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_resource == NULL ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_resource, sizeof(*out_resource)
        )) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    status = meiso_composition_edge_private_read_snapshot(
        handle, &composition
    );
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (composition.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!composition.edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    if (index >= MEISO_COMPOSITION_PROFILE_ROUTE_COUNT) {
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

static bool meiso_composition_edge_private_typed_command_valid(
    const MeisoEdgeHandle *handle,
    const void *input,
    size_t input_size,
    const MeisoEdgeCommandReceipt *out_receipt
)
{
    return meiso_composition_edge_private_handle_valid(handle) &&
        out_receipt != NULL &&
        meiso_composition_edge_private_optional_input_disjoint(
            handle, input, input_size
        ) &&
        meiso_composition_edge_private_storage_disjoint(
            handle, out_receipt, sizeof(*out_receipt)
        ) &&
        meiso_composition_edge_private_disjoint_pair(
            input, input_size, out_receipt, sizeof(*out_receipt)
        );
}

MeisoEdgeStatus meiso_composition_edge_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_receipt == NULL || !meiso_byte_view_valid(canonical_mesh) ||
        !meiso_composition_edge_private_optional_input_disjoint(
            handle, canonical_mesh.data, canonical_mesh.size
        ) ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_receipt, sizeof(*out_receipt)
        ) ||
        !meiso_composition_edge_private_disjoint_pair(
            canonical_mesh.data,
            canonical_mesh.size,
            out_receipt,
            sizeof(*out_receipt)
        ) || handle->backend_ops->put_mesh == NULL) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    return handle->backend_ops->put_mesh(
        handle, canonical_mesh, out_receipt
    );
}

#define MEISO_DEFINE_EDGE_TYPED_COMMAND(function_name, ops_member, value_type) \
MeisoEdgeStatus function_name(                                                  \
    MeisoEdgeHandle *handle,                                                     \
    bool enabled,                                                               \
    const value_type *value,                                                     \
    MeisoEdgeCommandReceipt *out_receipt                                         \
)                                                                               \
{                                                                               \
    const size_t input_size = value == NULL ? 0U : sizeof(*value);              \
    if (!meiso_composition_edge_private_typed_command_valid(                    \
            handle, value, input_size, out_receipt                              \
        ) || handle->backend_ops->ops_member == NULL) {                         \
        return MEISO_EDGE_INVALID_ARGUMENT;                                     \
    }                                                                           \
    return handle->backend_ops->ops_member(                                     \
        handle, enabled, value, out_receipt                                     \
    );                                                                          \
}

MEISO_DEFINE_EDGE_TYPED_COMMAND(
    meiso_composition_edge_set_scene,
    set_scene,
    MeisoCompositionSceneDesired
)
MEISO_DEFINE_EDGE_TYPED_COMMAND(
    meiso_composition_edge_set_presentation,
    set_presentation,
    MeisoCompositionEdgePresentationSettings
)
MEISO_DEFINE_EDGE_TYPED_COMMAND(
    meiso_composition_edge_set_wifi,
    set_wifi,
    MeisoWifiTelemetryDesiredParameters
)
MEISO_DEFINE_EDGE_TYPED_COMMAND(
    meiso_composition_edge_set_device,
    set_device,
    MeisoDeviceTelemetryParameters
)
MEISO_DEFINE_EDGE_TYPED_COMMAND(
    meiso_composition_edge_set_render_media,
    set_render_media,
    MeisoCompositionMediaDesired
)
MEISO_DEFINE_EDGE_TYPED_COMMAND(
    meiso_composition_edge_set_camera_media,
    set_camera_media,
    MeisoCompositionMediaDesired
)

#undef MEISO_DEFINE_EDGE_TYPED_COMMAND

MeisoEdgeStatus meiso_composition_edge_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
)
{
    MeisoCompositionEdgeSnapshot candidate;
    MeisoEdgeStatus status;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_snapshot == NULL ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_snapshot, sizeof(*out_snapshot)
        )) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    status = meiso_composition_edge_private_read_snapshot(handle, &candidate);
    if (status != MEISO_EDGE_OK) {
        return status;
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
    if (!meiso_composition_edge_private_handle_valid(handle) ||
        out_frame == NULL ||
        meiso_composition_edge_private_media_index(source) < 0 ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, out_frame, sizeof(*out_frame)
        ) || handle->backend_ops->media_latest == NULL) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    return handle->backend_ops->media_latest(handle, source, out_frame);
}

MeisoEdgeStatus meiso_composition_edge_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
)
{
    if (!meiso_composition_edge_private_handle_valid(handle) ||
        expected_frame == NULL ||
        meiso_composition_edge_private_media_index(expected_frame->source) < 0 ||
        !meiso_composition_edge_private_storage_disjoint(
            handle, expected_frame, sizeof(*expected_frame)
        ) || handle->backend_ops->media_release == NULL) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    return handle->backend_ops->media_release(handle, expected_frame);
}
