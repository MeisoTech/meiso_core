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
    MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED = 4
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

/* BOOTSTRAPPING publishes no graph; activate publishes the exact v3 graph. */
MeisoEdgeStatus meiso_composition_edge_simulator_activate(
    MeisoEdgeHandle *handle
);

/*
 * At most one queued desired is applied. now becomes the source time for an
 * applied Wi-Fi or DeviceTelemetry sample.
 */
MeisoEdgeStatus meiso_composition_edge_simulator_step(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now,
    MeisoCompositionEdgeSimulatorTransition *out_transition
);

/* Copies one complete selected-route access unit into the caller arena. */
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
