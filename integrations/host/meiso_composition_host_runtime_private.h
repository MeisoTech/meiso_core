#ifndef MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_H
#define MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_H

/*
 * Build-only Host Runtime state shared by its implementation units. Callers
 * must use <meiso/host/composition_runtime.h>; this header owns arena layout
 * and cross-unit borrows but exposes no installed SDK contract.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <meiso/host/composition_runtime.h>
#include <meiso/profiles/composition/scene.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MAGIC = 0x48415050U, /* HAPP */
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MEDIA_ADAPTER_RESERVE_BYTES = 16384U,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_DESIRED_SCRATCH_BYTES =
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE,

    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_DESIRED = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_ACTUAL = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_WIFI_INLINE = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_PRESENTATION_DESIRED = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_PRESENTATION_ACTUAL = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_DESIRED = 5,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_ACTUAL = 6,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB = 7,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_INLINE = 8,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_DESIRED = 9,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_ACTUAL = 10,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_DEVICE_INLINE = 11,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_DESIRED = 12,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_ACTUAL = 13,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_DATA = 14,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_DESIRED = 15,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_ACTUAL = 16,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_DATA = 17,

    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CATALOG = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_WIFI_DESIRED = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_WIFI_ACTUAL_ACCEPT = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_BEGIN = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_CHUNK = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_COMMIT = 5,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_ABORT = 6,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_PRESENTATION_DESIRED = 7,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_PRESENTATION_ACTUAL_ACCEPT = 8,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_SCENE_DESIRED = 9,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_SCENE_ACTUAL_ACCEPT = 10,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_DEVICE_DESIRED = 11,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_DEVICE_ACTUAL_ACCEPT = 12,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_RENDER_MEDIA_DESIRED = 13,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_RENDER_MEDIA_ACTUAL_ACCEPT = 14,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CAMERA_MEDIA_DESIRED = 15,
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_CAMERA_MEDIA_ACTUAL_ACCEPT = 16
};

extern const uint8_t meiso_composition_host_runtime_private_desired_route_index[
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT
];
extern const uint8_t meiso_composition_host_runtime_private_desired_source_index[
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT
];
extern const uint8_t meiso_composition_host_runtime_private_actual_route_index[
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT
];
extern const uint8_t meiso_composition_host_runtime_private_actual_source_index[
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT
];
extern const uint8_t meiso_composition_host_runtime_private_inline_route_index[
    MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT
];

typedef struct {
    MeisoStatePublisher *publisher;
    MeisoCompositionHostRuntimeDesiredSnapshot snapshot;
    uint8_t inflight_payload[
        MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_DESIRED_SCRATCH_BYTES
    ];
    size_t inflight_payload_size;
    uint64_t inflight_command_id;
    MeisoOwnerScope inflight_owner;
    uint64_t inflight_version;
    MeisoDeliveryGeneration inflight_generation;
    bool inflight_present;
    bool inflight_source_completed;
    bool inflight_offer_accepted;
    bool inflight_actual_observed;
    bool offer_queued;
    bool offer_sent;
    bool plan_present;
    bool plan_echoed;
    MeisoCompositionScenePresentationPlan plan;
} MeisoCompositionHostRuntimePrivateDesiredChannel;

typedef struct {
    MeisoStateReplica *replica;
    MeisoCompositionHostRuntimeActualSnapshot snapshot;
    bool owner_started;
    bool accept_pending;
    bool accept_work_queued;
    MeisoDeliveryGeneration accept_generation;
    MeisoOwnerScope accept_owner;
    uint64_t accept_version;
} MeisoCompositionHostRuntimePrivateActualChannel;

typedef struct {
    MeisoStream *stream;
    MeisoCompositionHostRuntimeInlineSnapshot snapshot;
    bool producer_present;
    MeisoOwnerScope producer;
    uint64_t epoch;
} MeisoCompositionHostRuntimePrivateInlineChannel;

typedef struct {
    size_t offset;
    bool consumed;
    int fd;
    uint8_t active_route_mask;
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaPosixTcp *adapter;
    MeisoCompositionMediaTransportConfig transport_config;
    uint64_t published_high_water[
        MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT
    ];
} MeisoCompositionHostRuntimePrivateMediaSlot;

typedef struct {
    size_t total;
    size_t control_session_offset;
    size_t control_session_bytes;
    size_t publisher_offset[MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT];
    size_t publisher_bytes[MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT];
    size_t replica_offset[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT];
    size_t replica_bytes[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT];
    size_t stream_offset[MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT];
    size_t stream_bytes[MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT];
    size_t mesh_offset;
    size_t mesh_bytes;
    size_t media_slots_offset;
    size_t media_transport_bytes;
    size_t media_slot_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
} MeisoCompositionHostRuntimePrivateArenaLayout;

struct MeisoCompositionHostRuntime {
    uint32_t magic;
    uint8_t *arena_begin;
    size_t arena_bytes;
    MeisoCompositionHostRuntimeConfig config;
    MeisoCompositionControlSession *control_session;
    MeisoCompositionProfileRouteDescriptor catalog_routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    MeisoCompositionHostRuntimePrivateDesiredChannel desired[
        MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT
    ];
    MeisoCompositionHostRuntimePrivateActualChannel actual[
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT
    ];
    MeisoCompositionHostRuntimePrivateInlineChannel inline_streams[
        MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT
    ];
    MeisoCompositionHostRuntimePrivateMediaSlot media_slots[
        MEISO_COMPOSITION_HOST_RUNTIME_MAX_MEDIA_CONNECTIONS
    ];
    MeisoCompositionHostRuntimePrivateMediaSlot *active_media;
    uint8_t media_slots_consumed;
    uint8_t fairness_cursor;
    bool deliveries_started;
    bool mutable_active;
    MeisoCompositionHostRuntimePhase phase;
    MeisoCompositionHostRuntimeTerminalReason terminal_reason;
    uint64_t next_command_id;
    uint64_t plan_generation_high_water;
    uint8_t *mesh_bytes;
    size_t mesh_capacity;
    size_t mesh_size;
    MeisoCompositionHostRuntimeMeshSnapshot mesh;
    bool blob_work_queued[4];
    uint32_t blob_chunk_queued_bytes;
    MeisoRuntimeBlobTransferRef blob_transfer;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_effective_present;
    MeisoWifiTelemetrySample wifi_sample;
    bool wifi_sample_present;
    MeisoCompositionScenePresentationStatus presentation_status;
    bool presentation_status_present;
    MeisoCompositionSceneStatus scene_status;
    bool scene_status_present;
    MeisoCompositionSceneRenderResult render_result;
    bool render_result_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_effective_present;
    MeisoDeviceTelemetrySample device_sample;
    bool device_sample_present;
    MeisoCompositionMediaActual render_media_actual;
    bool render_media_actual_present;
    MeisoCompositionMediaActual camera_media_actual;
    bool camera_media_actual_present;
    uint8_t desired_scratch[
        MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_DESIRED_SCRATCH_BYTES
    ];
    uint8_t profile_scratch[
        MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_DESIRED_SCRATCH_BYTES
    ];
    size_t control_session_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t mesh_staging_bytes;
    size_t media_slot_bytes;
    size_t media_transport_bytes;
};

_Static_assert(
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT == 6U &&
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT == 6U &&
        MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT == 3U,
    "host channel tables"
);
_Static_assert(
    (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI ==
            (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI &&
        (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION ==
            (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION &&
        (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_SCENE ==
            (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_SCENE &&
        (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_DEVICE ==
            (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_DEVICE &&
        (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA ==
            (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_RENDER_MEDIA &&
        (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA ==
            (unsigned)MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_CAMERA_MEDIA,
    "desired and actual barrier channel ordering"
);
_Static_assert(
    MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MEDIA_ADAPTER_RESERVE_BYTES >=
        MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES +
        MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES,
    "media adapter reserve covers bounded metadata and one RX packet"
);

bool meiso_composition_host_runtime_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
bool meiso_composition_host_runtime_private_fd_preflight(int fd);
bool meiso_composition_host_runtime_private_fd_prepare_send_safety(int fd);
bool meiso_composition_host_runtime_private_valid(
    const MeisoCompositionHostRuntime *runtime
);
bool meiso_composition_host_runtime_private_range_overlap(
    const MeisoCompositionHostRuntime *runtime,
    const void *value,
    size_t size
);
MeisoCompositionHostRuntimeStatus
meiso_composition_host_runtime_private_mutation_status(
    MeisoCompositionHostRuntime *runtime
);
bool meiso_composition_host_runtime_private_receipt_valid(
    const MeisoCompositionHostRuntime *runtime,
    const void *input,
    size_t input_size,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);
bool meiso_composition_host_runtime_private_media_route_config(
    const MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionControlSessionSnapshot *control_session,
    size_t media_index,
    MeisoCompositionMediaRouteConfig *out_route
);
bool meiso_composition_host_runtime_private_active_media_scope_current(
    MeisoCompositionHostRuntime *runtime
);
void meiso_composition_host_runtime_private_terminate_active_media(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
);
void meiso_composition_host_runtime_private_clear_desired_inflight(
    MeisoCompositionHostRuntimePrivateDesiredChannel *channel
);
void meiso_composition_host_runtime_private_expose_desired_inflight(
    MeisoCompositionHostRuntimePrivateDesiredChannel *channel
);
void meiso_composition_host_runtime_private_enter_terminal(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeTerminalReason reason,
    MeisoCompositionMediaTerminalReason media_reason,
    int error_number
);
bool meiso_composition_host_runtime_private_route_key_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
);
bool meiso_composition_host_runtime_private_route_channel(
    MeisoCompositionHostRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const uint8_t *routes,
    size_t count,
    size_t *out_index
);
bool meiso_composition_host_runtime_private_owner_equal(
    MeisoOwnerScope lhs,
    MeisoOwnerScope rhs
);
bool meiso_composition_host_runtime_private_desired_inflight_matches(
    const MeisoCompositionHostRuntimePrivateDesiredChannel *channel,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
);
void meiso_composition_host_runtime_private_retire_completed_desired_inflight(
    MeisoCompositionHostRuntimePrivateDesiredChannel *channel
);
bool meiso_composition_host_runtime_private_handle_runtime_event(
    MeisoCompositionHostRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostRuntimePumpResult *out_result
);
void meiso_composition_host_runtime_private_refresh_phase(
    MeisoCompositionHostRuntime *runtime
);
MeisoCompositionHostRuntimePumpStatus
meiso_composition_host_runtime_private_pump_media(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionHostRuntimeIoReady *ready,
    MeisoCompositionHostRuntimePumpResult *out_result
);
MeisoCompositionHostRuntimePumpStatus
meiso_composition_host_runtime_private_pump_local(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimePumpResult *out_result
);
MeisoCompositionHostRuntimePumpStatus
meiso_composition_host_runtime_private_pump_control(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionHostRuntimeIoReady *ready,
    MeisoCompositionHostRuntimePumpResult *out_result
);

#endif
