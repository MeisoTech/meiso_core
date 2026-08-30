#ifndef MEISO_HOST_COMPOSITION_RUNTIME_H
#define MEISO_HOST_COMPOSITION_RUNTIME_H

#include <meiso/composition/control_session.h>
#include <meiso/adapters/posix/composition_media_tcp.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native Host-side Runtime/SDK facade for the Composition Profile.
 *
 * The facade owns one Host CompositionControlSession, six desired State
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

#define MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT 6U
#define MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT 6U
#define MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT 3U
#define MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT 2U
#define MEISO_COMPOSITION_HOST_RUNTIME_INLINE_QUEUE_SLOTS 4U
#define MEISO_COMPOSITION_HOST_RUNTIME_MAX_MEDIA_CONNECTIONS 4U
#define MEISO_COMPOSITION_HOST_RUNTIME_CONTROL_ADAPTER_MIN_RESERVE_BYTES 65536U

typedef struct MeisoCompositionHostRuntime MeisoCompositionHostRuntime;

typedef uint8_t MeisoCompositionHostRuntimeMediaRouteMask;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER = UINT8_C(1) << 0U,
    MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA = UINT8_C(1) << 1U,
    MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ALL = UINT8_C(0x03)
};

typedef uint8_t MeisoCompositionHostRuntimeDesiredChannel;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_SCENE = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_DEVICE = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoCompositionHostRuntimeActualChannel;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_SCENE = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_DEVICE = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoCompositionHostRuntimeInlineChannel;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_INLINE_WIFI = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_INLINE_RENDER_RESULT = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_INLINE_DEVICE = 2
};

typedef struct {
    /* Already-connected nonblocking control socket; runtime never closes it. */
    int control_connected_fd;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce local_nonce;
    MeisoPeerId expected_edge_peer_id;
    MeisoProtocolVersion protocol;
    uint64_t catalog_generation;
    MeisoCompositionProfileCatalogConfig catalog;
    size_t max_control_io_bytes_per_step;

    /*
     * Passed to the owned bootstrap ControlSession. Adapter reserve must be at
     * least CONTROL_ADAPTER_MIN_RESERVE_BYTES; Runtime/Scheduler are nonzero.
     */
    size_t control_adapter_reserve_bytes;
    size_t control_runtime_reserve_bytes;
    size_t control_scheduler_reserve_bytes;

    /*
     * Nonzero first generation used by each independent desired publisher.
     * The runtime owns all later delivery generations and State versions.
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
} MeisoCompositionHostRuntimeConfig;

typedef uint8_t MeisoCompositionHostRuntimeInitStatus;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_OK = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_FD_ERROR = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_MISALIGNED_ARENA = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_ARENA_TOO_SMALL = 5,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_CAPACITY = 6,
    MEISO_COMPOSITION_HOST_RUNTIME_INIT_INTERNAL_FAILURE = 7
};

typedef uint8_t MeisoCompositionHostRuntimeStatus;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_OK = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_CAPACITY = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_BUSY = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT = 5,
    MEISO_COMPOSITION_HOST_RUNTIME_NOT_READY = 6,
    MEISO_COMPOSITION_HOST_RUNTIME_NO_FRAME = 7,
    MEISO_COMPOSITION_HOST_RUNTIME_FRAME_MISMATCH = 8,
    MEISO_COMPOSITION_HOST_RUNTIME_COUNTER_EXHAUSTED = 9,
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL = 10,
    MEISO_COMPOSITION_HOST_RUNTIME_INTERNAL_FAILURE = 11
};

typedef uint8_t MeisoCompositionHostRuntimeCommandKind;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_PUT_MESH = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_SCENE = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_PRESENTATION = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_WIFI = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_DEVICE = 5,
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_RENDER_MEDIA = 6,
    MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_CAMERA_MEDIA = 7
};

typedef uint8_t MeisoCompositionHostRuntimeLocalQueueResult;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING = 2
};

/*
 * A successful command receipt proves only an in-arena local enqueue/copy.
 * It never means wire transmission, Edge receipt, desired acceptance, actual
 * application, Blob visibility, render completion or media availability.
 */
typedef struct {
    uint64_t command_id;
    MeisoCompositionHostRuntimeCommandKind kind;
    MeisoCompositionHostRuntimeLocalQueueResult local_result;
    /* Exact old local command made terminal by LOCAL_REPLACED_PENDING. */
    bool replaced_command_present;
    uint64_t replaced_command_id;
    bool desired_state_present;
    MeisoCompositionHostRuntimeDesiredChannel desired_channel;
    uint64_t desired_state_version;
    bool plan_generation_present;
    uint64_t plan_generation;
    bool mesh_content_id_present;
    MeisoContentId mesh_content_id;
} MeisoCompositionHostRuntimeCommandReceipt;

/*
 * Presentation fields owned by the application caller. The Host Runtime fixes
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
} MeisoCompositionHostRuntimePresentationSettings;

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
} MeisoCompositionHostRuntimeDesiredSnapshot;

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
} MeisoCompositionHostRuntimeActualSnapshot;

/* Sticky decoded latest; the owned Core Stream itself is drained by pump. */
typedef struct {
    bool present;
    MeisoStreamKey key;
    MeisoTimePoint source_time;
    MeisoStreamReceiveFlags receive_flags;
    uint64_t missing_before;
    uint64_t received_count;
    uint64_t replaced_count;
} MeisoCompositionHostRuntimeInlineSnapshot;

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
} MeisoCompositionHostRuntimeMeshSnapshot;

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
} MeisoCompositionHostRuntimeMediaRouteSnapshot;

typedef uint8_t MeisoCompositionHostRuntimePhase;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_PHASE_HANDSHAKE = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_PHASE_CATALOG = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_PHASE_REBIND = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL = 5
};

typedef uint8_t MeisoCompositionHostRuntimeTerminalReason;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_NONE = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_TRANSPORT = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_SESSION = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CATALOG_CHANGED = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE = 5
};

typedef struct {
    MeisoCompositionHostRuntimePhase phase;
    MeisoCompositionHostRuntimeTerminalReason terminal_reason;
    MeisoSessionState control_session_state;
    MeisoSessionId control_session_id;
    uint64_t active_catalog_generation;
    bool control_ready;
    bool media_bound;
    MeisoCompositionHostRuntimeMediaRouteMask media_active_route_mask;
    uint8_t media_slots_consumed;
    uint8_t media_slots_capacity;

    MeisoCompositionHostRuntimeDesiredSnapshot
        desired[MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT];
    MeisoCompositionHostRuntimeActualSnapshot
        actual[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT];
    MeisoCompositionHostRuntimeInlineSnapshot
        inline_streams[MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT];
    MeisoCompositionHostRuntimeMeshSnapshot mesh;
    MeisoCompositionHostRuntimeMediaRouteSnapshot
        media[MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT];

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
    size_t control_session_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t mesh_staging_bytes;
    size_t media_slot_bytes;
} MeisoCompositionHostRuntimeSnapshot;

typedef struct {
    bool control_readable;
    bool control_writable;
    bool media_readable;
    bool media_writable;
} MeisoCompositionHostRuntimeIoReady;

typedef struct {
    bool control_read;
    bool control_write;
    bool media_read;
    bool media_write;
} MeisoCompositionHostRuntimeIoInterest;

typedef uint8_t MeisoCompositionHostRuntimePumpEvent;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_CONTROL_PROGRESS = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_DESIRED_DELIVERY = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INLINE_UPDATED = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MESH_PROGRESS = 5,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_PROGRESS = 6,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_FRAME = 7,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY = 8,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_EVENT_TERMINAL = 9
};

typedef uint8_t MeisoCompositionHostRuntimePumpStatus;
enum {
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS = 0,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE = 1,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK = 2,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL = 3,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INVALID_ARGUMENT = 4,
    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE = 5
};

typedef struct {
    MeisoCompositionHostRuntimePumpEvent event;
    bool event_present;
    bool command_present;
    uint64_t command_id;
    bool desired_channel_present;
    MeisoCompositionHostRuntimeDesiredChannel desired_channel;
    bool actual_channel_present;
    MeisoCompositionHostRuntimeActualChannel actual_channel;
    bool inline_channel_present;
    MeisoCompositionHostRuntimeInlineChannel inline_channel;
    bool media_source_present;
    MeisoCompositionMediaSource media_source;
} MeisoCompositionHostRuntimePumpResult;

size_t meiso_composition_host_runtime_alignment_required(void);

/*
 * Returns the exact single-arena requirement, including all configured media
 * slots. It performs no fd operation. Zero means invalid config or overflow.
 * config, arena and output ranges must be mutually disjoint. init validates
 * an exact Host/Profile configuration, starts the owned control session, and
 * copies all catalog/config truth. It never creates, closes or changes flags
 * on a socket. Every preflight failure (argument/config/fd/alignment/size or
 * capacity) writes no arena byte and leaves out_runtime unchanged. Once the exact
 * same config and a caller-stable fd have passed all preflight, a defensive
 * child INTERNAL_FAILURE still leaves out_runtime unchanged but invalidates arena
 * contents; that arena must not be treated as a prior/live runtime or reused
 * without reinitialization.
 * Unless a function says otherwise, every borrowed input, public output and
 * the complete runtime arena must be mutually disjoint. Detectable alias is an
 * invalid argument and every failure is atomic with output bytes unchanged.
 */
size_t meiso_composition_host_runtime_memory_required(
    const MeisoCompositionHostRuntimeConfig *config
);
MeisoCompositionHostRuntimeInitStatus meiso_composition_host_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionHostRuntimeConfig *config,
    MeisoCompositionHostRuntime **out_runtime
);

/*
 * Mesh bytes must be one complete canonical MSH0 value. The runtime validates it,
 * computes SHA-256 ContentId and copies the full value before returning. Only
 * one mesh command may be queued/in-flight. Output ranges and mesh input must
 * be disjoint from one another and the full runtime arena. Failure is atomic.
 */
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_put_mesh(
    MeisoCompositionHostRuntime *runtime,
    MeisoByteView canonical_mesh,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);

/*
 * enabled=false requires value/settings==NULL and emits prefix-only desired.
 * Commands may be queued during handshake; wire delivery starts only after
 * exact Composition Profile READY. Each channel owns one immutable copied payload for
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
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_scene(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_presentation(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionHostRuntimePresentationSettings *settings,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_wifi(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_device(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoDeviceTelemetryParameters *parameters,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_render_media(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_camera_media(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
);

/*
 * Binds a caller-owned, already-connected nonblocking media socket. The runtime
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
 * media stream. The runtime machine-rejects only numeric equality with its current
 * control fd; portable underlying-socket identity and fd-number reuse remain
 * caller-owned preconditions.
 */
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_bind_media_fd(
    MeisoCompositionHostRuntime *runtime,
    int connected_fd,
    MeisoCompositionHostRuntimeMediaRouteMask active_routes
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_unbind_media_fd(
    MeisoCompositionHostRuntime *runtime,
    int error_number
);

/*
 * One pump call performs at most one bounded control, local command, or media
 * action under a persistent fairness cursor. It resolves Runtime RX
 * obligations internally before another ControlSession call. No raw Runtime event or
 * wire borrow escapes. WOULD_BLOCK/invalid calls leave out_result unchanged;
 * a bounded domain attempt may advance the fairness cursor, including a
 * fail-closed TX_BUSY attempt made without a syscall.
 */
MeisoCompositionHostRuntimePumpStatus meiso_composition_host_runtime_pump(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionHostRuntimeIoReady *ready,
    MeisoCompositionHostRuntimePumpResult *out_result
);

/* Pure level-triggered interests; false leaves out_interest unchanged. */
bool meiso_composition_host_runtime_readiness(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeIoInterest *out_interest
);

/*
 * Host-only external-media borrow. It never aliases snapshot storage.
 * Successful release is exact source/binding/sequence. The borrow ends on
 * release, replacement on that route, unbind, catalog/session boundary or
 * abort. Mutable runtime calls for unrelated routes do not invalidate it.
 */
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_media_latest(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest
);
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_media_release(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionMediaFrameRef *expected_frame
);

/*
 * Pure value-copy snapshot. Desired delivery, Edge actual, inline Stream and
 * external media fields are deliberately independent; no field implies a
 * later stage. For example desired.delivery_accepted never implies applied,
 * mesh.edge_core_published never implies SceneStore visibility, and an actual
 * RUNNING state never implies that a Stream sample/media frame was received.
 */
bool meiso_composition_host_runtime_snapshot(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeSnapshot *out_snapshot
);

/*
 * Sticky terminal boundary. It makes local commands and all received-data
 * views unavailable, terminates media slots/borrows, aborts the owned ControlSession,
 * and never closes either fd. It does not mutate opaque Core state by layout.
 * A catalog-generation change or Session terminal applies the same boundary.
 * Same-Session catalog replacement is intentionally unsupported and terminal;
 * a fresh runtime must not attach to the already-used control byte stream. A fresh
 * socket/Session requires a fresh HostRuntime init and fresh arena: State
 * delivery, Blob transfer, plan token, production epoch, Stream sequence and
 * media binding are never resumed or resurrected across instances. NONE,
 * unknown reason and reentrant abort are rejected with no runtime mutation.
 */
MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_abort(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeTerminalReason reason,
    int error_number
);

#ifdef __cplusplus
}
#endif

#endif
