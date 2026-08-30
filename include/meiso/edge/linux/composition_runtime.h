#ifndef MEISO_EDGE_LINUX_COMPOSITION_RUNTIME_H
#define MEISO_EDGE_LINUX_COMPOSITION_RUNTIME_H

#include <meiso/edge/linux/camera_v4l2.h>
#include <meiso/composition/coordinator.h>
#include <meiso/composition/cpu_raster_renderer.h>
#include <meiso/composition/control_session.h>
#include <meiso/adapters/posix/composition_media_tcp.h>
#include <meiso/edge/linux/device_telemetry.h>
#include <meiso/edge/linux/wifi.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * K230D Linux Composition Edge Runtime integration seam.
 *
 * One runtime owns one CompositionControlSession, the six Host-desired State replicas,
 * six Edge-actual State publishers, three inline Stream publishers, a bounded
 * Mesh BlobStore, SceneStore, C2 Coordinator, CPU raster renderer, Linux
 * Wi-Fi/DeviceTelemetry/camera adapters, and caller-selected one-use media
 * transport/adapter slots. None of those handles escapes this API.
 *
 * The runtime owns no control/media listening socket, DNS/discovery, browser,
 * thread, heap allocation, authentication policy or persistent world. It is
 * target-local Runtime~SDK integration, not a new Core/Profile/wire contract.
 * Calls are single-threaded and non-reentrant except the standalone thermal
 * worker function explicitly documented below.
 */

#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT 6U
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT 6U
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT 3U
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_ROUTE_COUNT 2U
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_QUEUE_SLOTS 4U
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MAX_MEDIA_CONNECTIONS 4U
#define MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MESH_BLOB_SLOTS \
    MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS

typedef struct MeisoK230dCompositionEdgeRuntime MeisoK230dCompositionEdgeRuntime;

typedef uint8_t MeisoK230dCompositionEdgeRuntimeMediaRouteMask;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_RENDER = UINT8_C(1) << 0U,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_CAMERA = UINT8_C(1) << 1U,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_ALL = UINT8_C(0x03)
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimeDesiredChannel;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_WIFI = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_PRESENTATION = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_DEVICE = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_RENDER_MEDIA = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimeActualChannel;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA = 5
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimeInlineChannel;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_WIFI = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_DEVICE = 2
};

typedef struct {
    /* Already-connected nonblocking control socket; runtime never closes it. */
    int control_connected_fd;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce local_nonce;
    MeisoPeerId expected_host_peer_id;
    MeisoProtocolVersion protocol;
    uint64_t catalog_generation;

    /* Exact nonzero endpoint incarnations for the offered 6/18 catalog. */
    uint64_t wifi_incarnation;
    uint64_t presentation_incarnation;
    uint64_t scene_incarnation;
    uint64_t device_telemetry_incarnation;
    uint64_t render_media_incarnation;
    uint64_t camera_media_incarnation;

    /* Target-private locators/policy; copied before init returns. */
    MeisoWifiLinuxAdapterConfig wifi;
    MeisoDeviceTelemetryLinuxAdapterConfig device_telemetry;
    MeisoCameraV4l2Config camera;

    /* Exact Edge clock domain used by State/Stream/render/media timestamps. */
    uint64_t edge_clock_epoch;

    /* ControlSession bootstrap reservations; all are nonzero. */
    size_t max_control_io_bytes_per_step;
    size_t control_adapter_reserve_bytes;
    size_t control_runtime_reserve_bytes;
    size_t control_scheduler_reserve_bytes;

    /* First generation for each independently owned actual publisher. */
    MeisoDeliveryGeneration initial_actual_delivery_generation;

    /*
     * One-use media slots reserve the worst-case two-route transport plus
     * POSIX adapter. 1..MAX_MEDIA_CONNECTIONS. Network fds remain caller-owned.
     */
    uint8_t media_connection_slots;
    size_t max_media_io_bytes_per_step;

    /* Nonzero bounded-worker budget, never a synchronous pump sleep. */
    uint32_t thermal_worker_budget_ms;
} MeisoK230dCompositionEdgeRuntimeConfig;

typedef uint8_t MeisoK230dCompositionEdgeRuntimeInitStatus;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_OK = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INVALID_ARGUMENT = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INVALID_CONFIG = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_PROBE_UNAVAILABLE = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_FD_ERROR = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_MISALIGNED_ARENA = 5,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_ARENA_TOO_SMALL = 6,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_CAPACITY = 7,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_PLATFORM_UNSUPPORTED = 8,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_INTERNAL_FAILURE = 9
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimeStatus;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_STATE = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_NOT_READY = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_NO_WORK = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_BUSY = 5,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_STALE = 6,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MISMATCH = 7,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_CAPACITY = 8,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_COUNTER_EXHAUSTED = 9,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL = 10,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INTERNAL_FAILURE = 11
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimePhase;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_HANDSHAKE = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_CATALOG = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_REBIND = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_READY = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL = 5
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimeTerminalReason;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_NONE = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_TRANSPORT = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_SESSION = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CATALOG_CHANGED = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_MEDIA_TRANSPORT = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CAMERA_CLEANUP = 5,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT = 6,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE = 7
};

/*
 * A thermal request is a target-local value copy for exactly one due Device
 * sample. reader contains private procfs/sysfs locators and must never be
 * serialized, logged or retained after this bounded attempt. No runtime pointer
 * is shared with the worker.
 */
typedef struct {
    uint64_t request_id;
    MeisoRuntimeDesiredReference observed_desired;
    uint64_t production_epoch;
    MeisoTimePoint sample_due;
    MeisoTimePoint deadline;
    MeisoDeviceTelemetryLinuxAdapter reader;
} MeisoK230dCompositionEdgeRuntimeThermalRequest;

typedef uint8_t MeisoK230dCompositionEdgeRuntimeThermalReadStatus;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_READY = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_UNAVAILABLE = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_INVALID_ARGUMENT = 2
};

typedef struct {
    uint64_t request_id;
    MeisoRuntimeDesiredReference observed_desired;
    uint64_t production_epoch;
    MeisoDeviceTelemetryLinuxTemperature temperature;
    MeisoK230dCompositionEdgeRuntimeThermalReadStatus read_status;
} MeisoK230dCompositionEdgeRuntimeThermalResult;

typedef struct {
    bool present;
    MeisoRuntimeDesiredReference observed_desired;
    MeisoRuntimeDesiredReference applied_desired;
    MeisoRuntimeApplyResult apply_result;
    MeisoRuntimeAvailability availability;
    bool effective_enabled;
    MeisoRuntimeProductionStatus production_status;
    MeisoRuntimeProductionReason reason;
    uint64_t production_epoch;
    uint64_t actual_state_version;
    MeisoDeliveryGeneration delivery_generation;
    bool delivery_pending;
    bool delivery_accepted;
    bool delivery_failed;
} MeisoK230dCompositionEdgeRuntimeActualSnapshot;

typedef struct {
    bool enabled;
    uint64_t stream_epoch;
    uint64_t next_sequence;
    uint64_t samples_materialized;
    uint64_t samples_accepted;
    uint64_t samples_rejected;
    uint32_t queue_depth;
} MeisoK230dCompositionEdgeRuntimeInlineSnapshot;

typedef struct {
    bool bound;
    MeisoK230dCompositionEdgeRuntimeMediaRouteMask active_route_mask;
    uint8_t slot_index;
    MeisoCompositionMediaTransportPhase transport_phase;
    uint64_t render_submitted_sequence;
    uint64_t camera_submitted_sequence;
    uint64_t render_whole_frames_dropped;
    uint64_t camera_whole_frames_dropped;
} MeisoK230dCompositionEdgeRuntimeMediaSnapshot;

typedef struct {
    MeisoK230dCompositionEdgeRuntimePhase phase;
    MeisoK230dCompositionEdgeRuntimeTerminalReason terminal_reason;
    MeisoSessionState control_session_state;
    MeisoSessionId control_session_id;
    uint64_t active_catalog_generation;
    bool control_ready;

    MeisoK230dCompositionEdgeRuntimeActualSnapshot
        actual[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT];
    MeisoK230dCompositionEdgeRuntimeInlineSnapshot
        inline_streams[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT];

    bool wifi_effective_present;
    MeisoWifiTelemetryEffectiveParameters wifi_effective;
    bool wifi_sample_present;
    MeisoWifiTelemetrySample wifi_sample;
    bool device_effective_present;
    MeisoDeviceTelemetryParameters device_effective;
    bool device_sample_present;
    MeisoDeviceTelemetrySample device_sample;
    bool render_media_actual_present;
    MeisoCompositionMediaActual render_media_actual;
    bool camera_media_actual_present;
    MeisoCompositionMediaActual camera_media_actual;
    bool presentation_status_present;
    MeisoCompositionScenePresentationStatus presentation_status;
    bool scene_status_present;
    MeisoCompositionSceneStatus scene_status;

    MeisoCompositionSceneControlScope scene_control_scope;
    bool active_world_present;
    MeisoCompositionSceneWorldRef active_world;
    MeisoCompositionCpuRasterSnapshot renderer;
    MeisoCameraV4l2Snapshot camera;
    MeisoK230dCompositionEdgeRuntimeMediaSnapshot media;

    uint32_t blob_published_count;
    uint64_t blob_published_bytes;
    uint64_t blob_transfer_id_high_water;
    /*
     * While result_pending this is the exact deep-copied pending RESULT
     * reference; otherwise it is the active receive reference. next_offset
     * belongs to the active receive and becomes zero after its terminal
     * RESULT completion. No Blob payload borrow escapes the pump call.
     */
    MeisoRuntimeBlobTransferRef blob_transfer;
    uint64_t blob_next_offset;
    MeisoRuntimeBlobOperation blob_result_operation;
    bool blob_transfer_active;
    bool blob_begin_ready;
    bool blob_result_pending;
    bool blob_result_queued;
    uint64_t next_thermal_request_id;
    bool thermal_request_pending;
    uint8_t media_slots_consumed;
    uint8_t media_slots_capacity;

    size_t arena_bytes;
    size_t control_session_bytes;
    size_t blob_store_bytes;
    size_t scene_store_bytes;
    size_t coordinator_bytes;
    size_t renderer_bytes;
    size_t camera_bytes;
    size_t core_state_bytes;
    size_t core_stream_bytes;
    size_t media_slot_bytes;
} MeisoK230dCompositionEdgeRuntimeSnapshot;

typedef struct {
    bool control_readable;
    bool control_writable;
    bool media_readable;
    bool media_writable;
    /* Caller grants one nonblocking DQBUF attempt; this is not fd readiness. */
    bool camera_poll_allowed;
    MeisoTimePoint now;
} MeisoK230dCompositionEdgeRuntimeIoReady;

typedef struct {
    bool control_read;
    bool control_write;
    bool media_read;
    bool media_write;
    bool camera_poll_useful;
    bool thermal_request_ready;
} MeisoK230dCompositionEdgeRuntimeIoInterest;

typedef uint8_t MeisoK230dCompositionEdgeRuntimePumpEvent;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_DESIRED_APPLIED = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_DESIRED_REJECTED = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_ACTUAL_DELIVERY = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BLOB_PUBLISHED = 5,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_SCENE_PROGRESS = 6,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_RENDER_COMPLETE = 7,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INLINE_SAMPLE = 8,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CAMERA_FRAME = 9,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_MEDIA_PROGRESS = 10,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_THERMAL_REQUEST = 11,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY = 12,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_EVENT_TERMINAL = 13
};

typedef uint8_t MeisoK230dCompositionEdgeRuntimePumpStatus;
enum {
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS = 0,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE = 1,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_WOULD_BLOCK = 2,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_TERMINAL = 3,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INVALID_ARGUMENT = 4,
    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE = 5
};

typedef struct {
    MeisoK230dCompositionEdgeRuntimePumpEvent event;
    bool event_present;
    bool desired_channel_present;
    MeisoK230dCompositionEdgeRuntimeDesiredChannel desired_channel;
    bool actual_channel_present;
    MeisoK230dCompositionEdgeRuntimeActualChannel actual_channel;
    bool inline_channel_present;
    MeisoK230dCompositionEdgeRuntimeInlineChannel inline_channel;
    bool media_source_present;
    MeisoCompositionMediaSource media_source;
    bool desired_reference_present;
    MeisoRuntimeDesiredReference desired_reference;
} MeisoK230dCompositionEdgeRuntimePumpResult;

typedef struct {
    uint8_t slot_index;
    MeisoK230dCompositionEdgeRuntimeMediaRouteMask active_route_mask;
    MeisoSessionId session_id;
    uint64_t catalog_generation;
} MeisoK230dCompositionEdgeRuntimeMediaBindReceipt;

size_t meiso_k230d_composition_edge_runtime_alignment_required(void);

/*
 * Returns the exact one-arena requirement for every owned object and all
 * configured one-use media slots. It performs no fd/filesystem/device I/O.
 * Zero means invalid config or size overflow. The fixed target capabilities
 * are exact RGBA8888 320x240 RAW render preview and progressive NV12 320x240
 * RAW camera; Wi-Fi/device fields are the init-time probe intersection.
 */
size_t meiso_k230d_composition_edge_runtime_memory_required(
    const MeisoK230dCompositionEdgeRuntimeConfig *config
);

/*
 * init copies target-private locators/syscall tables, initializes every owned
 * arena object, probes Wi-Fi/device capability, builds the exact 6-endpoint /
 * 18-route Composition Profile catalog and starts the Edge ControlSession. It performs no
 * camera STREAMON and creates/closes no control/media socket. The custom
 * camera syscall context remains a caller-owned borrow for runtime lifetime.
 *
 * config, every bounded config borrow, complete arena and out_runtime must be
 * mutually disjoint. Every detectable config/probe/fd/capacity preflight
 * failure writes no arena byte and leaves out_runtime exact. Once owned child
 * initialization starts, a defensive INTERNAL_FAILURE still leaves out_runtime
 * exact but intentionally invalidates the arena; that arena must not be
 * reused without reinitializing its bytes. All large storage is caller-arena
 * backed; no API allocates heap storage.
 */
MeisoK230dCompositionEdgeRuntimeInitStatus meiso_k230d_composition_edge_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoK230dCompositionEdgeRuntimeConfig *config,
    MeisoK230dCompositionEdgeRuntime **out_runtime
);

/*
 * Binds one caller-owned, already-connected nonblocking media fd. The caller
 * cannot select routes: the runtime derives the immutable 1/2/3 mask exclusively
 * from current successfully applied RUNNING RenderMedia/CameraMedia actual
 * truth. Every bit in that full intended mask must also have its current
 * actual delivery accepted, with no newer delivery pending; otherwise the
 * whole bind is NOT_READY and the fd/arena/output remain unchanged. It never
 * omits an unaccepted route or inspects a disabled/unapplied route to
 * manufacture a different BIND. Every success consumes a fresh preallocated
 * slot and returns that exact derived mask. The runtime never closes or changes
 * flags on the fd.
 *
 * Any selected actual enable/effective/binding/production epoch change,
 * control boundary or explicit unbind terminates the current media slot and
 * invalidates its packet borrow. Rebinding requires a new fd and unused slot.
 */
MeisoK230dCompositionEdgeRuntimeStatus meiso_k230d_composition_edge_runtime_bind_media_fd(
    MeisoK230dCompositionEdgeRuntime *runtime,
    int connected_fd,
    MeisoK230dCompositionEdgeRuntimeMediaBindReceipt *out_receipt
);
MeisoK230dCompositionEdgeRuntimeStatus meiso_k230d_composition_edge_runtime_unbind_media_fd(
    MeisoK230dCompositionEdgeRuntime *runtime,
    int error_number
);

/*
 * take copies and marks dispatched one pending target-local thermal request;
 * the runtime still retains only its small identity/deadline obligation. NO_WORK
 * leaves output unchanged. The standalone worker call reads at most one
 * thermal value through the copied adapter and never accesses/mutates runtime.
 * It is the only API permitted on a worker thread. The caller enforces the
 * request deadline and submits the typed result on the runtime thread.
 *
 * submit accepts exactly one matching, on-time result. Late/stale/mismatched
 * results are zero-change. At the sample deadline the runtime proceeds without a
 * temperature field rather than blocking control/media; sample() only sees a
 * fresh exact result and never performs the direct thermal read itself.
 */
MeisoK230dCompositionEdgeRuntimeStatus
meiso_k230d_composition_edge_runtime_take_thermal_request(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeThermalRequest *out_request
);
MeisoK230dCompositionEdgeRuntimeThermalReadStatus
meiso_k230d_composition_edge_runtime_run_thermal_request(
    const MeisoK230dCompositionEdgeRuntimeThermalRequest *request,
    MeisoK230dCompositionEdgeRuntimeThermalResult *out_result
);
MeisoK230dCompositionEdgeRuntimeStatus
meiso_k230d_composition_edge_runtime_submit_thermal_result(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeThermalResult *result,
    MeisoTimePoint completed_at
);

/*
 * One call performs at most one bounded control, desired-apply, actual/Stream
 * queue, Scene/C2/raster, camera DQ/QBUF, thermal scheduling or media action
 * under a persistent fairness cursor. Runtime State/Blob RX obligations are
 * resolved internally through the owned replicas/BlobStore/mesh verifier;
 * no wire borrow escapes. A published/duplicate Mesh is marked Scene-visible
 * only for the current desired owner/control scope before prepare may use it.
 * While any queued State/Stream work retains an runtime-owned payload borrow, the
 * runtime suppresses control reads and advances only that source/Scheduler TX path
 * until its completion or boundary. This serializes back-to-back desired
 * versions without mixing an old work identity with replacement payload bytes.
 * A stale older actual still receives its delivery ACK so the publisher can
 * expose the retained latest version; it is not adopted as current production.
 * No inline Stream sample or external media BIND is produced until the exact
 * current production actual has been delivery-accepted with no newer pending
 * actual, so Host Runtime never observes data for an unknown epoch.
 *
 * Actual State is produced only here from completed apply truth. There is no
 * public arbitrary-actual setter: decoded desired, adapter/C2/renderer result,
 * availability, effective parameters and production gate jointly determine
 * the exact actual. An actual RUNNING media source may be primed before media
 * bind, but camera/render bytes are not materialized until its derived route
 * is ACTIVE on the current media slot. Media transport/protocol loss fails
 * every route selected in that slot and queues new failure actuals while the
 * control/Wi-Fi/device paths survive; recovery requires fresh desired State
 * and a fresh media fd/slot, never implicit resume.
 *
 * RenderResult generation is capped by the active PresentationPlan
 * max_result_hz. RenderPreview submission and camera dequeue are independently
 * capped by their applied effective fps_millihz using checked fixed-point
 * deadlines. An early preview is skipped as a whole and never delays its
 * inline RenderResult. Cadence resets on a new production epoch, disable or
 * failure. Every newly applied enabled desired identity starts a fresh
 * production epoch even when its effective bytes are identical; same-epoch
 * continuity is only valid for the same applied desired identity. The camera
 * cadence controls output dequeue only; it does not reconfigure sensor
 * exposure, gain or ISP timing. A locally generated State accept/offer
 * rejected by Runtime is an invariant failure and terminates once rather than
 * being retried as a busy condition.
 *
 * After any queued source borrow completes, every old-world retirement drains
 * unsent old RenderResult samples and clears the Edge-owned job/complete view
 * copies at the same boundary as renderer invalidation. Presentation disable
 * also stops the inline producer and reports an otherwise enabled Scene as
 * nonproducing until a fresh plan activates it.
 *
 * ready.now must use config.edge_clock_epoch. WOULD_BLOCK/invalid calls leave
 * out_result unchanged; only an attempted syscall may advance fairness state.
 */
MeisoK230dCompositionEdgeRuntimePumpStatus meiso_k230d_composition_edge_runtime_pump(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
);

/* Pure level-triggered interests; false leaves out_interest unchanged. */
bool meiso_k230d_composition_edge_runtime_readiness(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeIoInterest *out_interest
);

/*
 * Pure value-copy snapshot. Desired observation, apply, actual delivery,
 * producer materialization, inline Stream acceptance and external media send
 * remain separate truth. No RUNNING actual implies a frame was sent.
 */
bool meiso_k230d_composition_edge_runtime_snapshot(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeSnapshot *out_snapshot
);

/*
 * Sticky terminal boundary. It makes at most two adapter stop attempts for the
 * runtime-owned camera, terminates media slots, invalidates all runtime borrows/work
 * and aborts ControlSession; control/media network fds remain caller-owned. If the
 * camera adapter still reports owned cleanup debt, terminal_reason is
 * CAMERA_CLEANUP and the camera snapshot retains that exact ledger instead of
 * claiming clean. Same-Session catalog change is intentionally terminal. A
 * reconnect requires a fresh socket, fresh runtime
 * arena, fresh ControlSession/Runtime/State/Stream/media/C2/renderer control scope,
 * and explicit desired/Blob visibility reconstruction: no resume, epoch/token
 * reuse or world resurrection is supported.
 */
MeisoK230dCompositionEdgeRuntimeStatus meiso_k230d_composition_edge_runtime_abort(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeTerminalReason reason,
    int error_number
);

#ifdef __cplusplus
}
#endif

#endif
