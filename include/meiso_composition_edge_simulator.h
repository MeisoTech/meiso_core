#ifndef MEISO_COMPOSITION_EDGE_SIMULATOR_H
#define MEISO_COMPOSITION_EDGE_SIMULATOR_H

#include "meiso_composition_edge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t catalog_generation;
    MeisoCompositionSessionV3CatalogConfig catalog;
    uint64_t continuity_serial;
    MeisoOwnerScope desired_owner;
    uint64_t wifi_production_epoch;
    uint64_t render_media_stream_epoch;
    uint64_t camera_media_stream_epoch;
    MeisoWifiTelemetrySample wifi_sample;
    /* Optional deterministic device desired/Actual/Stream support. */
    bool device_telemetry_enabled;
    uint64_t device_production_epoch;
    MeisoDeviceTelemetrySample device_sample;
    /*
     * Optional deterministic Composition subset. Disabled requires every
     * following field to be canonical zero. Enabled reserves an immutable
     * BlobStore with 1..MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS slots,
     * SceneStore and C2 coordinator inside the caller arena. The two initial
     * production epochs advance independently for every newly applied
     * enabled Presentation/Scene desired. RenderResult Stream epochs follow
     * the applied Scene production epoch.
     */
    bool composition_enabled;
    uint32_t composition_blob_slot_count;
    uint64_t composition_clock_epoch;
    uint64_t presentation_production_epoch;
    uint64_t scene_production_epoch;
    /*
     * Optional source-pinned CPU raster preview. This requires Composition,
     * reserves the fixed two-target renderer in the caller arena, replaces
     * the fake RenderResult with the CPU raster result, and publishes the
     * complete RGBA8888 target on an enabled render-media route. It remains
     * synthetic offscreen evidence: no driver, scanout, physical display,
     * cross-target pixel promise, wall clock, thread, socket or heap.
     * The configured render capability must be exact RGBA8888/raw at the
     * fixed Scene target dimensions; broader formats or dimensions are an
     * invalid config rather than capabilities the renderer cannot honor.
     */
    bool cpu_raster_preview_enabled;
} MeisoCompositionEdgeSimulatorConfig;

typedef uint8_t MeisoCompositionEdgeSimulatorInitStatus;
enum {
    MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK = 0,
    MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_ARENA_TOO_SMALL = 4,
    MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INTERNAL_FAILURE = 5
};

typedef uint8_t MeisoCompositionEdgeSimulatorTransition;
enum {
    MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE = 0,
    MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED = 1,
    MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED = 2,
    MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED = 3,
    MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED = 4,
    MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED = 5,
    MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED = 6,
    MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED = 7,
    MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_ACTIVATED = 8,
    MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED = 9,
    MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_PREVIEW_PUBLISHED = 10
};

typedef struct {
    MeisoCompositionMediaSource source;
    uint64_t media_stream_epoch;
    uint64_t frame_sequence;
    MeisoTimePoint source_time;
    MeisoOwnerScope applied_scene_owner;
    uint64_t applied_scene_state_version;
    uint64_t active_plan_generation;
    uint64_t world_generation;
    uint64_t dependency_sequence;
    uint64_t producer_dropped_frames;
    MeisoCompositionMediaPixelFormat pixel_format;
    MeisoCompositionMediaCodec codec;
    uint16_t width;
    uint16_t height;
    MeisoCompositionMediaFrameFlags flags;
    MeisoByteView access_unit;
} MeisoCompositionEdgeSimulatorFrame;

size_t meiso_composition_edge_simulator_alignment_required(void);
size_t meiso_composition_edge_simulator_memory_required(
    const MeisoCompositionEdgeSimulatorConfig *config
);

/*
 * Simulator storage, config, out_handle and every borrowed input/output are
 * mutually disjoint. The simulator owns exactly memory_required() arena bytes,
 * allocates no heap, reads no wall clock and creates no thread or socket.
 * Every init failure leaves arena and out_handle byte-exact.
 */
MeisoCompositionEdgeSimulatorInitStatus meiso_composition_edge_simulator_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoEdgeHandle **out_handle
);

/*
 * BOOTSTRAPPING publishes no graph. activate initializes any configured
 * Composition stores and then publishes the exact v3 graph. A defensive
 * child-initializer failure makes the handle sticky INTERNAL_FAILURE terminal;
 * it never returns a partially READY graph.
 */
MeisoEdgeStatus meiso_composition_edge_simulator_activate(
    MeisoEdgeHandle *handle
);

/*
 * At most one queued desired, Mesh publication, Presentation activation,
 * RenderResult, or optional CPU-raster preview frame is advanced. now becomes
 * the source time for an applied telemetry sample and the logical Composition
 * boundary/render time.
 * When Composition is enabled, now.clock_epoch must exactly equal the
 * configured composition_clock_epoch; a mismatch is INVALID_ARGUMENT with
 * no simulator or output change.
 */
MeisoEdgeStatus meiso_composition_edge_simulator_step(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now,
    MeisoCompositionEdgeSimulatorTransition *out_transition
);

/*
 * Copies one complete selected-route access unit into the caller arena. When
 * cpu_raster_preview_enabled is true, the renderer exclusively owns the render
 * preview route and caller publication to that route returns CONFLICT.
 */
MeisoEdgeStatus meiso_composition_edge_simulator_publish_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
);

/* Sticky terminal; clears graph, typed truth, queued work and media borrows. */
MeisoEdgeStatus meiso_composition_edge_simulator_terminate(
    MeisoEdgeHandle *handle,
    MeisoEdgeTerminalReason reason
);

#ifdef __cplusplus
}
#endif

#endif
