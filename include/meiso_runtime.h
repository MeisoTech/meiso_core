#ifndef MEISO_RUNTIME_H
#define MEISO_RUNTIME_H

#include "meiso_session.h"
#include "meiso_state.h"
#include "meiso_stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_RUNTIME_WIRE_VERSION 2U
#define MEISO_RUNTIME_WIRE_HEADER_SIZE 12U
#define MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE 8U
#define MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE 56U

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
    MEISO_RUNTIME_STREAM_INVALID = 23
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

typedef uint8_t MeisoRuntimeEventType;
enum {
    MEISO_RUNTIME_EVENT_CATALOG_OFFERED = 1,
    MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED = 2,
    MEISO_RUNTIME_EVENT_CATALOG_REJECTED = 3,
    MEISO_RUNTIME_EVENT_STATE_OFFER = 4,
    MEISO_RUNTIME_EVENT_STATE_ACCEPT = 5,
    MEISO_RUNTIME_EVENT_STREAM_SAMPLE = 6
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
} MeisoRuntimeEvent;

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

/* A handle is permanently bound to this READY SessionId and role. */
MeisoRuntimeInitStatus meiso_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeConfig *config,
    MeisoRuntime **out_runtime
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
 * message must be the exact borrow returned by the bound Session. State and
 * Stream byte views in the event borrow that message and expire before the
 * next non-const call on either handle.
 */
MeisoRuntimeStatus meiso_runtime_receive(
    MeisoRuntime *runtime,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
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

/* Catalog views borrow the Runtime arena until its next non-const call. */
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
MeisoRuntimeStatus meiso_runtime_production_snapshot(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeProductionSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
