#ifndef MEISO_RUNTIME_RUNTIME_H
#define MEISO_RUNTIME_RUNTIME_H

#include <meiso/core/session.h>
#include <meiso/core/blob.h>
#include <meiso/core/state.h>
#include <meiso/core/stream.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_RUNTIME_WIRE_VERSION_V2 2U
#define MEISO_RUNTIME_WIRE_VERSION_V3 3U
#define MEISO_RUNTIME_WIRE_VERSION_V4 4U
#define MEISO_RUNTIME_WIRE_HEADER_SIZE 12U
#define MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE 8U
#define MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE 56U
#define MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE 4096U
#define MEISO_RUNTIME_V3_CONTENT_ID_WIRE_SIZE 36U
#define MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE 120U
#define MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE 84U
#define MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE 120U
#define MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE 84U
#define MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE 96U
#define MEISO_RUNTIME_V4_ROUTE_RECORD_WIRE_SIZE 80U

/*
 * MRT0 v3 Blob bodies begin after the common 12-byte header. All integers are
 * big-endian and every reserved byte is zero:
 *   BEGIN/COMMIT: route[0..55], transfer_id[56..63], ContentId[64..99],
 *                 total_size[100..107].
 *   CHUNK:        route[0..55], transfer_id[56..63], offset[64..71],
 *                 bytes[72..end] (the frame remainder is the byte count).
 *   ABORT:        route[0..55], transfer_id[56..63], reason[64],
 *                 reserved[65..71].
 *   RESULT:       route[0..55], transfer_id[56..63], operation[64],
 *                 result[65], reserved[66..67], next_offset[68..75],
 *                 total_size[76..83].
 * ContentId wire bytes are algorithm:u16, digest_size:u8, reserved:u8, then
 * the 32-byte SHA-256 digest. Noncanonical ContentIds are rejected.
 */

typedef struct MeisoRuntime MeisoRuntime;

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoEndpointId;

/* capability is opaque to Runtime and is copied into the Runtime arena. */
typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    MeisoSchemaRef capability_schema;
    MeisoByteView capability;
} MeisoRuntimeEndpointDescriptor;

typedef uint8_t MeisoRuntimeFacet;
enum {
    MEISO_RUNTIME_FACET_CAPABILITY = 1,
    MEISO_RUNTIME_FACET_DESIRED = 2,
    MEISO_RUNTIME_FACET_ACTUAL = 3,
    MEISO_RUNTIME_FACET_DATA = 4,
    MEISO_RUNTIME_FACET_OPERATION = 5,
    MEISO_RUNTIME_FACET_IMMUTABLE = 6
};

typedef struct {
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceDescriptor resource;
    MeisoRuntimeFacet facet;
} MeisoRuntimeRouteDescriptor;

typedef uint8_t MeisoRuntimeDeliveryClass;
enum {
    MEISO_RUNTIME_DELIVERY_INLINE = 1,
    MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA = 2
};

/* MRT0 v4 preserves the v2/v3 descriptor and adds catalog delivery truth. */
typedef struct {
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeDeliveryClass delivery;
} MeisoRuntimeV4RouteDescriptor;

/* Complete identity of a route in the currently active catalog. */
typedef struct {
    uint64_t catalog_generation;
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceId resource_id;
    MeisoRuntimeFacet facet;
} MeisoRuntimeRouteKey;

typedef struct {
    MeisoSession *session;
    uint32_t max_endpoints;
    /* Retained (EndpointId, incarnation) history; must cover max_endpoints. */
    uint32_t max_endpoint_history;
    uint32_t max_routes;
    uint32_t max_capability_bytes;
    uint32_t max_desired_profile_bytes_per_endpoint;
    uint32_t max_actual_profile_bytes_per_endpoint;
} MeisoRuntimeConfig;

/*
 * v3 extends the v2 Runtime arena with one bounded Host-to-Edge Blob
 * transfer. max_blob_transfer_bytes is an admission limit on both peers and
 * reserves reassembly bytes only in an Edge Runtime arena. memory_required
 * therefore depends on the role of the READY bound Session.
 */
typedef struct {
    MeisoRuntimeConfig base;
    uint32_t max_blob_transfer_bytes;
} MeisoRuntimeV3Config;

/*
 * v4 retains v3 Blob carriage. Delivery sidecars are reserved only by the
 * v4 initializer, so v2/v3 memory_required results and arena layout remain
 * byte-exact.
 */
typedef struct {
    MeisoRuntimeConfig base;
    uint32_t max_blob_transfer_bytes;
} MeisoRuntimeV4Config;

typedef uint8_t MeisoRuntimeInitStatus;
enum {
    MEISO_RUNTIME_INIT_OK = 0,
    MEISO_RUNTIME_INIT_INVALID_ARGUMENT = 1,
    MEISO_RUNTIME_INIT_CORE_NOT_READY = 2,
    MEISO_RUNTIME_INIT_INVALID_CONFIG = 3,
    MEISO_RUNTIME_INIT_MISALIGNED_ARENA = 4,
    MEISO_RUNTIME_INIT_ARENA_TOO_SMALL = 5
};

typedef uint8_t MeisoRuntimeStatus;
enum {
    MEISO_RUNTIME_OK = 0,
    MEISO_RUNTIME_INVALID_ARGUMENT = 1,
    MEISO_RUNTIME_CORE_NOT_READY = 2,
    MEISO_RUNTIME_STALE_SESSION = 3,
    MEISO_RUNTIME_WRONG_ROLE = 4,
    MEISO_RUNTIME_TX_BUSY = 5,
    MEISO_RUNTIME_CAPACITY = 6,
    MEISO_RUNTIME_CATALOG_PENDING = 7,
    MEISO_RUNTIME_NO_CATALOG = 8,
    MEISO_RUNTIME_STALE_CATALOG = 9,
    MEISO_RUNTIME_MALFORMED = 10,
    MEISO_RUNTIME_CONFLICT = 11,
    MEISO_RUNTIME_UNKNOWN_ENDPOINT = 12,
    MEISO_RUNTIME_STALE_ENDPOINT = 13,
    MEISO_RUNTIME_UNKNOWN_RESOURCE = 14,
    MEISO_RUNTIME_SCHEMA_MISMATCH = 15,
    MEISO_RUNTIME_UNSUPPORTED_KIND = 16,
    MEISO_RUNTIME_OWNER_MISMATCH = 17,
    MEISO_RUNTIME_FACET_MISMATCH = 18,
    MEISO_RUNTIME_ROUTE_NOT_ACTIVE = 19,
    MEISO_RUNTIME_PAYLOAD_CAPACITY = 20,
    MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH = 21,
    MEISO_RUNTIME_STATE_INVALID = 22,
    MEISO_RUNTIME_STREAM_INVALID = 23,
    MEISO_RUNTIME_WIRE_VERSION_MISMATCH = 24,
    MEISO_RUNTIME_BLOB_BUSY = 25,
    MEISO_RUNTIME_BLOB_NOT_ACTIVE = 26,
    MEISO_RUNTIME_BLOB_INVALID = 27,
    MEISO_RUNTIME_DELIVERY_MISMATCH = 28
};

typedef uint16_t MeisoRuntimeProductionReason;
enum {
    MEISO_RUNTIME_PRODUCTION_REASON_NONE = 0,
    MEISO_RUNTIME_PRODUCTION_REASON_DISABLED = 1,
    MEISO_RUNTIME_PRODUCTION_REASON_APPLYING = 2,
    MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER = 3,
    MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED = 4,
    MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE = 5,
    MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED = 6
};

typedef uint8_t MeisoRuntimeProductionStatus;
enum {
    MEISO_RUNTIME_PRODUCTION_STOPPED = 1,
    MEISO_RUNTIME_PRODUCTION_STARTING = 2,
    MEISO_RUNTIME_PRODUCTION_RUNNING = 3,
    MEISO_RUNTIME_PRODUCTION_DEGRADED = 4,
    MEISO_RUNTIME_PRODUCTION_STOPPING = 5,
    MEISO_RUNTIME_PRODUCTION_FAILED = 6
};

typedef uint8_t MeisoRuntimeApplyResult;
enum {
    MEISO_RUNTIME_APPLY_NONE = 1,
    MEISO_RUNTIME_APPLY_PENDING = 2,
    MEISO_RUNTIME_APPLY_APPLIED = 3,
    MEISO_RUNTIME_APPLY_REJECTED = 4,
    MEISO_RUNTIME_APPLY_FAILED = 5
};

typedef uint8_t MeisoRuntimeAvailability;
enum {
    MEISO_RUNTIME_AVAILABILITY_PRESENT = 1,
    MEISO_RUNTIME_AVAILABILITY_ABSENT = 2,
    MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE = 3
};

/* Canonical payload of a DESIRED-facet Host-owned State. */
typedef struct {
    bool enabled;
    MeisoByteView profile;
} MeisoRuntimeDesiredState;

typedef struct {
    bool present;
    MeisoOwnerScope owner;
    uint64_t version;
} MeisoRuntimeDesiredReference;

/* Canonical payload of an ACTUAL-facet Edge-owned State. */
typedef struct {
    MeisoRuntimeDesiredReference observed_desired;
    MeisoRuntimeDesiredReference applied_desired;
    MeisoRuntimeApplyResult apply_result;
    MeisoRuntimeAvailability availability;
    bool effective_enabled;
    MeisoRuntimeProductionStatus production_status;
    MeisoRuntimeProductionReason reason;
    uint64_t production_epoch;
    MeisoByteView profile;
} MeisoRuntimeActualState;

typedef struct {
    bool actual_present;
    MeisoRuntimeDesiredReference observed_desired;
    MeisoRuntimeDesiredReference applied_desired;
    MeisoRuntimeApplyResult apply_result;
    MeisoRuntimeAvailability availability;
    bool effective_enabled;
    MeisoRuntimeProductionStatus production_status;
    MeisoRuntimeProductionReason reason;
    uint64_t production_epoch;
    uint64_t production_epoch_high_water;
    MeisoByteView effective_profile;
    bool data_active;
} MeisoRuntimeProductionSnapshot;

typedef uint8_t MeisoRuntimeCatalogRejectReason;
enum {
    MEISO_RUNTIME_CATALOG_REJECT_UNSUPPORTED = 1,
    MEISO_RUNTIME_CATALOG_REJECT_POLICY = 2,
    MEISO_RUNTIME_CATALOG_REJECT_CAPACITY = 3
};

typedef uint8_t MeisoRuntimeBlobOperation;
enum {
    MEISO_RUNTIME_BLOB_BEGIN = 1,
    MEISO_RUNTIME_BLOB_CHUNK = 2,
    MEISO_RUNTIME_BLOB_COMMIT = 3,
    MEISO_RUNTIME_BLOB_ABORT = 4
};

/*
 * READY is the sole non-terminal result and is valid only for BEGIN.
 * PUBLISHED, DUPLICATE, and ABORTED are successful terminal results; all
 * remaining values are terminal rejections. A terminal result retires the
 * exact Session-scoped transfer on the Host when accepted. Legal wire pairs
 * are BEGIN+READY; BEGIN+terminal rejection; CHUNK+DISCONTINUITY or
 * RECEIVER_FAILURE; COMMIT+PUBLISHED, DUPLICATE, or terminal rejection; and
 * ABORT+ABORTED. READY has next_offset=0. PUBLISHED and DUPLICATE have
 * next_offset=total_size. ABORTED and every rejection report the bytes
 * accepted before that operation (zero for a rejected BEGIN). Every RESULT
 * echoes the transfer's declared total_size. An impossible pair, nonzero
 * reserved byte, wrong total/next value, or non-exact current Host route/id is
 * MALFORMED or CONFLICT and never retires a different/current transfer.
 */
typedef uint8_t MeisoRuntimeBlobResult;
enum {
    MEISO_RUNTIME_BLOB_RESULT_READY = 1,
    MEISO_RUNTIME_BLOB_RESULT_PUBLISHED = 2,
    MEISO_RUNTIME_BLOB_RESULT_DUPLICATE = 3,
    MEISO_RUNTIME_BLOB_RESULT_ABORTED = 4,
    MEISO_RUNTIME_BLOB_RESULT_BUSY = 5,
    MEISO_RUNTIME_BLOB_RESULT_CONFLICT = 6,
    MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID = 7,
    MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH = 8,
    MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED = 9,
    MEISO_RUNTIME_BLOB_RESULT_CAPACITY = 10,
    MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY = 11,
    MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE = 12
};

typedef uint8_t MeisoRuntimeBlobAbortReason;
enum {
    MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST = 1
};

/* transfer_id is nonzero, Runtime-generated, and never reused per Session. */
typedef struct {
    MeisoRuntimeRouteKey route;
    uint64_t transfer_id;
    MeisoContentId content_id;
    uint64_t total_size;
} MeisoRuntimeBlobTransferRef;

typedef struct {
    MeisoBlobStageStatus stage_status;
    MeisoBlobCommitStatus commit_status;
    MeisoBlobAbortStatus forced_abort_status;
    MeisoRuntimeBlobResult result;
    bool stage_attempted;
    bool commit_attempted;
    bool forced_abort_attempted;
} MeisoRuntimeBlobPublishReport;

typedef uint8_t MeisoRuntimeEventType;
enum {
    MEISO_RUNTIME_EVENT_CATALOG_OFFERED = 1,
    MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED = 2,
    MEISO_RUNTIME_EVENT_CATALOG_REJECTED = 3,
    MEISO_RUNTIME_EVENT_STATE_OFFER = 4,
    MEISO_RUNTIME_EVENT_STATE_ACCEPT = 5,
    MEISO_RUNTIME_EVENT_STREAM_SAMPLE = 6,
    MEISO_RUNTIME_EVENT_BLOB_REQUEST = 7,
    MEISO_RUNTIME_EVENT_BLOB_RESULT = 8
};

typedef struct {
    MeisoRuntimeEventType type;
    uint64_t catalog_generation;
    MeisoRuntimeCatalogRejectReason reject_reason;
    MeisoRuntimeRouteKey route;
    MeisoStateOffer state_offer;
    MeisoDeliveryGeneration state_accept_generation;
    MeisoOwnerScope state_accept_owner;
    uint64_t state_accept_version;
    uint64_t production_epoch;
    MeisoStreamSample stream_sample;
    MeisoRuntimeBlobOperation blob_operation;
    MeisoRuntimeBlobTransferRef blob_transfer;
    uint64_t blob_offset;
    MeisoByteView blob_payload;
    MeisoRuntimeBlobResult blob_result;
    uint64_t blob_next_offset;
} MeisoRuntimeEvent;

typedef struct {
    /*
     * When result_pending, transfer/next_offset identify that pending RESULT
     * (which may reject a second BEGIN while transfer_active still describes
     * the occupied slot). Otherwise they identify the active transfer.
     */
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeBlobOperation pending_operation;
    MeisoRuntimeBlobResult pending_result;
    uint64_t next_offset;
    uint64_t transfer_id_high_water;
    uint32_t max_transfer_bytes;
    uint8_t wire_version;
    bool transfer_active;
    bool begin_ready;
    bool publish_pending;
    bool result_pending;
    bool core_ready;
} MeisoRuntimeBlobSnapshot;

typedef struct {
    MeisoSessionId session_id;
    MeisoSessionRole local_role;
    MeisoSessionLimits negotiated_limits;
    uint64_t highest_catalog_generation;
    uint64_t pending_catalog_generation;
    uint64_t active_catalog_generation;
    uint64_t staged_catalog_generation;
    uint32_t active_endpoint_count;
    uint32_t active_route_count;
    uint32_t staged_endpoint_count;
    uint32_t staged_route_count;
    bool active_catalog_present;
    bool staged_catalog_present;
    bool catalog_decision_pending;
    bool core_ready;
} MeisoRuntimeSnapshot;

/*
 * Immutable copy for an SDK media adapter. It is current only until the next
 * non-const Runtime call; the exact Session/catalog/endpoint/production tuple
 * must be re-queried after any mutation. media_stream_epoch and channel_nonce
 * remain Profile actual-State truth and are intentionally not invented here.
 */
typedef struct {
    MeisoSessionId session_id;
    MeisoRuntimeRouteKey route;
    MeisoResourceDescriptor resource;
    uint64_t production_epoch;
} MeisoRuntimeV4ExternalMediaBinding;

/*
 * One owner must serialize every call on a Runtime and its bound Session.
 * Different Runtime/Session pairs are independent; this API provides no lock.
 * The Runtime arena, complete opaque Session arena, and every output object
 * must not overlap; the caller owns this storage proof.
 */

bool meiso_endpoint_id_valid(MeisoEndpointId value);
bool meiso_endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs);
bool meiso_runtime_facet_valid(MeisoRuntimeFacet value);

/* Canonical State payload helpers; decoded profile bytes borrow input. */
size_t meiso_runtime_desired_state_encoded_size(
    const MeisoRuntimeDesiredState *state
);
bool meiso_runtime_desired_state_encode(
    const MeisoRuntimeDesiredState *state,
    MeisoMutableBuffer output,
    MeisoByteView *out_payload
);
bool meiso_runtime_desired_state_decode(
    MeisoByteView payload,
    MeisoRuntimeDesiredState *out_state
);
size_t meiso_runtime_actual_state_encoded_size(
    const MeisoRuntimeActualState *state
);
bool meiso_runtime_actual_state_encode(
    const MeisoRuntimeActualState *state,
    MeisoMutableBuffer output,
    MeisoByteView *out_payload
);
bool meiso_runtime_actual_state_decode(
    MeisoByteView payload,
    MeisoRuntimeActualState *out_state
);

size_t meiso_runtime_alignment_required(void);
size_t meiso_runtime_memory_required(const MeisoRuntimeConfig *config);
size_t meiso_runtime_v3_memory_required(const MeisoRuntimeV3Config *config);
size_t meiso_runtime_v4_memory_required(const MeisoRuntimeV4Config *config);

/* A handle is permanently bound to this READY SessionId and role. */
MeisoRuntimeInitStatus meiso_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeConfig *config,
    MeisoRuntime **out_runtime
);

/*
 * Creates a strict MRT0 v3 handle. It rejects MRT0 v2 frames; a v2 handle
 * likewise rejects v3. The Session/Profile layer is responsible for choosing
 * v3 symmetrically (the composition profile selects it beginning at its v2
 * ProfileRef). A Runtime remains permanently bound to the exact READY
 * SessionId and role observed here.
 */
MeisoRuntimeInitStatus meiso_runtime_v3_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeV3Config *config,
    MeisoRuntime **out_runtime
);

/*
 * Creates a strict MRT0 v4 handle. v2/v3 frames are rejected. The accepted
 * catalog uses 80-byte route records: the old 72 bytes are unchanged,
 * delivery is byte 72, and bytes 73..79 are zero. INLINE routes retain every
 * existing Runtime payload limit. EXTERNAL_MEDIA is legal only for a nonzero
 * maximum-unit Stream/DATA route. Its Resource owner selects the producer;
 * both Host-owned and Edge-owned data are legal. External delivery never
 * authorizes inline STREAM_SAMPLE carriage.
 */
MeisoRuntimeInitStatus meiso_runtime_v4_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeV4Config *config,
    MeisoRuntime **out_runtime
);

/*
 * Explicitly reconciles this handle with its permanently bound Session at an
 * SDK/control boundary. This is a single-owner, non-reentrant mutation and
 * has no output or borrowed storage.
 *
 * A READY Session with the exact bound SessionId, local role, and negotiated
 * limits returns OK and preserves the complete Runtime byte-for-byte,
 * including pending State, catalog, Blob transfer, and Blob publication
 * state. If the Session is unavailable, unknown, or no longer READY, the
 * first call atomically invalidates the Runtime, clears its catalogs and all
 * pending State/Blob state (including the Edge reassembly scratch), preserves
 * the Blob transfer-id high-water, and returns CORE_NOT_READY; later
 * calls return STALE_SESSION. A live identity/role/limits mismatch invalidates
 * identically but returns STALE_SESSION on the first and every later call.
 * Immutable binding/configuration and monotonic high-water bookkeeping may
 * remain in the opaque arena; cleared catalog/State/Blob semantic state is no
 * longer observable as active or reusable.
 * During a Blob publication verifier callback it returns CONFLICT with no
 * mutation.
 */
MeisoRuntimeStatus meiso_runtime_refresh_bound_session(
    MeisoRuntime *runtime
);

/*
 * Edge only. The complete catalog is one bounded, atomic wire frame.
 * A generation successfully queued to Core, including one later rejected,
 * cannot be reused. TX_BUSY does not consume it.
 */
MeisoRuntimeStatus meiso_runtime_offer_catalog(
    MeisoRuntime *runtime,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeRouteDescriptor *routes,
    size_t route_count
);

/*
 * Edge-only MRT0 v4 catalog offer; failure is atomic as in the base API.
 * Catalog replacement and bound-Session invalidation clear both delivery
 * banks together with their route banks; no external binding survives.
 */
MeisoRuntimeStatus meiso_runtime_v4_offer_catalog(
    MeisoRuntime *runtime,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeV4RouteDescriptor *routes,
    size_t route_count
);

/* Host only. A successful decision is queued before local catalog mutation. */
MeisoRuntimeStatus meiso_runtime_accept_catalog(MeisoRuntime *runtime);
MeisoRuntimeStatus meiso_runtime_reject_catalog(
    MeisoRuntime *runtime,
    MeisoRuntimeCatalogRejectReason reason
);

/*
 * State and Stream metadata are the Resource Core types, not parallel Runtime
 * semantics. The route key must name the exact active catalog route. Input
 * byte views may be current Session RX borrows: Runtime finishes every read
 * and bounded copy before submitting to Session invalidates such a borrow.
 */
MeisoRuntimeStatus meiso_runtime_send_state_offer(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer
);
MeisoRuntimeStatus meiso_runtime_send_state_accept(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
);
MeisoRuntimeStatus meiso_runtime_send_stream_sample(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route,
    uint64_t production_epoch,
    const MeisoStreamSample *sample
);
/*
 * On v4, the active DATA route's Resource owner must match the local Session
 * role and sample producer role; the opposite peer is the only legal
 * receiver. The v2/v3 compatibility handles remain Edge-producer only.
 * On a v4 EXTERNAL_MEDIA route this returns DELIVERY_MISMATCH and leaves the
 * complete Runtime arena, bound Session arena, sample, and every caller output
 * byte unchanged; the SDK media adapter uses external_media_binding instead.
 */

/*
 * MRT0 v3 Host-only Blob carriage. The exact active route must be a
 * Host-owned BLOB/IMMUTABLE route. BEGIN is stop-and-wait: no CHUNK is legal
 * until the Edge READY result arrives. CHUNK frames are then sent in exact
 * offset order without per-chunk acknowledgement; a short chunk is legal
 * only when it ends at total_size. ABORT is also legal while waiting for
 * READY; a crossing late READY is observational and cannot reopen transfer.
 * COMMIT and ABORT await one terminal result.
 *
 * A successful BEGIN queue generates and consumes a strictly increasing
 * Session-scoped transfer_id and writes the complete reference to out_transfer.
 * Caller-provided transfer IDs are impossible. On TX_BUSY, out_transfer is
 * untouched and these calls leave transfer identity, high-water, offset, and
 * pending result state unchanged.
 * All Blob send/publish/result calls return CONFLICT without consuming the
 * pending State receive when its StateReplica outcome has not been committed.
 */
MeisoRuntimeStatus meiso_runtime_send_blob_begin(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const MeisoContentId *content_id,
    uint64_t total_size,
    MeisoRuntimeBlobTransferRef *out_transfer
);
MeisoRuntimeStatus meiso_runtime_send_blob_chunk(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t offset,
    MeisoByteView payload
);
MeisoRuntimeStatus meiso_runtime_send_blob_commit(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer
);
MeisoRuntimeStatus meiso_runtime_send_blob_abort(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobAbortReason reason
);

/*
 * message must be the exact borrow returned by the bound Session. State and
 * Stream byte views in the event borrow that message and expire before the
 * next non-const call on either handle. For BLOB_REQUEST, BEGIN and ABORT have
 * an empty blob_payload; CHUNK borrows the current Session message; COMMIT
 * borrows the complete reassembly scratch in the Runtime arena. Both Blob
 * borrows expire before the next non-const call on Runtime or Session.
 * After a COMMIT event, publish_blob_receive must be the immediate next
 * Runtime mutation only when blob_snapshot reports publish_pending and no
 * result_pending. A failed length/SHA precheck instead reports result_pending
 * and requires send_blob_result. BLOB_RESULT always has an empty blob_payload.
 */
MeisoRuntimeStatus meiso_runtime_receive(
    MeisoRuntime *runtime,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
);

/*
 * Edge only, immediately after the exact COMMIT BLOB_REQUEST event. Runtime
 * has already reassembled the complete bytes and checked their SHA-256
 * ContentId. This synchronous bridge performs BlobStore stage, bounded copy,
 * and commit_verified with a combined SHA-256 and caller schema/Profile
 * verifier over the same Core verification snapshot. It never retains store,
 * verifier, or context. The caller must prove that store was initialized from
 * and is exclusively bound to the exact active ResourceDescriptor named by
 * expected_transfer.route; Runtime cannot inspect an opaque BlobStore to
 * distinguish a different capacity-compatible route. Every Core failure that
 * can retain a
 * staging candidate successfully STARTED by this call is followed by
 * TRANSFER_DISCONTINUITY abort before a terminal result becomes pending.
 * UNCHANGED, BUSY, and every other pre-existing staging candidate are never
 * aborted or overwritten. Runtime, BlobStore, and output arenas must be
 * caller-proven disjoint. A verifier attempt to re-enter this Runtime is
 * rejected (RuntimeStatus APIs return CONFLICT; query APIs fail) without
 * changing the pending publication.
 * Publication does not grant SDK/Profile visibility.
 */
MeisoRuntimeStatus meiso_runtime_publish_blob_receive(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
);

/*
 * Edge only, in place of publish_blob_receive after the exact COMMIT
 * BLOB_REQUEST event: the receiver has no room for this content now (an Edge
 * short of memory, Display Ingress planes-v12 §7). Nothing is staged or
 * published; the pending COMMIT result becomes CAPACITY with every byte
 * accepted, sent by send_blob_result as any other. Same preconditions as
 * publish_blob_receive: BLOB_NOT_ACTIVE without a pending publication,
 * CONFLICT for another transfer or a result already pending.
 */
MeisoRuntimeStatus meiso_runtime_refuse_blob_receive(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer
);

/*
 * Edge only. Queues the exact pending READY or terminal RESULT. READY echoes
 * next_offset=0 and the declared total. PUBLISHED/DUPLICATE echo
 * next_offset=total; ABORTED and failures echo the number of accepted bytes.
 * TX_BUSY is retryable and byte-for-byte state preserving.
 */
MeisoRuntimeStatus meiso_runtime_send_blob_result(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoRuntimeBlobOperation expected_operation
);

/* Generic SHA-256 identity verifier; schema/Profile verifiers may be stricter. */
bool meiso_runtime_blob_sha256_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
);

/*
 * Immediately after applying a DESIRED/ACTUAL STATE_OFFER to its StateReplica,
 * report the Core result for that receive event. Only RECEIVE_NEW binds the
 * desired reference or changes the production gate; rejected State never can.
 */
MeisoRuntimeStatus meiso_runtime_commit_state_receive(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
);

bool meiso_runtime_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeSnapshot *out_snapshot
);
bool meiso_runtime_blob_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeBlobSnapshot *out_snapshot
);

/*
 * Endpoint capability byte views borrow the Runtime arena until its next
 * non-const call. Route descriptors are scalar copies and contain no arena
 * borrow. The base route_at/staged_route_at/find_route APIs intentionally
 * fail with WIRE_VERSION_MISMATCH on a v4 handle because their result cannot
 * represent delivery class; v4 callers must use the corresponding v4 API.
 */
MeisoRuntimeStatus meiso_runtime_endpoint_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
);
MeisoRuntimeStatus meiso_runtime_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeRouteDescriptor *out_route
);
MeisoRuntimeStatus meiso_runtime_staged_endpoint_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
);
MeisoRuntimeStatus meiso_runtime_staged_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeRouteDescriptor *out_route
);
MeisoRuntimeStatus meiso_runtime_v4_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeV4RouteDescriptor *out_route
);
MeisoRuntimeStatus meiso_runtime_v4_staged_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeV4RouteDescriptor *out_route
);
MeisoRuntimeStatus meiso_runtime_find_endpoint(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeEndpointDescriptor *out_endpoint
);
MeisoRuntimeStatus meiso_runtime_find_route(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeRouteDescriptor *out_route
);
MeisoRuntimeStatus meiso_runtime_v4_find_route(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeV4RouteDescriptor *out_route
);

/*
 * Returns only an active EXTERNAL_MEDIA Stream/DATA route whose endpoint
 * production gate is active. The returned Resource owner identifies whether
 * Host or Edge is the producer. INLINE routes return
 * DELIVERY_MISMATCH; inactive/stale routes fail without partial output.
 * Catalog replacement or Session terminal makes the old query fail.
 */
MeisoRuntimeStatus meiso_runtime_v4_external_media_binding(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeV4ExternalMediaBinding *out_binding
);
MeisoRuntimeStatus meiso_runtime_production_snapshot(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeProductionSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
