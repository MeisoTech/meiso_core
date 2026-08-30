#ifndef MEISO_TARGETS_K230D_LINUX_COMPOSITION_EDGE_RUNTIME_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_COMPOSITION_EDGE_RUNTIME_PRIVATE_H

/*
 * Build-only implementation seam; never install or include outside this target.
 * The caller retains arena storage for the full runtime lifetime. Initialized
 * children and scratch borrows remain arena-backed, while control/media fds and
 * the camera syscall context retain the ownership documented by the public API.
 * Runtime state is single-threaded and non-reentrant; only detached thermal
 * request processing may run elsewhere, and it never dereferences this struct.
 */

#include <meiso/edge/linux/composition_runtime.h>

#include <fcntl.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE \
    __attribute__((visibility("hidden")))
#else
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
#endif

enum {
    K230D_COMPOSITION_EDGE_RUNTIME_MAGIC = 0x45415050U, /* EAPP */
    EDGE_MEDIA_ADAPTER_RESERVE_BYTES = 16384U,

    ROUTE_WIFI_DESIRED = 0,
    ROUTE_WIFI_ACTUAL = 1,
    ROUTE_WIFI_INLINE = 2,
    ROUTE_PRESENTATION_DESIRED = 3,
    ROUTE_PRESENTATION_ACTUAL = 4,
    ROUTE_SCENE_DESIRED = 5,
    ROUTE_SCENE_ACTUAL = 6,
    ROUTE_MESH_BLOB = 7,
    ROUTE_RENDER_INLINE = 8,
    ROUTE_DEVICE_DESIRED = 9,
    ROUTE_DEVICE_ACTUAL = 10,
    ROUTE_DEVICE_INLINE = 11,
    ROUTE_RENDER_MEDIA_DESIRED = 12,
    ROUTE_RENDER_MEDIA_ACTUAL = 13,
    ROUTE_RENDER_MEDIA_DATA = 14,
    ROUTE_CAMERA_MEDIA_DESIRED = 15,
    ROUTE_CAMERA_MEDIA_ACTUAL = 16,
    ROUTE_CAMERA_MEDIA_DATA = 17,

    SOURCE_CATALOG = 0,
    SOURCE_WIFI_DESIRED_ACCEPT = 1,
    SOURCE_WIFI_ACTUAL = 2,
    SOURCE_WIFI_INLINE = 3,
    SOURCE_BLOB_RESULT = 4,
    SOURCE_PRESENTATION_DESIRED_ACCEPT = 5,
    SOURCE_PRESENTATION_ACTUAL = 6,
    SOURCE_SCENE_DESIRED_ACCEPT = 7,
    SOURCE_SCENE_ACTUAL = 8,
    SOURCE_RENDER_INLINE = 9,
    SOURCE_DEVICE_DESIRED_ACCEPT = 10,
    SOURCE_DEVICE_ACTUAL = 11,
    SOURCE_DEVICE_INLINE = 12,
    SOURCE_RENDER_MEDIA_DESIRED_ACCEPT = 13,
    SOURCE_RENDER_MEDIA_ACTUAL = 14,
    SOURCE_CAMERA_MEDIA_DESIRED_ACCEPT = 15,
    SOURCE_CAMERA_MEDIA_ACTUAL = 16
};

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
extern const uint8_t meiso_k230d_composition_edge_runtime_private_desired_route_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT
];

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
extern const uint8_t meiso_k230d_composition_edge_runtime_private_actual_route_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT
];

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
extern const uint8_t meiso_k230d_composition_edge_runtime_private_inline_route_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT
];

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
extern const uint8_t meiso_k230d_composition_edge_runtime_private_desired_source_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT
];

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
extern const uint8_t meiso_k230d_composition_edge_runtime_private_actual_source_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT
];

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
extern const uint8_t meiso_k230d_composition_edge_runtime_private_inline_source_index[
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT
];

typedef struct {
    MeisoStateReplica *replica;
    bool owner_started;
    bool accept_pending;
    bool accept_queued;
    MeisoDeliveryGeneration accept_generation;
    MeisoOwnerScope accept_owner;
    uint64_t accept_version;
    MeisoRuntimeDesiredState decoded;
    MeisoRuntimeDesiredReference reference;
} EdgeDesiredChannel;

typedef struct {
    MeisoStatePublisher *publisher;
    MeisoK230dCompositionEdgeRuntimeActualSnapshot snapshot;
    bool offer_queued;
    bool offer_sent;
    uint64_t production_epoch_high_water;
} EdgeActualChannel;

typedef struct {
    MeisoStream *stream;
    MeisoK230dCompositionEdgeRuntimeInlineSnapshot snapshot;
    bool sample_queued;
    bool sample_sent;
    uint64_t pending_sequence;
} EdgeInlineChannel;

typedef struct {
    size_t offset;
    bool consumed;
    int fd;
    uint8_t active_route_mask;
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaPosixTcp *adapter;
    MeisoCompositionMediaTransportConfig transport_config;
} EdgeMediaSlot;

typedef struct {
    uint64_t production_epoch;
    uint64_t next_due_ns;
    uint64_t remainder;
    uint32_t rate_millihz;
    bool armed;
} EdgeCadence;

typedef struct {
    size_t total;
    size_t control_session_offset;
    size_t control_session_bytes;
    size_t replica_offset[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT];
    size_t replica_bytes[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT];
    size_t publisher_offset[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT];
    size_t publisher_bytes[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT];
    size_t stream_offset[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT];
    size_t stream_bytes[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT];
    size_t blob_offset;
    size_t blob_bytes;
    size_t scene_offset;
    size_t scene_bytes;
    size_t coordinator_offset;
    size_t coordinator_bytes;
    size_t renderer_offset;
    size_t renderer_bytes;
    size_t camera_offset;
    size_t camera_bytes;
    size_t media_slots_offset;
    size_t media_transport_bytes;
    size_t media_slot_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
} K230dCompositionEdgeRuntimeLayout;

typedef struct {
    const void *data;
    size_t size;
} EdgeRange;

struct MeisoK230dCompositionEdgeRuntime {
    uint32_t magic;
    uint8_t *arena_begin;
    size_t arena_bytes;
    MeisoK230dCompositionEdgeRuntimeConfig config;
    MeisoCompositionControlSession *control_session;
    MeisoCompositionProfileCatalogConfig catalog;
    MeisoCompositionProfileRouteDescriptor catalog_routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    EdgeDesiredChannel desired[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT];
    EdgeActualChannel actual[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT];
    EdgeInlineChannel inline_streams[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT
    ];
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionCoordinator *coordinator;
    uint8_t *coordinator_arena;
    MeisoCompositionCpuRasterRenderer *renderer;
    MeisoCameraV4l2Adapter *camera;
    MeisoWifiLinuxAdapter wifi;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoWifiTelemetryCapability wifi_capability;
    MeisoDeviceTelemetryCapability device_capability;
    EdgeMediaSlot media_slots[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MAX_MEDIA_CONNECTIONS];
    EdgeMediaSlot *active_media;
    uint8_t media_slots_consumed;
    uint8_t fairness_cursor;
    bool deliveries_started;
    bool coordinator_initialized;
    bool desired_owner_present;
    MeisoOwnerScope desired_owner;
    bool mutable_active;
    MeisoK230dCompositionEdgeRuntimePhase phase;
    MeisoK230dCompositionEdgeRuntimeTerminalReason terminal_reason;
    uint64_t next_thermal_request_id;
    MeisoK230dCompositionEdgeRuntimeThermalRequest thermal_request;
    bool thermal_request_pending;
    bool thermal_request_dispatched;
    MeisoK230dCompositionEdgeRuntimeThermalResult thermal_result;
    bool thermal_result_present;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_effective_present;
    MeisoWifiTelemetrySample wifi_sample;
    bool wifi_sample_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_effective_present;
    MeisoDeviceTelemetrySample device_sample;
    bool device_sample_present;
    MeisoCompositionMediaActual render_media_actual;
    bool render_media_actual_present;
    MeisoCompositionMediaActual camera_media_actual;
    bool camera_media_actual_present;
    MeisoCompositionScenePresentationStatus presentation_status;
    bool presentation_status_present;
    MeisoCompositionSceneStatus scene_status;
    bool scene_status_present;
    MeisoCompositionCpuRasterJobRef render_job;
    bool render_job_pending;
    MeisoCompositionCpuRasterCompleteView render_complete;
    bool render_complete_present;
    bool render_inline_materialized;
    bool render_inline_accepted;
    bool render_media_materialized;
    uint64_t render_media_sequence;
    MeisoCameraV4l2FrameView camera_frame;
    bool camera_frame_pending;
    bool camera_frame_submitted;
    uint64_t camera_media_sequence;
    uint64_t wifi_next_due_ns;
    uint64_t device_next_due_ns;
    EdgeCadence render_result_cadence;
    EdgeCadence render_media_cadence;
    EdgeCadence camera_media_cadence;
    MeisoCompositionSceneDesired scene_desired;
    bool scene_desired_present;
    MeisoCompositionScenePresentationPlan presentation_plan;
    bool presentation_plan_present;
    MeisoContentId visible_blob_ids[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MESH_BLOB_SLOTS
    ];
    uint64_t visible_blob_sizes[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MESH_BLOB_SLOTS
    ];
    uint8_t visible_blob_count;
    uint64_t blob_published_bytes;
    uint64_t blob_transfer_id_high_water;
    MeisoRuntimeBlobTransferRef blob_receive_transfer;
    MeisoRuntimeBlobTransferRef blob_result_transfer;
    uint64_t blob_receive_next_offset;
    MeisoRuntimeBlobOperation blob_result_operation;
    bool blob_receive_active;
    bool blob_receive_begin_ready;
    bool blob_result_pending;
    bool blob_result_queued;
    bool blob_result_marks_ready;
    bool blob_result_clears_active;
    uint8_t state_scratch[
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t actual_scratch[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    uint8_t profile_scratch[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    uint8_t stream_scratch[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    size_t control_session_bytes;
    size_t blob_store_bytes;
    size_t scene_store_bytes;
    size_t coordinator_bytes;
    size_t renderer_bytes;
    size_t camera_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t media_slot_bytes;
    size_t media_transport_bytes;
};

_Static_assert(
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT == 6U &&
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT == 6U &&
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT == 3U,
    "edge channel tables"
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
void meiso_k230d_composition_edge_runtime_private_cadence_reset(EdgeCadence *cadence);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_cadence_ready(
    EdgeCadence *cadence,
    uint64_t production_epoch,
    uint32_t rate_millihz,
    uint64_t now_ns
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_cadence_commit(EdgeCadence *cadence, uint64_t now_ns);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_fd_preflight(int fd);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_fd_prepare_send_safety(int fd);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
MeisoOwnerScope meiso_k230d_composition_edge_runtime_private_edge_owner(
    const MeisoK230dCompositionEdgeRuntimeConfig *config
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(const MeisoK230dCompositionEdgeRuntime *runtime);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    const void *value,
    size_t size
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_route_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_route_channel(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const uint8_t *route_indices,
    size_t count,
    size_t *out_index
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_ensure_coordinator(MeisoK230dCompositionEdgeRuntime *runtime);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_ensure_desired_owner(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoOwnerScope owner
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_publish_actual(MeisoK230dCompositionEdgeRuntime *runtime, size_t index);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
void meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeProductionReason reason
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_set_actual_applied(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoRuntimeDesiredReference desired,
    bool enabled,
    bool running,
    bool start_new_epoch
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool require_stopped
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_fill_media_actual(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    const MeisoCompositionMediaDesired *effective
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_media_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_start_inline_producer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t inline_index,
    uint64_t production_epoch
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_disable_inline_producer(EdgeInlineChannel *channel);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_prepare_stored_scene(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool *out_attempted,
    bool *out_active_plan,
    MeisoCompositionScenePrepareStatus *out_status
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_try_prepare_presentation(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_apply_presentation(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference,
    MeisoTimePoint now
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_refresh_composition_actuals(MeisoK230dCompositionEdgeRuntime *runtime);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_apply_scene(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference,
    MeisoTimePoint now
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
void meiso_k230d_composition_edge_runtime_private_enter_terminal(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeTerminalReason reason,
    int error_number
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_refresh_phase(MeisoK230dCompositionEdgeRuntime *runtime);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_remember_visible_blob(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_apply_visible_blobs(MeisoK230dCompositionEdgeRuntime *runtime);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_handle_desired_offer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_handle_actual_accept(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_handle_runtime_event(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_handle_source_completion(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_edge_runtime_source_work_outstanding(
    const MeisoK230dCompositionEdgeRuntime *runtime
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_queue_desired_accept(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_queue_actual_offer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_queue_inline_sample(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_enqueue_inline_payload(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoTimePoint source_time,
    MeisoByteView payload
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_actual_delivery_current(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
int meiso_k230d_composition_edge_runtime_private_pump_wifi_sample(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
int meiso_k230d_composition_edge_runtime_private_pump_device_sample(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
bool meiso_k230d_composition_edge_runtime_private_media_route_active(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    uint8_t route_mask
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
int meiso_k230d_composition_edge_runtime_private_pump_renderer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
int meiso_k230d_composition_edge_runtime_private_pump_camera(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
void meiso_k230d_composition_edge_runtime_private_terminate_active_media(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
);

MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PRIVATE
MeisoK230dCompositionEdgeRuntimePumpStatus meiso_k230d_composition_edge_runtime_private_pump_media(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

#endif
