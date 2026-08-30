#ifndef MEISO_COMPOSITION_EDGE_SIMULATOR_PRIVATE_H
#define MEISO_COMPOSITION_EDGE_SIMULATOR_PRIVATE_H

/* Build-private/non-installed; every simulator child lives inside its arena. */

#include <meiso/simulation/composition_edge.h>

#include "../integrations/composition/meiso_composition_edge_private.h"

typedef int MeisoCompositionEdgeSimulatorPrepareResult;
enum {
    MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL = -1,
    MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_WAITING = 0,
    MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_PREPARED = 1,
    MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_REJECTED = 2
};

void meiso_composition_edge_simulator_private_apply_actual_state(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    bool enabled,
    bool running,
    uint64_t production_epoch
);
void meiso_composition_edge_simulator_private_apply_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    uint64_t production_epoch
);
void meiso_composition_edge_simulator_private_reject_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    MeisoRuntimeProductionReason reason
);

void meiso_composition_edge_simulator_private_apply_wifi(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);
void meiso_composition_edge_simulator_private_apply_device(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);
void meiso_composition_edge_simulator_private_apply_media(
    MeisoEdgeHandle *handle,
    size_t index
);
bool meiso_composition_edge_simulator_private_take_epoch(
    uint64_t *epoch,
    bool *available,
    uint64_t *out
);
bool meiso_composition_edge_simulator_private_candidate_nonce(
    const MeisoCompositionCoordinatorPlanRef *ref,
    uint8_t out_nonce[MEISO_SHA256_DIGEST_SIZE]
);
bool meiso_composition_edge_simulator_private_refresh_presentation_status(
    MeisoEdgeHandle *handle
);
bool meiso_composition_edge_simulator_private_refresh_scene_status(
    MeisoEdgeHandle *handle
);
bool meiso_composition_edge_simulator_private_refresh_composition_status(
    MeisoEdgeHandle *handle
);

bool meiso_composition_edge_simulator_private_clear_render_truth(
    MeisoEdgeHandle *handle
);
MeisoCompositionEdgeSimulatorPrepareResult
meiso_composition_edge_simulator_private_prepare_candidate(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);
bool meiso_composition_edge_simulator_private_publish_mesh(
    MeisoEdgeHandle *handle
);
bool meiso_composition_edge_simulator_private_runtime_plan(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE],
    MeisoRuntimeDesiredState *out_desired
);
bool meiso_composition_edge_simulator_private_apply_scene_command(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);
bool meiso_composition_edge_simulator_private_apply_presentation_command(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);
bool meiso_composition_edge_simulator_private_activate_presentation(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);

bool meiso_composition_edge_simulator_private_publish_render_result(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_commit_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_publish_raster_preview(
    MeisoEdgeHandle *handle
);

MeisoEdgeStatus meiso_composition_edge_simulator_private_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_scene(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_presentation(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionEdgePresentationSettings *settings,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_wifi(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *value,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_device(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoDeviceTelemetryParameters *value,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_render_media(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionMediaDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_camera_media(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionMediaDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_media_latest(
    MeisoEdgeHandle *handle,
    MeisoCompositionMediaSource source,
    MeisoEdgeMediaFrameView *out_frame
);
MeisoEdgeStatus meiso_composition_edge_simulator_private_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
);

#endif
