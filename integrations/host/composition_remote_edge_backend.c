#include "meiso_composition_remote_edge_private.h"

/* Remote backend owns HostRuntime status and command projection into Client. */

#include <string.h>

#define MEISO_ENUM_VALUE(value) ((uint32_t)(value))

_Static_assert(
    MEISO_COMPOSITION_EDGE_DESIRED_COUNT ==
        MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT,
    "desired channel count drift"
);
_Static_assert(
    MEISO_COMPOSITION_EDGE_ACTUAL_COUNT ==
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT,
    "actual channel count drift"
);
_Static_assert(
    MEISO_COMPOSITION_EDGE_INLINE_STREAM_COUNT ==
        MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT,
    "inline channel count drift"
);
_Static_assert(
    MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT ==
        MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT,
    "media route count drift"
);
_Static_assert(
    MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_PUT_MESH) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_PUT_MESH
            ) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_SET_SCENE) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_SCENE
            ) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_SET_PRESENTATION) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_PRESENTATION
            ) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_SET_WIFI) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_WIFI
            ) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_SET_DEVICE) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_DEVICE
            ) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_SET_RENDER_MEDIA) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_RENDER_MEDIA
            ) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_COMMAND_SET_CAMERA_MEDIA) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_CAMERA_MEDIA
            ),
    "command mapping drift"
);
_Static_assert(
    MEISO_ENUM_VALUE(MEISO_EDGE_LOCAL_QUEUED) ==
            MEISO_ENUM_VALUE(MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED) &&
        MEISO_ENUM_VALUE(MEISO_EDGE_LOCAL_REPLACED_PENDING) ==
            MEISO_ENUM_VALUE(
                MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING
            ),
    "local receipt mapping drift"
);

#undef MEISO_ENUM_VALUE

MeisoCompositionHostRuntime *
meiso_composition_remote_edge_private_runtime(MeisoEdgeHandle *handle)
{
    return handle == NULL ? NULL :
        handle->backend_runtime.remote_host_runtime;
}

const MeisoCompositionHostRuntime *
meiso_composition_remote_edge_private_const_runtime(
    const MeisoEdgeHandle *handle
)
{
    return handle == NULL ? NULL :
        handle->backend_runtime.remote_host_runtime;
}

static MeisoEdgeTerminalReason
meiso_composition_remote_edge_private_terminal_reason(
    MeisoCompositionHostRuntimeTerminalReason reason
)
{
    switch (reason) {
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_TRANSPORT:
        return MEISO_EDGE_TERMINAL_TRANSPORT;
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_SESSION:
        return MEISO_EDGE_TERMINAL_SESSION;
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CATALOG_CHANGED:
        return MEISO_EDGE_TERMINAL_CATALOG_DISCONTINUITY;
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT:
        return MEISO_EDGE_TERMINAL_LOCAL_ABORT;
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE:
        return MEISO_EDGE_TERMINAL_INTERNAL_FAILURE;
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_NONE:
    default:
        return MEISO_EDGE_TERMINAL_INTERNAL_FAILURE;
    }
}

bool meiso_composition_remote_edge_private_base_snapshot(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionHostRuntimeSnapshot *host,
    MeisoEdgeSnapshot *out_edge
)
{
    bool catalog_mismatch;

    if (handle == NULL || host == NULL || out_edge == NULL) {
        return false;
    }
    memset(out_edge, 0, sizeof(*out_edge));
    out_edge->api_version = MEISO_HOST_EDGE_API_VERSION;
    out_edge->profile = handle->profile;
    out_edge->continuity_serial = handle->continuity_serial;
    catalog_mismatch = host->control_ready &&
        host->active_catalog_generation != handle->catalog_generation;
    if (host->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL ||
        catalog_mismatch) {
        out_edge->phase = MEISO_EDGE_PHASE_TERMINAL;
        out_edge->terminal_reason = catalog_mismatch ?
            MEISO_EDGE_TERMINAL_CATALOG_DISCONTINUITY :
            meiso_composition_remote_edge_private_terminal_reason(
                host->terminal_reason
            );
        return true;
    }
    out_edge->online =
        host->control_session_state == MEISO_SESSION_STATE_READY;
    if (host->control_ready &&
        host->active_catalog_generation == handle->catalog_generation) {
        out_edge->phase = MEISO_EDGE_PHASE_READY;
        out_edge->resource_graph_live = true;
        out_edge->catalog_generation = handle->catalog_generation;
        out_edge->operations = MEISO_COMPOSITION_EDGE_HOST_OPERATIONS;
        out_edge->endpoint_count = MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT;
        out_edge->resource_count = MEISO_COMPOSITION_PROFILE_ROUTE_COUNT;
    } else {
        out_edge->phase = MEISO_EDGE_PHASE_BOOTSTRAPPING;
    }
    return true;
}

bool meiso_composition_remote_edge_private_read_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionHostRuntimeSnapshot *out_host,
    MeisoEdgeSnapshot *out_edge
)
{
    return meiso_composition_edge_private_handle_valid(handle) &&
        handle->backend_kind == MEISO_COMPOSITION_EDGE_BACKEND_HOST &&
        out_host != NULL && out_edge != NULL &&
        meiso_composition_host_runtime_snapshot(
            meiso_composition_remote_edge_private_const_runtime(handle),
            out_host
        ) &&
        meiso_composition_remote_edge_private_base_snapshot(
            handle, out_host, out_edge
        );
}

MeisoEdgeStatus meiso_composition_remote_edge_private_status(
    MeisoCompositionHostRuntimeStatus status
)
{
    switch (status) {
    case MEISO_COMPOSITION_HOST_RUNTIME_OK:
        return MEISO_EDGE_OK;
    case MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT:
        return MEISO_EDGE_INVALID_ARGUMENT;
    case MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE:
    case MEISO_COMPOSITION_HOST_RUNTIME_FRAME_MISMATCH:
        return MEISO_EDGE_STALE;
    case MEISO_COMPOSITION_HOST_RUNTIME_CAPACITY:
        return MEISO_EDGE_CAPACITY;
    case MEISO_COMPOSITION_HOST_RUNTIME_BUSY:
        return MEISO_EDGE_BUSY;
    case MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT:
        return MEISO_EDGE_CONFLICT;
    case MEISO_COMPOSITION_HOST_RUNTIME_NOT_READY:
        return MEISO_EDGE_NOT_READY;
    case MEISO_COMPOSITION_HOST_RUNTIME_NO_FRAME:
        return MEISO_EDGE_NO_DATA;
    case MEISO_COMPOSITION_HOST_RUNTIME_COUNTER_EXHAUSTED:
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL:
        return MEISO_EDGE_TERMINAL;
    case MEISO_COMPOSITION_HOST_RUNTIME_INTERNAL_FAILURE:
    default:
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
}

bool meiso_composition_remote_edge_private_receipt(
    const MeisoCompositionHostRuntimeCommandReceipt *host,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    if (host == NULL || out_receipt == NULL ||
        host->kind < MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_PUT_MESH ||
        host->kind > MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_CAMERA_MEDIA ||
        (host->local_result != MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED &&
         host->local_result !=
             MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING) ||
        (host->desired_state_present &&
         host->desired_channel >=
             MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT)) {
        return false;
    }
    memset(out_receipt, 0, sizeof(*out_receipt));
    out_receipt->command_id = host->command_id;
    out_receipt->kind = host->kind;
    out_receipt->local_result = host->local_result;
    out_receipt->replaced_command_present = host->replaced_command_present;
    out_receipt->replaced_command_id = host->replaced_command_id;
    out_receipt->desired_state_present = host->desired_state_present;
    out_receipt->desired_channel = host->desired_channel;
    out_receipt->desired_state_version = host->desired_state_version;
    out_receipt->plan_generation_present = host->plan_generation_present;
    out_receipt->plan_generation = host->plan_generation;
    out_receipt->mesh_content_id_present = host->mesh_content_id_present;
    out_receipt->mesh_content_id = host->mesh_content_id;
    return true;
}

static MeisoEdgeStatus meiso_composition_remote_edge_private_preflight(
    const MeisoEdgeHandle *handle
)
{
    MeisoCompositionHostRuntimeSnapshot host;
    MeisoEdgeSnapshot edge;

    if (!meiso_composition_remote_edge_private_read_snapshot(
            handle, &host, &edge
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    return edge.phase == MEISO_EDGE_PHASE_TERMINAL ?
        MEISO_EDGE_TERMINAL : MEISO_EDGE_OK;
}

static MeisoEdgeStatus meiso_composition_remote_edge_private_finish_command(
    MeisoCompositionHostRuntimeStatus host_status,
    const MeisoCompositionHostRuntimeCommandReceipt *host_receipt,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeCommandReceipt candidate;
    MeisoEdgeStatus status =
        meiso_composition_remote_edge_private_status(host_status);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!meiso_composition_remote_edge_private_receipt(
            host_receipt, &candidate
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    memmove(out_receipt, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus meiso_composition_remote_edge_private_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoEdgeStatus status =
        meiso_composition_remote_edge_private_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    return meiso_composition_remote_edge_private_finish_command(
        meiso_composition_host_runtime_put_mesh(
            meiso_composition_remote_edge_private_runtime(handle),
            canonical_mesh,
            &receipt
        ),
        &receipt,
        out_receipt
    );
}

static MeisoEdgeStatus meiso_composition_remote_edge_private_set_scene(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoEdgeStatus status =
        meiso_composition_remote_edge_private_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    return meiso_composition_remote_edge_private_finish_command(
        meiso_composition_host_runtime_set_scene(
            meiso_composition_remote_edge_private_runtime(handle),
            enabled,
            value,
            &receipt
        ),
        &receipt,
        out_receipt
    );
}

static MeisoEdgeStatus
meiso_composition_remote_edge_private_set_presentation(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionEdgePresentationSettings *settings,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoCompositionHostRuntimePresentationSettings mapped;
    const MeisoCompositionHostRuntimePresentationSettings *mapped_pointer = NULL;
    MeisoEdgeStatus status;

    if (settings != NULL) {
        mapped = (MeisoCompositionHostRuntimePresentationSettings){
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
    status = meiso_composition_remote_edge_private_preflight(handle);
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    return meiso_composition_remote_edge_private_finish_command(
        meiso_composition_host_runtime_set_presentation(
            meiso_composition_remote_edge_private_runtime(handle),
            enabled,
            mapped_pointer,
            &receipt
        ),
        &receipt,
        out_receipt
    );
}

#define MEISO_DEFINE_REMOTE_TYPED_COMMAND(                                   \
    function_name, host_function, value_type                                 \
)                                                                            \
static MeisoEdgeStatus function_name(                                        \
    MeisoEdgeHandle *handle,                                                  \
    bool enabled,                                                            \
    const value_type *value,                                                  \
    MeisoEdgeCommandReceipt *out_receipt                                      \
)                                                                            \
{                                                                            \
    MeisoCompositionHostRuntimeCommandReceipt receipt;                        \
    MeisoEdgeStatus status =                                                  \
        meiso_composition_remote_edge_private_preflight(handle);              \
    if (status != MEISO_EDGE_OK) {                                            \
        return status;                                                        \
    }                                                                        \
    return meiso_composition_remote_edge_private_finish_command(              \
        host_function(                                                        \
            meiso_composition_remote_edge_private_runtime(handle),            \
            enabled,                                                          \
            value,                                                            \
            &receipt                                                          \
        ),                                                                    \
        &receipt,                                                             \
        out_receipt                                                           \
    );                                                                        \
}

MEISO_DEFINE_REMOTE_TYPED_COMMAND(
    meiso_composition_remote_edge_private_set_wifi,
    meiso_composition_host_runtime_set_wifi,
    MeisoWifiTelemetryDesiredParameters
)
MEISO_DEFINE_REMOTE_TYPED_COMMAND(
    meiso_composition_remote_edge_private_set_device,
    meiso_composition_host_runtime_set_device,
    MeisoDeviceTelemetryParameters
)
MEISO_DEFINE_REMOTE_TYPED_COMMAND(
    meiso_composition_remote_edge_private_set_render_media,
    meiso_composition_host_runtime_set_render_media,
    MeisoCompositionMediaDesired
)
MEISO_DEFINE_REMOTE_TYPED_COMMAND(
    meiso_composition_remote_edge_private_set_camera_media,
    meiso_composition_host_runtime_set_camera_media,
    MeisoCompositionMediaDesired
)

#undef MEISO_DEFINE_REMOTE_TYPED_COMMAND

const MeisoCompositionEdgeBackendOps
    meiso_composition_remote_edge_private_backend_ops = {
        meiso_composition_remote_edge_private_snapshot,
        meiso_composition_remote_edge_private_put_mesh,
        meiso_composition_remote_edge_private_set_scene,
        meiso_composition_remote_edge_private_set_presentation,
        meiso_composition_remote_edge_private_set_wifi,
        meiso_composition_remote_edge_private_set_device,
        meiso_composition_remote_edge_private_set_render_media,
        meiso_composition_remote_edge_private_set_camera_media,
        meiso_composition_remote_edge_private_media_latest,
        meiso_composition_remote_edge_private_media_release
    };
