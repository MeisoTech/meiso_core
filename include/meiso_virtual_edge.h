#ifndef MEISO_VIRTUAL_EDGE_H
#define MEISO_VIRTUAL_EDGE_H

#include "meiso_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_VIRTUAL_EDGE_API_VERSION 1U

/*
 * Application-facing projection of one continuous accepted Edge Session and
 * catalog.  The integration layer creates and drives the opaque handle; an
 * application never owns a socket, Session, Runtime, pump or platform handle.
 */
typedef struct MeisoEdgeHandle MeisoEdgeHandle;

typedef uint8_t MeisoEdgeStatus;
enum {
    MEISO_EDGE_OK = 0,
    MEISO_EDGE_INVALID_ARGUMENT = 1,
    MEISO_EDGE_NOT_READY = 2,
    MEISO_EDGE_UNSUPPORTED = 3,
    MEISO_EDGE_BUSY = 4,
    MEISO_EDGE_CAPACITY = 5,
    MEISO_EDGE_CONFLICT = 6,
    MEISO_EDGE_NO_DATA = 7,
    MEISO_EDGE_STALE = 8,
    MEISO_EDGE_COUNTER_EXHAUSTED = 9,
    MEISO_EDGE_TERMINAL = 10,
    MEISO_EDGE_INTERNAL_FAILURE = 11
};

typedef uint8_t MeisoEdgePhase;
enum {
    MEISO_EDGE_PHASE_BOOTSTRAPPING = 1,
    MEISO_EDGE_PHASE_READY = 2,
    MEISO_EDGE_PHASE_TERMINAL = 3
};

typedef uint8_t MeisoEdgeTerminalReason;
enum {
    MEISO_EDGE_TERMINAL_NONE = 0,
    MEISO_EDGE_TERMINAL_TRANSPORT = 1,
    MEISO_EDGE_TERMINAL_SESSION = 2,
    MEISO_EDGE_TERMINAL_CATALOG_DISCONTINUITY = 3,
    MEISO_EDGE_TERMINAL_LOCAL_ABORT = 4,
    MEISO_EDGE_TERMINAL_INTERNAL_FAILURE = 5
};

typedef uint32_t MeisoEdgeOperationMask;
enum {
    MEISO_EDGE_OPERATION_STATE_READ = UINT32_C(1) << 0U,
    MEISO_EDGE_OPERATION_STATE_SUBMIT = UINT32_C(1) << 1U,
    MEISO_EDGE_OPERATION_STATE_WATCH = UINT32_C(1) << 2U,
    MEISO_EDGE_OPERATION_STREAM_READ = UINT32_C(1) << 3U,
    MEISO_EDGE_OPERATION_STREAM_PUBLISH = UINT32_C(1) << 4U,
    MEISO_EDGE_OPERATION_ACTION_INVOKE = UINT32_C(1) << 5U,
    MEISO_EDGE_OPERATION_BLOB_LOOKUP = UINT32_C(1) << 6U,
    MEISO_EDGE_OPERATION_BLOB_PUT = UINT32_C(1) << 7U,
    MEISO_EDGE_OPERATION_COMPOSITION = UINT32_C(1) << 8U,
    MEISO_EDGE_OPERATION_MEDIA_READ = UINT32_C(1) << 9U,
    MEISO_EDGE_OPERATION_ALL = UINT32_C(0x000003ff)
};

typedef uint8_t MeisoEdgeResourceAccess;
enum {
    MEISO_EDGE_RESOURCE_HOST_SUBMIT = 1,
    MEISO_EDGE_RESOURCE_EDGE_READ = 2
};

typedef uint8_t MeisoEdgePayloadPath;
enum {
    MEISO_EDGE_PAYLOAD_INLINE = 1,
    MEISO_EDGE_PAYLOAD_EXTERNAL_MEDIA = 2
};

typedef struct {
    uint32_t api_version;
    MeisoProfileRef profile;
    MeisoEdgePhase phase;
    MeisoEdgeTerminalReason terminal_reason;
    /* Core Session is READY; this alone does not make the graph live. */
    bool online;
    /* Accepted catalog/resources and all typed values are current. */
    bool resource_graph_live;
    /* Local to this handle lifetime; never compare across handles. */
    uint64_t continuity_serial;
    uint64_t catalog_generation;
    MeisoEdgeOperationMask operations;
    uint16_t endpoint_count;
    uint16_t resource_count;
} MeisoEdgeSnapshot;

/* Capability bytes borrow immutable SDK storage while this graph stays live. */
typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    MeisoSchemaRef capability_schema;
    MeisoByteView capability;
} MeisoEdgeEndpointView;

/* Value-copy resource identity; no wire route key or transport handle leaks. */
typedef struct {
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceDescriptor resource;
    MeisoEdgeResourceAccess access;
    MeisoEdgePayloadPath payload_path;
} MeisoEdgeResourceView;

/*
 * Snapshot remains readable at a terminal boundary, but then publishes zero
 * live capability/resource counts. Discovery is available only while the
 * resource graph is live. Every failure leaves the output byte-exact.
 */
MeisoEdgeStatus meiso_edge_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoEdgeSnapshot *out_snapshot
);
MeisoEdgeStatus meiso_edge_operation_supported(
    const MeisoEdgeHandle *handle,
    MeisoEdgeOperationMask operation,
    bool *out_supported
);
MeisoEdgeStatus meiso_edge_endpoint_at(
    const MeisoEdgeHandle *handle,
    size_t index,
    MeisoEdgeEndpointView *out_endpoint
);
MeisoEdgeStatus meiso_edge_resource_at(
    const MeisoEdgeHandle *handle,
    size_t index,
    MeisoEdgeResourceView *out_resource
);

#ifdef __cplusplus
}
#endif

#endif
