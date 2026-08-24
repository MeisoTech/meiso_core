#ifndef MEISO_COMPOSITION_HOST_APP_H
#define MEISO_COMPOSITION_HOST_APP_H

#include "composition_debug_peer.h"
#include "composition_media_posix_tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native Host-side Runtime~SDK facade for Composition Profile v3.
 *
 * The facade owns one Host CompositionDebugPeer, six desired State
 * publishers, six actual State replicas, three inline destination Streams,
 * one bounded outgoing mesh transfer, and a caller-selected number of
 * one-use external-media transport/adapter slots. It owns no socket, thread,
 * filesystem path, JSON value, HTTP/SSE connection, renderer or Edge store.
 * Browser code must call a separate typed bridge; it never supplies wire
 * frames, State versions, delivery generations, ContentIds, transfer IDs,
 * production epochs, Stream sequences, plan generations, digests or tokens.
 *
 * All functions are single-threaded and non-reentrant. No function allocates
 * heap storage. Every large State/Stream/Blob/media scratch region is part of
 * the caller arena before init succeeds.
 */

#define MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT 6U
#define MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT 6U
#define MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT 3U
#define MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT 2U
#define MEISO_COMPOSITION_HOST_APP_INLINE_QUEUE_SLOTS 4U
#define MEISO_COMPOSITION_HOST_APP_MAX_MEDIA_CONNECTIONS 4U
#define MEISO_COMPOSITION_HOST_APP_CONTROL_ADAPTER_MIN_RESERVE_BYTES 65536U

typedef struct MeisoCompositionHostApp MeisoCompositionHostApp;

typedef uint8_t MeisoCompositionHostAppMediaRouteMask;
enum {
    MEISO_COMPOSITION_HOST_APP_MEDIA_RENDER = UINT8_C(1) << 0U,
    MEISO_COMPOSITION_HOST_APP_MEDIA_CAMERA = UINT8_C(1) << 1U,
    MEISO_COMPOSITION_HOST_APP_MEDIA_ALL = UINT8_C(0x03)
};

typedef uint8_t MeisoCompositionHostAppDesiredChannel;
enum {
    MEISO_COMPOSITION_HOST_APP_DESIRED_WIFI = 0,
    MEISO_COMPOSITION_HOST_APP_DESIRED_PRESENTATION = 1,
    MEISO_COMPOSITION_HOST_APP_DESIRED_SCENE = 2,
    MEISO_COMPOSITION_HOST_APP_DESIRED_DEVICE = 3,
    MEISO_COMPOSITION_HOST_APP_DESIRED_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_HOST_APP_DESIRED_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoCompositionHostAppActualChannel;
enum {
    MEISO_COMPOSITION_HOST_APP_ACTUAL_WIFI = 0,
    MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION = 1,
    MEISO_COMPOSITION_HOST_APP_ACTUAL_SCENE = 2,
    MEISO_COMPOSITION_HOST_APP_ACTUAL_DEVICE = 3,
    MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoCompositionHostAppInlineChannel;
enum {
    MEISO_COMPOSITION_HOST_APP_INLINE_WIFI = 0,
    MEISO_COMPOSITION_HOST_APP_INLINE_RENDER_RESULT = 1,
    MEISO_COMPOSITION_HOST_APP_INLINE_DEVICE = 2
};

typedef struct {
    /* Already-connected nonblocking control socket; app never closes it. */
    int control_connected_fd;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce local_nonce;
    MeisoPeerId expected_edge_peer_id;
    MeisoProtocolVersion protocol;
    uint64_t catalog_generation;
    MeisoCompositionSessionV3CatalogConfig catalog;
    size_t max_control_io_bytes_per_step;

    /* Passed to the owned bootstrap peer. Adapter reserve must be at least
     * CONTROL_ADAPTER_MIN_RESERVE_BYTES; Runtime/Scheduler are nonzero. */
    size_t control_adapter_reserve_bytes;
    size_t control_runtime_reserve_bytes;
    size_t control_scheduler_reserve_bytes;

    /*
     * Nonzero first generation used by each independent desired publisher.
     * The app owns all later delivery generations and State versions.
     */
    MeisoDeliveryGeneration initial_desired_delivery_generation;

    /*
     * Each bind consumes one never-reused transport+adapter arena slot. Every
     * slot reserves the worst-case two-route payload capacity before enable;
     * a one-route bind neither reads nor aliases the inactive reservation.
     * 1..MAX_MEDIA_CONNECTIONS; max_media_io_bytes_per_step is nonzero.
     */
    uint8_t media_connection_slots;
    size_t max_media_io_bytes_per_step;
} MeisoCompositionHostAppConfig;

typedef uint8_t MeisoCompositionHostAppInitStatus;
enum {
    MEISO_COMPOSITION_HOST_APP_INIT_OK = 0,
    MEISO_COMPOSITION_HOST_APP_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_HOST_APP_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_HOST_APP_INIT_FD_ERROR = 3,
    MEISO_COMPOSITION_HOST_APP_INIT_MISALIGNED_ARENA = 4,
    MEISO_COMPOSITION_HOST_APP_INIT_ARENA_TOO_SMALL = 5,
    MEISO_COMPOSITION_HOST_APP_INIT_CAPACITY = 6,
    MEISO_COMPOSITION_HOST_APP_INIT_INTERNAL_FAILURE = 7
};

typedef uint8_t MeisoCompositionHostAppStatus;
enum {
    MEISO_COMPOSITION_HOST_APP_OK = 0,
    MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_HOST_APP_INVALID_STATE = 2,
    MEISO_COMPOSITION_HOST_APP_CAPACITY = 3,
    MEISO_COMPOSITION_HOST_APP_BUSY = 4,
    MEISO_COMPOSITION_HOST_APP_CONFLICT = 5,
    MEISO_COMPOSITION_HOST_APP_NOT_READY = 6,
    MEISO_COMPOSITION_HOST_APP_NO_FRAME = 7,
    MEISO_COMPOSITION_HOST_APP_FRAME_MISMATCH = 8,
    MEISO_COMPOSITION_HOST_APP_COUNTER_EXHAUSTED = 9,
    MEISO_COMPOSITION_HOST_APP_TERMINAL = 10,
    MEISO_COMPOSITION_HOST_APP_INTERNAL_FAILURE = 11
};

typedef uint8_t MeisoCompositionHostAppCommandKind;
enum {
    MEISO_COMPOSITION_HOST_APP_COMMAND_PUT_MESH = 1,
    MEISO_COMPOSITION_HOST_APP_COMMAND_SET_SCENE = 2,
    MEISO_COMPOSITION_HOST_APP_COMMAND_SET_PRESENTATION = 3,
    MEISO_COMPOSITION_HOST_APP_COMMAND_SET_WIFI = 4,
    MEISO_COMPOSITION_HOST_APP_COMMAND_SET_DEVICE = 5,
    MEISO_COMPOSITION_HOST_APP_COMMAND_SET_RENDER_MEDIA = 6,
    MEISO_COMPOSITION_HOST_APP_COMMAND_SET_CAMERA_MEDIA = 7
};

typedef uint8_t MeisoCompositionHostAppLocalQueueResult;
enum {
    MEISO_COMPOSITION_HOST_APP_LOCAL_QUEUED = 1,
    MEISO_COMPOSITION_HOST_APP_LOCAL_REPLACED_PENDING = 2
};

/*
 * A successful command receipt proves only an in-arena local enqueue/copy.
 * It never means wire transmission, Edge receipt, desired acceptance, actual
 * application, Blob visibility, render completion or media availability.
 */
typedef struct {
    uint64_t command_id;
    MeisoCompositionHostAppCommandKind kind;
    MeisoCompositionHostAppLocalQueueResult local_result;
    /* Exact old local command made terminal by LOCAL_REPLACED_PENDING. */
    bool replaced_command_present;
    uint64_t replaced_command_id;
    bool desired_state_present;
    MeisoCompositionHostAppDesiredChannel desired_channel;
    uint64_t desired_state_version;
    bool plan_generation_present;
    uint64_t plan_generation;
    bool mesh_content_id_present;
    MeisoContentId mesh_content_id;
} MeisoCompositionHostAppCommandReceipt;

/*
 * Presentation fields owned by the application caller. The Host app fixes
 * scene_binding/source/geometry/capability generation, allocates a strictly
 * increasing plan generation, computes the canonical digest, and may perform
 * a candidate-first higher-State-version prepared-token echo. Echo is
 * permitted only for the current exact PlanRef/digest when a PREPARED
 * actual's observed_desired matches the current desired State version. A
 * stale, replaced, disabled or mismatched actual never queues an echo.
 */
typedef struct {
    MeisoCompositionScenePlanFlags flags;
    uint16_t target_width;
    uint16_t target_height;
    MeisoCompositionSceneColorFormat color_format;
    MeisoCompositionSceneDepthFormat depth_format;
    uint16_t max_result_hz;
    uint32_t offline_lease_ms;
} MeisoCompositionHostAppPresentationSettings;

typedef struct {
    bool requested;
    bool enabled;
    uint64_t command_id;
    MeisoOwnerScope owner;
    uint64_t state_version;
    MeisoDeliveryGeneration delivery_generation;
    bool delivery_active;
    bool delivery_pending;
    bool delivery_accepted;
    bool delivery_failed;
    uint64_t replaced_pending_count;

    /*
     * Exact older value whose copied payload is currently borrowed by the
     * control scheduler, sent but not yet accepted, or accepted while the
     * matching Actual.observed_desired is still outstanding.  The public
     * command fields above always describe the latest locally coalesced
     * value; accepting this older value never marks that latest value
     * accepted.
     */
    bool inflight_present;
    uint64_t inflight_command_id;
    MeisoOwnerScope inflight_owner;
    uint64_t inflight_state_version;
    MeisoDeliveryGeneration inflight_delivery_generation;
    bool inflight_source_completed;
    bool inflight_offer_accepted;
    bool inflight_actual_observed;
} MeisoCompositionHostAppDesiredSnapshot;

/* Profile bytes are decoded into the typed fields in the aggregate snapshot. */
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
} MeisoCompositionHostAppActualSnapshot;

/* Sticky decoded latest; the owned Core Stream itself is drained by pump. */
typedef struct {
    bool present;
    MeisoStreamKey key;
    MeisoTimePoint source_time;
    MeisoStreamReceiveFlags receive_flags;
    uint64_t missing_before;
    uint64_t received_count;
    uint64_t replaced_count;
} MeisoCompositionHostAppInlineSnapshot;

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
    uint64_t transfer_id_high_water;
    MeisoRuntimeBlobResult terminal_result;
    /* PUBLISHED/DUPLICATE means Edge Core BlobStore only, not Scene visible. */
    bool edge_core_published;
} MeisoCompositionHostAppMeshSnapshot;

typedef struct {
    bool actual_binding_ready;
    bool latest_present;
    MeisoCompositionMediaSource source;
    MeisoCompositionMediaBinding binding;
    uint64_t latest_frame_sequence;
    MeisoTimePoint latest_source_time;
    uint32_t latest_access_unit_bytes;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
} MeisoCompositionHostAppMediaRouteSnapshot;

typedef uint8_t MeisoCompositionHostAppPhase;
enum {
    MEISO_COMPOSITION_HOST_APP_PHASE_HANDSHAKE = 1,
    MEISO_COMPOSITION_HOST_APP_PHASE_CATALOG = 2,
    MEISO_COMPOSITION_HOST_APP_PHASE_REBIND = 3,
    MEISO_COMPOSITION_HOST_APP_PHASE_READY = 4,
    MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL = 5
};

typedef uint8_t MeisoCompositionHostAppTerminalReason;
enum {
    MEISO_COMPOSITION_HOST_APP_TERMINAL_NONE = 0,
    MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_TRANSPORT = 1,
    MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_SESSION = 2,
    MEISO_COMPOSITION_HOST_APP_TERMINAL_CATALOG_CHANGED = 3,
    MEISO_COMPOSITION_HOST_APP_TERMINAL_LOCAL_ABORT = 4,
    MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE = 5
};

typedef struct {
    MeisoCompositionHostAppPhase phase;
    MeisoCompositionHostAppTerminalReason terminal_reason;
    MeisoSessionState control_session_state;
    MeisoSessionId control_session_id;
    uint64_t active_catalog_generation;
    bool control_ready;
    bool media_bound;
    MeisoCompositionHostAppMediaRouteMask media_active_route_mask;
    uint8_t media_slots_consumed;
    uint8_t media_slots_capacity;

    MeisoCompositionHostAppDesiredSnapshot
        desired[MEISO_COMPOSITION_HOST_APP_DESIRED_COUNT];
    MeisoCompositionHostAppActualSnapshot
        actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT];
    MeisoCompositionHostAppInlineSnapshot
        inline_streams[MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT];
    MeisoCompositionHostAppMeshSnapshot mesh;
    MeisoCompositionHostAppMediaRouteSnapshot
        media[MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT];

    bool wifi_effective_present;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_sample_present;
    MeisoWifiTelemetrySample wifi_sample;

    bool presentation_status_present;
    MeisoCompositionScenePresentationStatus presentation_status;
    bool scene_status_present;
    MeisoCompositionSceneStatus scene_status;
    bool render_result_present;
    MeisoCompositionSceneRenderResult render_result;

    bool device_effective_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_sample_present;
    MeisoDeviceTelemetrySample device_sample;

    bool render_media_actual_present;
    MeisoCompositionMediaActual render_media_actual;
    bool camera_media_actual_present;
    MeisoCompositionMediaActual camera_media_actual;

    uint64_t next_command_id;
    uint64_t plan_generation_high_water;
    size_t arena_bytes;
    size_t control_peer_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t mesh_staging_bytes;
    size_t media_slot_bytes;
} MeisoCompositionHostAppSnapshot;

typedef struct {
    bool control_readable;
    bool control_writable;
    bool media_readable;
    bool media_writable;
} MeisoCompositionHostAppIoReady;

typedef struct {
    bool control_read;
    bool control_write;
    bool media_read;
    bool media_write;
} MeisoCompositionHostAppIoInterest;

typedef uint8_t MeisoCompositionHostAppPumpEvent;
enum {
    MEISO_COMPOSITION_HOST_APP_PUMP_CONTROL_PROGRESS = 1,
    MEISO_COMPOSITION_HOST_APP_PUMP_DESIRED_DELIVERY = 2,
    MEISO_COMPOSITION_HOST_APP_PUMP_ACTUAL_UPDATED = 3,
    MEISO_COMPOSITION_HOST_APP_PUMP_INLINE_UPDATED = 4,
    MEISO_COMPOSITION_HOST_APP_PUMP_MESH_PROGRESS = 5,
    MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_PROGRESS = 6,
    MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_FRAME = 7,
    MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY = 8,
    MEISO_COMPOSITION_HOST_APP_PUMP_EVENT_TERMINAL = 9
};

typedef uint8_t MeisoCompositionHostAppPumpStatus;
enum {
    MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS = 0,
    MEISO_COMPOSITION_HOST_APP_PUMP_IDLE = 1,
    MEISO_COMPOSITION_HOST_APP_PUMP_WOULD_BLOCK = 2,
    MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL = 3,
    MEISO_COMPOSITION_HOST_APP_PUMP_INVALID_ARGUMENT = 4,
    MEISO_COMPOSITION_HOST_APP_PUMP_INTERNAL_FAILURE = 5
};

typedef struct {
    MeisoCompositionHostAppPumpEvent event;
    bool event_present;
    bool command_present;
    uint64_t command_id;
    bool desired_channel_present;
    MeisoCompositionHostAppDesiredChannel desired_channel;
    bool actual_channel_present;
    MeisoCompositionHostAppActualChannel actual_channel;
    bool inline_channel_present;
    MeisoCompositionHostAppInlineChannel inline_channel;
    bool media_source_present;
    MeisoCompositionMediaSource media_source;
} MeisoCompositionHostAppPumpResult;

size_t meiso_composition_host_app_alignment_required(void);

/*
 * Returns the exact single-arena requirement, including all configured media
 * slots. It performs no fd operation. Zero means invalid config or overflow.
 * config, arena and output ranges must be mutually disjoint. init validates
 * an exact Host/Profile-v3 configuration, starts the owned control peer, and
 * copies all catalog/config truth. It never creates, closes or changes flags
 * on a socket. Every preflight failure (argument/config/fd/alignment/size or
 * capacity) writes no arena byte and leaves out_app unchanged. Once the exact
 * same config and a caller-stable fd have passed all preflight, a defensive
 * child INTERNAL_FAILURE still leaves out_app unchanged but invalidates arena
 * contents; that arena must not be treated as a prior/live app or reused
 * without reinitialization.
 * Unless a function says otherwise, every borrowed input, public output and
 * the complete app arena must be mutually disjoint. Detectable alias is an
 * invalid argument and every failure is atomic with output bytes unchanged.
 */
size_t meiso_composition_host_app_memory_required(
    const MeisoCompositionHostAppConfig *config
);
MeisoCompositionHostAppInitStatus meiso_composition_host_app_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionHostAppConfig *config,
    MeisoCompositionHostApp **out_app
);

/*
 * Mesh bytes must be one complete canonical MSH0 value. The app validates it,
 * computes SHA-256 ContentId and copies the full value before returning. Only
 * one mesh command may be queued/in-flight. Output ranges and mesh input must
 * be disjoint from one another and the full app arena. Failure is atomic.
 */
MeisoCompositionHostAppStatus meiso_composition_host_app_put_mesh(
    MeisoCompositionHostApp *app,
    MeisoByteView canonical_mesh,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);

/*
 * enabled=false requires value/settings==NULL and emits prefix-only desired.
 * Commands may be queued during handshake; wire delivery starts only after
 * exact Profile-v3 READY. Each channel owns one immutable copied payload for
 * an exact wire in-flight offer while its StatePublisher may independently
 * retain/coalesce the latest local value. A later same-channel command names
 * a replaced command only when that command was still local and had not
 * become the exact in-flight offer; an already queued/sent offer completes
 * under its own command_id. The next coalesced value is not offered until the
 * old exact offer has both been accepted and appeared in a successfully
 * decoded Actual.observed_desired (an apply failure still counts as observed).
 * Presentation token echo is an internal higher State version retaining the
 * originating command_id and obeys the same per-channel barrier; it does not
 * synthesize a second user command receipt.
 */
MeisoCompositionHostAppStatus meiso_composition_host_app_set_scene(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);
MeisoCompositionHostAppStatus meiso_composition_host_app_set_presentation(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionHostAppPresentationSettings *settings,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);
MeisoCompositionHostAppStatus meiso_composition_host_app_set_wifi(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);
MeisoCompositionHostAppStatus meiso_composition_host_app_set_device(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoDeviceTelemetryParameters *parameters,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);
MeisoCompositionHostAppStatus meiso_composition_host_app_set_render_media(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);
MeisoCompositionHostAppStatus meiso_composition_host_app_set_camera_media(
    MeisoCompositionHostApp *app,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostAppCommandReceipt *out_receipt
);

/*
 * Binds a caller-owned, already-connected nonblocking media socket. The app
 * derives the exact nonzero route mask from current RUNNING Actual bindings;
 * active_routes is the caller's expected-mask assertion (0x1, 0x2 or 0x3),
 * not a route selector, and mismatch is INVALID_STATE with no mutation. No
 * inactive Actual contributes a binding; its transport route config remains
 * canonical and ignored. OPEN stays common and only derived routes BIND in
 * canonical render-then-camera order. Every bind consumes a never-reused
 * preallocated media slot. unbind terminates that slot, invalidates media
 * borrows and never closes fd; a later bind consumes the next slot and
 * repeats OPEN/BIND. Any selected actual enable/disable, binding, production
 * epoch or catalog change automatically terminates the current media scope.
 * Before inspecting or mutating the fd/slot, bind returns NOT_READY while
 * either media Desired is in flight, pending/unaccepted/failed, or its latest
 * exact owner/version has not been both observed and applied by the matching
 * Actual. A channel never requested locally is converged only while no
 * unsolicited Actual is present. Thus a mask is derived exactly once from
 * settled two-channel truth, never from the transient interval between a rapid
 * Desired and its Actual.
 * The caller must supply a fresh media TCP byte stream: it must not be the
 * control socket, a dup/alias of the control socket or any previously used
 * media stream. The app machine-rejects only numeric equality with its current
 * control fd; portable underlying-socket identity and fd-number reuse remain
 * caller-owned preconditions.
 */
MeisoCompositionHostAppStatus meiso_composition_host_app_bind_media_fd(
    MeisoCompositionHostApp *app,
    int connected_fd,
    MeisoCompositionHostAppMediaRouteMask active_routes
);
MeisoCompositionHostAppStatus meiso_composition_host_app_unbind_media_fd(
    MeisoCompositionHostApp *app,
    int error_number
);

/*
 * One pump call performs at most one bounded control, local command, or media
 * action under a persistent fairness cursor. It resolves Runtime RX
 * obligations internally before another peer call. No raw Runtime event or
 * wire borrow escapes. WOULD_BLOCK/invalid calls leave out_result unchanged;
 * a bounded domain attempt may advance the fairness cursor, including a
 * fail-closed TX_BUSY attempt made without a syscall.
 */
MeisoCompositionHostAppPumpStatus meiso_composition_host_app_pump(
    MeisoCompositionHostApp *app,
    const MeisoCompositionHostAppIoReady *ready,
    MeisoCompositionHostAppPumpResult *out_result
);

/* Pure level-triggered interests; false leaves out_interest unchanged. */
bool meiso_composition_host_app_readiness(
    const MeisoCompositionHostApp *app,
    MeisoCompositionHostAppIoInterest *out_interest
);

/*
 * Host-only external-media borrow. It never aliases snapshot storage.
 * Successful release is exact source/binding/sequence. The borrow ends on
 * release, replacement on that route, unbind, catalog/session boundary or
 * abort. Mutable app calls for unrelated routes do not invalidate it.
 */
MeisoCompositionHostAppStatus meiso_composition_host_app_media_latest(
    const MeisoCompositionHostApp *app,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest
);
MeisoCompositionHostAppStatus meiso_composition_host_app_media_release(
    MeisoCompositionHostApp *app,
    const MeisoCompositionMediaFrameRef *expected_frame
);

/*
 * Pure value-copy snapshot. Desired delivery, Edge actual, inline Stream and
 * external media fields are deliberately independent; no field implies a
 * later stage. For example desired.delivery_accepted never implies applied,
 * mesh.edge_core_published never implies SceneStore visibility, and an actual
 * RUNNING state never implies that a Stream sample/media frame was received.
 */
bool meiso_composition_host_app_snapshot(
    const MeisoCompositionHostApp *app,
    MeisoCompositionHostAppSnapshot *out_snapshot
);

/*
 * Sticky terminal boundary. It makes local commands and all received-data
 * views unavailable, terminates media slots/borrows, aborts the owned peer,
 * and never closes either fd. It does not mutate opaque Core state by layout.
 * A catalog-generation change or Session terminal applies the same boundary.
 * Same-Session catalog replacement is intentionally unsupported and terminal;
 * a fresh app must not attach to the already-used control byte stream. A fresh
 * socket/Session requires a fresh HostApp init and fresh arena: State
 * delivery, Blob transfer, plan token, production epoch, Stream sequence and
 * media binding are never resumed or resurrected across instances. NONE,
 * unknown reason and reentrant abort are rejected with no app mutation.
 */
MeisoCompositionHostAppStatus meiso_composition_host_app_abort(
    MeisoCompositionHostApp *app,
    MeisoCompositionHostAppTerminalReason reason,
    int error_number
);

#ifdef __cplusplus
}
#endif

#endif
