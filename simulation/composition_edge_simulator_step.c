#include "meiso_composition_edge_simulator_private.h"

/* Simulator step is the single deterministic transition scheduler. */

#include <string.h>

MeisoEdgeStatus meiso_composition_edge_simulator_step(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now,
    MeisoCompositionEdgeSimulatorTransition *out_transition
)
{
    MeisoCompositionEdgeSimulatorTransition transition =
        MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR ||
        out_transition == NULL || !meiso_time_point_valid(now) ||
        (handle->simulator_composition_supported &&
         now.clock_epoch != handle->simulator_composition_clock_epoch) ||
        !meiso_composition_edge_private_storage_disjoint(handle, out_transition,
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
        if (!meiso_composition_edge_simulator_private_publish_mesh(handle)) {
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
        meiso_composition_edge_simulator_private_apply_wifi(handle, now);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED;
    } else if (handle->simulator_device_pending) {
        if (handle->simulator_device_enabled &&
            handle->simulator_snapshot.inline_streams[
                MEISO_COMPOSITION_EDGE_INLINE_DEVICE
            ].received_count == UINT64_MAX) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        meiso_composition_edge_simulator_private_apply_device(handle, now);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED;
    } else if (handle->simulator_scene_pending.pending) {
        if (!meiso_composition_edge_simulator_private_apply_scene_command(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED;
    } else if (handle->simulator_presentation_pending.pending) {
        if (!meiso_composition_edge_simulator_private_apply_presentation_command(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition =
            MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED;
    } else if (handle->simulator_scene_retry_ready) {
        if (!meiso_composition_edge_simulator_private_apply_scene_command(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED;
    } else if (handle->simulator_presentation_activation_pending) {
        if (!meiso_composition_edge_simulator_private_activate_presentation(handle, now)) {
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
        if (!meiso_composition_edge_simulator_private_publish_render_result(handle, now)) {
            (void)meiso_composition_edge_simulator_terminate(
                handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
            );
            return MEISO_EDGE_INTERNAL_FAILURE;
        }
        transition =
            MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED;
    } else if (handle->simulator_media_pending[0].pending) {
        meiso_composition_edge_simulator_private_apply_media(handle, 0U);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED;
    } else if (handle->simulator_media_pending[1].pending) {
        meiso_composition_edge_simulator_private_apply_media(handle, 1U);
        transition = MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED;
    } else if (handle->simulator_raster_preview_pending &&
               handle->simulator_snapshot.media[0].actual_binding_ready) {
        const MeisoEdgeStatus preview_status =
            meiso_composition_edge_simulator_private_publish_raster_preview(handle);
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
