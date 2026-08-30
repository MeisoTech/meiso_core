#ifndef MEISO_COMPOSITION_REMOTE_EDGE_PRIVATE_H
#define MEISO_COMPOSITION_REMOTE_EDGE_PRIVATE_H

/* Build-private/non-installed; RemoteEdge borrows, and never owns, HostRuntime. */

#include <meiso/host/remote_edge.h>

#include "../composition/meiso_composition_edge_private.h"

MeisoCompositionHostRuntime *
meiso_composition_remote_edge_private_runtime(MeisoEdgeHandle *handle);
const MeisoCompositionHostRuntime *
meiso_composition_remote_edge_private_const_runtime(
    const MeisoEdgeHandle *handle
);
bool meiso_composition_remote_edge_private_base_snapshot(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionHostRuntimeSnapshot *host,
    MeisoEdgeSnapshot *out_edge
);
bool meiso_composition_remote_edge_private_read_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionHostRuntimeSnapshot *out_host,
    MeisoEdgeSnapshot *out_edge
);
MeisoEdgeStatus meiso_composition_remote_edge_private_status(
    MeisoCompositionHostRuntimeStatus status
);
bool meiso_composition_remote_edge_private_receipt(
    const MeisoCompositionHostRuntimeCommandReceipt *host,
    MeisoEdgeCommandReceipt *out_receipt
);
void meiso_composition_remote_edge_private_refresh_media_borrows(
    MeisoEdgeHandle *handle,
    const MeisoCompositionHostRuntimeSnapshot *host,
    const MeisoEdgeSnapshot *edge
);

MeisoEdgeStatus meiso_composition_remote_edge_private_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
);
MeisoEdgeStatus meiso_composition_remote_edge_private_media_latest(
    MeisoEdgeHandle *handle,
    MeisoCompositionMediaSource source,
    MeisoEdgeMediaFrameView *out_frame
);
MeisoEdgeStatus meiso_composition_remote_edge_private_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
);

#endif
