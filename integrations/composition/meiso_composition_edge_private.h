#ifndef MEISO_COMPOSITION_EDGE_PRIVATE_H
#define MEISO_COMPOSITION_EDGE_PRIVATE_H

/*
 * Build-private and non-installed. The caller arena owns handle storage for
 * its full lifetime; backends borrow external runtime/config inputs only as
 * stated by their public initializer and never transfer those lifetimes.
 */

#include <meiso/composition/coordinator.h>
#include <meiso/composition/cpu_raster_renderer.h>
#include <meiso/composition/scene_store.h>
#include <meiso/host/composition.h>
#include <meiso/profiles/composition/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MeisoCompositionHostRuntime MeisoCompositionHostRuntime;

#define MEISO_COMPOSITION_EDGE_HANDLE_MAGIC UINT64_C(0x4d4549534f454447)
#define MEISO_COMPOSITION_EDGE_HOST_OPERATIONS                              \
    (MEISO_EDGE_OPERATION_STATE_READ | MEISO_EDGE_OPERATION_STATE_SUBMIT | \
     MEISO_EDGE_OPERATION_STREAM_READ | MEISO_EDGE_OPERATION_BLOB_PUT |    \
     MEISO_EDGE_OPERATION_COMPOSITION | MEISO_EDGE_OPERATION_MEDIA_READ)
#define MEISO_COMPOSITION_EDGE_SIMULATOR_OPERATIONS                         \
    (MEISO_EDGE_OPERATION_STATE_READ | MEISO_EDGE_OPERATION_STATE_SUBMIT | \
     MEISO_EDGE_OPERATION_STREAM_READ | MEISO_EDGE_OPERATION_MEDIA_READ)

enum {
    MEISO_COMPOSITION_EDGE_BACKEND_HOST = 1,
    MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR = 2
};

typedef struct MeisoCompositionEdgeBackendOps
    MeisoCompositionEdgeBackendOps;

typedef struct {
    bool active;
    MeisoCompositionMediaFrameRef ref;
} MeisoCompositionEdgeMediaBorrow;

typedef struct {
    bool pending;
    bool enabled;
    MeisoCompositionMediaDesired desired;
} MeisoCompositionEdgeSimulatorMediaPending;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    bool present;
    bool borrowed;
    MeisoEdgeMediaFrameView frame;
} MeisoCompositionEdgeSimulatorMediaRoute;

typedef struct {
    bool pending;
    bool enabled;
    MeisoCompositionSceneDesired value;
} MeisoCompositionEdgeSimulatorScenePending;

typedef struct {
    bool pending;
    bool enabled;
    MeisoCompositionEdgePresentationSettings settings;
    MeisoCompositionScenePresentationPlan plan;
} MeisoCompositionEdgeSimulatorPresentationPending;

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
} MeisoCompositionEdgeSimulatorLayout;

/*
 * This structure is opaque publicly. Its field order and pointer slots retain
 * the pre-split arena ABI: backend_ops occupies the former host-runtime slot,
 * while backend_runtime occupies the former simulator BlobStore slot. The
 * latter union gives each backend one owner without increasing or shifting the
 * caller-owned arena layout.
 */
struct MeisoEdgeHandle {
    uint64_t magic;
    uint8_t backend_kind;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t host_arena_begin;
    uintptr_t host_arena_end;
    const MeisoCompositionEdgeBackendOps *backend_ops;
    uint64_t catalog_generation;
    uint64_t continuity_serial;
    MeisoProfileRef profile;
    uint8_t dynamic_capability[
        MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES
    ];
    MeisoRuntimeEndpointDescriptor
        endpoints[MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT];
    MeisoCompositionProfileRouteDescriptor
        routes[MEISO_COMPOSITION_PROFILE_ROUTE_COUNT];
    MeisoCompositionEdgeMediaBorrow
        media_borrows[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    MeisoCompositionProfileCatalogConfig simulator_catalog;
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
    MeisoCompositionEdgeSimulatorMediaPending
        simulator_media_pending[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
    MeisoCompositionEdgeSimulatorMediaRoute
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
    union {
        MeisoBlobStore *simulator_blob_store;
        MeisoCompositionHostRuntime *remote_host_runtime;
    } backend_runtime;
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
    MeisoCompositionEdgeSimulatorScenePending simulator_scene_pending;
    bool simulator_scene_retry_pending;
    bool simulator_scene_retry_ready;
    bool simulator_scene_value_present;
    bool simulator_scene_enabled;
    MeisoCompositionSceneDesired simulator_scene_value;
    MeisoCompositionEdgeSimulatorPresentationPending
        simulator_presentation_pending;
    bool simulator_presentation_plan_present;
    bool simulator_presentation_activation_pending;
    MeisoCompositionScenePresentationPlan simulator_presentation_plan;
    bool simulator_render_result_pending;
};

struct MeisoCompositionEdgeBackendOps {
    MeisoEdgeStatus (*snapshot)(
        const MeisoEdgeHandle *handle,
        MeisoCompositionEdgeSnapshot *out_snapshot
    );
    MeisoEdgeStatus (*put_mesh)(
        MeisoEdgeHandle *handle,
        MeisoByteView canonical_mesh,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*set_scene)(
        MeisoEdgeHandle *handle,
        bool enabled,
        const MeisoCompositionSceneDesired *value,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*set_presentation)(
        MeisoEdgeHandle *handle,
        bool enabled,
        const MeisoCompositionEdgePresentationSettings *settings,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*set_wifi)(
        MeisoEdgeHandle *handle,
        bool enabled,
        const MeisoWifiTelemetryDesiredParameters *value,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*set_device)(
        MeisoEdgeHandle *handle,
        bool enabled,
        const MeisoDeviceTelemetryParameters *value,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*set_render_media)(
        MeisoEdgeHandle *handle,
        bool enabled,
        const MeisoCompositionMediaDesired *value,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*set_camera_media)(
        MeisoEdgeHandle *handle,
        bool enabled,
        const MeisoCompositionMediaDesired *value,
        MeisoEdgeCommandReceipt *out_receipt
    );
    MeisoEdgeStatus (*media_latest)(
        MeisoEdgeHandle *handle,
        MeisoCompositionMediaSource source,
        MeisoEdgeMediaFrameView *out_frame
    );
    MeisoEdgeStatus (*media_release)(
        MeisoEdgeHandle *handle,
        const MeisoEdgeMediaFrameKey *expected_frame
    );
};

bool meiso_composition_edge_private_checked_range(
    const void *pointer,
    size_t size,
    uintptr_t *out_begin,
    uintptr_t *out_end
);
bool meiso_composition_edge_private_checked_add_size(
    size_t lhs,
    size_t rhs,
    size_t *out
);
bool meiso_composition_edge_private_checked_align_size(
    size_t value,
    size_t alignment,
    size_t *out
);
bool meiso_composition_edge_private_ranges_overlap(
    uintptr_t lhs_begin,
    uintptr_t lhs_end,
    uintptr_t rhs_begin,
    uintptr_t rhs_end
);
bool meiso_composition_edge_private_range_inside(
    uintptr_t outer_begin,
    uintptr_t outer_end,
    uintptr_t inner_begin,
    uintptr_t inner_end
);
bool meiso_composition_edge_private_handle_valid(
    const MeisoEdgeHandle *handle
);
bool meiso_composition_edge_private_storage_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
);
bool meiso_composition_edge_private_optional_input_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
);
bool meiso_composition_edge_private_disjoint_pair(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
int meiso_composition_edge_private_media_index(
    MeisoCompositionMediaSource source
);

extern const MeisoCompositionEdgeBackendOps
    meiso_composition_remote_edge_private_backend_ops;
extern const MeisoCompositionEdgeBackendOps
    meiso_composition_edge_simulator_private_backend_ops;

#endif
