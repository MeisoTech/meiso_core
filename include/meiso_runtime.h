#ifndef MEISO_RUNTIME_H
#define MEISO_RUNTIME_H

#include "meiso_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_RUNTIME_WIRE_VERSION 1U
#define MEISO_RUNTIME_WIRE_HEADER_SIZE 12U

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

typedef uint8_t MeisoRuntimeDelivery;
enum {
    /* Route is available after its catalog generation becomes active. */
    MEISO_RUNTIME_DELIVERY_ANNOUNCED = 1,
    /* Host desired + Edge actual ACTIVE are required before delivery. */
    MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION = 2
};

typedef struct {
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceDescriptor resource;
    MeisoSchemaRef selection_schema;
    uint32_t max_selection_bytes;
    MeisoRuntimeDelivery delivery;
} MeisoRuntimeRouteDescriptor;

typedef struct {
    MeisoSession *session;
    uint32_t max_endpoints;
    uint32_t max_routes;
    uint32_t max_capability_bytes;
    uint32_t max_parameter_bytes_per_route;
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
    MEISO_RUNTIME_NO_DESIRED = 18,
    MEISO_RUNTIME_REQUEST_MISMATCH = 19,
    MEISO_RUNTIME_ROUTE_NOT_ACTIVE = 20,
    MEISO_RUNTIME_PAYLOAD_CAPACITY = 21
};

typedef uint8_t MeisoRuntimeActualStatus;
enum {
    MEISO_RUNTIME_ACTUAL_NONE = 0,
    MEISO_RUNTIME_ACTUAL_APPLYING = 1,
    MEISO_RUNTIME_ACTUAL_ACTIVE = 2,
    MEISO_RUNTIME_ACTUAL_REJECTED = 3,
    MEISO_RUNTIME_ACTUAL_UNAVAILABLE = 4,
    MEISO_RUNTIME_ACTUAL_DISABLED = 5
};

typedef uint8_t MeisoRuntimeActualReason;
enum {
    MEISO_RUNTIME_ACTUAL_REASON_NONE = 0,
    MEISO_RUNTIME_ACTUAL_REASON_UNSUPPORTED_PARAMETER = 1,
    MEISO_RUNTIME_ACTUAL_REASON_ADMISSION_DENIED = 2,
    MEISO_RUNTIME_ACTUAL_REASON_UNAVAILABLE = 3,
    MEISO_RUNTIME_ACTUAL_REASON_APPLY_FAILED = 4
};

typedef uint8_t MeisoRuntimeCatalogRejectReason;
enum {
    MEISO_RUNTIME_CATALOG_REJECT_UNSUPPORTED = 1,
    MEISO_RUNTIME_CATALOG_REJECT_POLICY = 2,
    MEISO_RUNTIME_CATALOG_REJECT_CAPACITY = 3
};

typedef struct {
    uint64_t request_id;
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceId resource_id;
    bool enabled;
    MeisoByteView parameters;
} MeisoRuntimeSubscriptionDesired;

typedef struct {
    uint64_t request_id;
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceId resource_id;
    MeisoRuntimeActualStatus status;
    MeisoRuntimeActualReason reason;
    uint64_t route_epoch;
    MeisoByteView effective_parameters;
} MeisoRuntimeSubscriptionActual;

typedef struct {
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceId resource_id;
    MeisoResourceKind kind;
    MeisoSchemaRef schema;
    uint64_t request_id;
    uint64_t route_epoch;
    MeisoByteView payload;
} MeisoRuntimeResourceMessage;

typedef uint8_t MeisoRuntimeEventType;
enum {
    MEISO_RUNTIME_EVENT_CATALOG_OFFERED = 1,
    MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED = 2,
    MEISO_RUNTIME_EVENT_CATALOG_REJECTED = 3,
    MEISO_RUNTIME_EVENT_SUBSCRIPTION_DESIRED = 4,
    MEISO_RUNTIME_EVENT_SUBSCRIPTION_ACTUAL = 5,
    MEISO_RUNTIME_EVENT_RESOURCE = 6
};

typedef struct {
    MeisoRuntimeEventType type;
    uint64_t catalog_generation;
    MeisoRuntimeCatalogRejectReason reject_reason;
    MeisoRuntimeSubscriptionDesired desired;
    MeisoRuntimeSubscriptionActual actual;
    MeisoRuntimeResourceMessage resource;
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

typedef struct {
    bool desired_present;
    bool desired_enabled;
    uint64_t request_id;
    MeisoByteView desired_parameters;
    MeisoRuntimeActualStatus actual_status;
    MeisoRuntimeActualReason actual_reason;
    uint64_t route_epoch;
    MeisoByteView effective_parameters;
    bool route_active;
} MeisoRuntimeSubscriptionSnapshot;

/*
 * One owner must serialize every call on a Runtime and its bound Session.
 * Different Runtime/Session pairs are independent; this API provides no lock.
 * The Runtime arena, complete opaque Session arena, and every output object
 * must not overlap; the caller owns this storage proof.
 */

bool meiso_endpoint_id_valid(MeisoEndpointId value);
bool meiso_endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs);

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

/*
 * Host only. Acceptance requires a complete staged catalog and atomically
 * replaces active state. Rejection is also valid for a reject-only pending
 * generation retained after a local capacity/unsupported result.
 */
MeisoRuntimeStatus meiso_runtime_accept_catalog(MeisoRuntime *runtime);
MeisoRuntimeStatus meiso_runtime_reject_catalog(
    MeisoRuntime *runtime,
    MeisoRuntimeCatalogRejectReason reason
);

/*
 * Host only; v0 subscription routes must be Edge-owned Streams. request_id
 * must increase strictly for each request on one active route.
 */
MeisoRuntimeStatus meiso_runtime_request_subscription(
    MeisoRuntime *runtime,
    const MeisoRuntimeSubscriptionDesired *desired
);

/* Edge only; route_epoch is nonzero only for ACTIVE and zero otherwise. */
MeisoRuntimeStatus meiso_runtime_report_subscription(
    MeisoRuntime *runtime,
    const MeisoRuntimeSubscriptionActual *actual
);

/*
 * The resource descriptor supplies the authoritative kind/schema/max payload.
 * ANNOUNCED routes become active with their accepted catalog. A
 * HOST_SUBSCRIPTION route additionally requires the current enabled desired
 * request to have actual ACTIVE.
 */
MeisoRuntimeStatus meiso_runtime_send_resource(
    MeisoRuntime *runtime,
    const MeisoRuntimeResourceMessage *message
);

/*
 * message must be the exact borrow returned by the bound Session. Resource
 * event payload borrows that message and expires before the next non-const
 * call on either handle. Desired/actual event bytes borrow the Runtime arena.
 */
MeisoRuntimeStatus meiso_runtime_receive(
    MeisoRuntime *runtime,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
);

bool meiso_runtime_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeSnapshot *out_snapshot
);

/* capability borrows the Runtime arena until its next non-const call. */
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

/* Parameter views borrow the Runtime arena until its next non-const call. */
MeisoRuntimeStatus meiso_runtime_subscription_snapshot(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeSubscriptionSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
