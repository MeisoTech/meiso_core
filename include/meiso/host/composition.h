#ifndef MEISO_HOST_COMPOSITION_H
#define MEISO_HOST_COMPOSITION_H

#include <meiso/profiles/composition/profile.h>
#include <meiso/host/edge.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_EDGE_DESIRED_COUNT 6U
#define MEISO_COMPOSITION_EDGE_ACTUAL_COUNT 6U
#define MEISO_COMPOSITION_EDGE_INLINE_STREAM_COUNT 3U
#define MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT 2U

typedef uint8_t MeisoCompositionEdgeDesiredChannel;
enum {
    MEISO_COMPOSITION_EDGE_DESIRED_WIFI = 0,
    MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION = 1,
    MEISO_COMPOSITION_EDGE_DESIRED_SCENE = 2,
    MEISO_COMPOSITION_EDGE_DESIRED_DEVICE = 3,
    MEISO_COMPOSITION_EDGE_DESIRED_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoCompositionEdgeActualChannel;
enum {
    MEISO_COMPOSITION_EDGE_ACTUAL_WIFI = 0,
    MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION = 1,
    MEISO_COMPOSITION_EDGE_ACTUAL_SCENE = 2,
    MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE = 3,
    MEISO_COMPOSITION_EDGE_ACTUAL_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoCompositionEdgeInlineChannel;
enum {
    MEISO_COMPOSITION_EDGE_INLINE_WIFI = 0,
    MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT = 1,
    MEISO_COMPOSITION_EDGE_INLINE_DEVICE = 2
};

typedef uint8_t MeisoEdgeCommandKind;
enum {
    MEISO_EDGE_COMMAND_PUT_MESH = 1,
    MEISO_EDGE_COMMAND_SET_SCENE = 2,
    MEISO_EDGE_COMMAND_SET_PRESENTATION = 3,
    MEISO_EDGE_COMMAND_SET_WIFI = 4,
    MEISO_EDGE_COMMAND_SET_DEVICE = 5,
    MEISO_EDGE_COMMAND_SET_RENDER_MEDIA = 6,
    MEISO_EDGE_COMMAND_SET_CAMERA_MEDIA = 7
};

typedef uint8_t MeisoEdgeLocalQueueResult;
enum {
    MEISO_EDGE_LOCAL_QUEUED = 1,
    MEISO_EDGE_LOCAL_REPLACED_PENDING = 2
};

/* A receipt proves only bounded local copy/queue, never K230D Edge application. */
typedef struct {
    uint64_t command_id;
    MeisoEdgeCommandKind kind;
    MeisoEdgeLocalQueueResult local_result;
    bool replaced_command_present;
    uint64_t replaced_command_id;
    bool desired_state_present;
    MeisoCompositionEdgeDesiredChannel desired_channel;
    uint64_t desired_state_version;
    bool plan_generation_present;
    uint64_t plan_generation;
    bool mesh_content_id_present;
    MeisoContentId mesh_content_id;
} MeisoEdgeCommandReceipt;

typedef struct {
    MeisoCompositionScenePlanFlags flags;
    uint16_t target_width;
    uint16_t target_height;
    MeisoCompositionSceneColorFormat color_format;
    MeisoCompositionSceneDepthFormat depth_format;
    /* PresentationPlan request: 1..MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ. */
    uint16_t max_result_hz;
    uint32_t offline_lease_ms;
} MeisoCompositionEdgePresentationSettings;

typedef struct {
    bool requested;
    bool enabled;
    uint64_t command_id;
    MeisoOwnerScope owner;
    uint64_t state_version;
    bool delivery_active;
    bool delivery_pending;
    bool delivery_accepted;
    bool delivery_failed;
    uint64_t replaced_pending_count;
    bool inflight_present;
    uint64_t inflight_command_id;
    MeisoOwnerScope inflight_owner;
    uint64_t inflight_state_version;
    bool inflight_source_completed;
    bool inflight_offer_accepted;
    bool inflight_actual_observed;
} MeisoCompositionEdgeDesiredView;

typedef struct {
    bool present;
    MeisoOwnerScope state_owner;
    uint64_t state_version;
    MeisoRuntimeDesiredReference observed_desired;
    MeisoRuntimeDesiredReference applied_desired;
    MeisoRuntimeApplyResult apply_result;
    MeisoRuntimeAvailability availability;
    bool effective_enabled;
    MeisoRuntimeProductionStatus production_status;
    MeisoRuntimeProductionReason reason;
    uint64_t production_epoch;
    bool profile_decoded;
} MeisoCompositionEdgeActualView;

typedef struct {
    bool present;
    MeisoStreamKey key;
    MeisoTimePoint source_time;
    MeisoStreamReceiveFlags receive_flags;
    uint64_t missing_before;
    uint64_t received_count;
    uint64_t replaced_count;
} MeisoCompositionEdgeInlineView;

typedef struct {
    bool queued;
    bool transfer_active;
    bool begin_ready;
    bool waiting_result;
    bool terminal_result_present;
    uint64_t command_id;
    MeisoContentId content_id;
    uint64_t total_bytes;
    uint64_t next_offset;
    MeisoRuntimeBlobResult terminal_result;
    bool edge_core_published;
} MeisoCompositionEdgeMeshView;

typedef struct {
    bool actual_binding_ready;
    bool latest_present;
    MeisoCompositionMediaSource source;
    uint64_t latest_frame_sequence;
    MeisoTimePoint latest_source_time;
    uint32_t latest_access_unit_bytes;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
} MeisoCompositionEdgeMediaRouteView;

/* Prepared tokens and endpoint bindings remain inside the SDK integration. */
typedef struct {
    uint64_t capability_generation;
    MeisoCompositionSceneCandidateState candidate_state;
    uint64_t candidate_plan_generation;
    uint8_t candidate_digest[MEISO_SHA256_DIGEST_SIZE];
    MeisoCompositionSceneActiveState active_state;
    uint64_t active_plan_generation;
    uint8_t active_digest[MEISO_SHA256_DIGEST_SIZE];
    MeisoCompositionScenePresentationReason reason;
    uint16_t target_width;
    uint16_t target_height;
    MeisoCompositionSceneColorFormat color_format;
    MeisoCompositionSceneDepthFormat depth_format;
} MeisoCompositionEdgePresentationStatus;

/* Channel nonce and transport binding remain inside the SDK integration. */
typedef struct {
    MeisoCompositionMediaSource source;
    MeisoCompositionMediaDesired effective;
    uint64_t media_stream_epoch;
    uint64_t producer_dropped_frames;
    uint32_t queue_depth;
    uint32_t recovery_generation;
} MeisoCompositionEdgeMediaActual;

typedef struct {
    MeisoEdgeSnapshot edge;
    MeisoCompositionEdgeDesiredView
        desired[MEISO_COMPOSITION_EDGE_DESIRED_COUNT];
    MeisoCompositionEdgeActualView
        actual[MEISO_COMPOSITION_EDGE_ACTUAL_COUNT];
    MeisoCompositionEdgeInlineView
        inline_streams[MEISO_COMPOSITION_EDGE_INLINE_STREAM_COUNT];
    MeisoCompositionEdgeMeshView mesh;
    MeisoCompositionEdgeMediaRouteView
        media[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];

    bool wifi_effective_present;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_sample_present;
    MeisoWifiTelemetrySample wifi_sample;

    bool presentation_status_present;
    MeisoCompositionEdgePresentationStatus presentation_status;
    bool scene_status_present;
    MeisoCompositionSceneStatus scene_status;
    bool render_result_present;
    MeisoCompositionSceneRenderResult render_result;

    bool device_effective_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_sample_present;
    MeisoDeviceTelemetrySample device_sample;

    bool render_media_actual_present;
    MeisoCompositionEdgeMediaActual render_media_actual;
    bool camera_media_actual_present;
    MeisoCompositionEdgeMediaActual camera_media_actual;
} MeisoCompositionEdgeSnapshot;

typedef struct {
    uint64_t continuity_serial;
    MeisoCompositionMediaSource source;
    uint64_t media_stream_epoch;
    uint64_t frame_sequence;
} MeisoEdgeMediaFrameKey;

typedef struct {
    MeisoEdgeMediaFrameKey key;
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
} MeisoEdgeMediaFrameView;

MeisoEdgeStatus meiso_composition_edge_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_set_scene(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_set_presentation(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionEdgePresentationSettings *settings,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_set_wifi(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_set_device(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoDeviceTelemetryParameters *parameters,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_set_render_media(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoEdgeCommandReceipt *out_receipt
);
MeisoEdgeStatus meiso_composition_edge_set_camera_media(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoEdgeCommandReceipt *out_receipt
);

MeisoEdgeStatus meiso_composition_edge_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
);

/*
 * Successful latest returns an access-unit borrow. The borrow ends on exact
 * key release, same-route replacement, media/session/catalog boundary or
 * backend terminal. No SessionId, route key, channel nonce or fd is exposed.
 */
MeisoEdgeStatus meiso_composition_edge_media_latest(
    MeisoEdgeHandle *handle,
    MeisoCompositionMediaSource source,
    MeisoEdgeMediaFrameView *out_frame
);
MeisoEdgeStatus meiso_composition_edge_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
);

#ifdef __cplusplus
}
#endif

#endif
