#ifndef MEISO_RUNTIME_PRIVATE_H
#define MEISO_RUNTIME_PRIVATE_H

#include <meiso/runtime/runtime.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Private Runtime representation and collaboration surface.
 *
 * This header is a build-only implementation detail. Its arena structures
 * preserve the field order and types required by Runtime operations. Public
 * callers see only the opaque MeisoRuntime handle declared by
 * <meiso/runtime/runtime.h>.
 */

#define MEISO_RUNTIME_MAGIC UINT64_C(0x4D4549534F52544D)

typedef uint8_t MeisoRuntimeWireType;
enum {
    MEISO_RUNTIME_WIRE_CATALOG = 1,
    MEISO_RUNTIME_WIRE_CATALOG_ACCEPT = 2,
    MEISO_RUNTIME_WIRE_CATALOG_REJECT = 3,
    MEISO_RUNTIME_WIRE_STATE_OFFER = 4,
    MEISO_RUNTIME_WIRE_STATE_ACCEPT = 5,
    MEISO_RUNTIME_WIRE_STREAM_SAMPLE = 6,
    MEISO_RUNTIME_WIRE_BLOB_BEGIN = 7,
    MEISO_RUNTIME_WIRE_BLOB_CHUNK = 8,
    MEISO_RUNTIME_WIRE_BLOB_COMMIT = 9,
    MEISO_RUNTIME_WIRE_BLOB_ABORT = 10,
    MEISO_RUNTIME_WIRE_BLOB_RESULT = 11
};

enum {
    MEISO_RUNTIME_CATALOG_PREFIX_SIZE = 16,
    MEISO_RUNTIME_ENDPOINT_RECORD_SIZE = 48,
    MEISO_RUNTIME_ROUTE_RECORD_SIZE = 72,
    MEISO_RUNTIME_ROUTE_V4_RECORD_SIZE =
        MEISO_RUNTIME_V4_ROUTE_RECORD_WIRE_SIZE,
    MEISO_RUNTIME_CATALOG_ACCEPT_SIZE = 8,
    MEISO_RUNTIME_CATALOG_REJECT_SIZE = 12,
    MEISO_RUNTIME_ROUTE_KEY_SIZE = 56,
    MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE = 96,
    MEISO_RUNTIME_STATE_ACCEPT_SIZE = 88,
    MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE = 144,
    MEISO_RUNTIME_CONTENT_ID_WIRE_SIZE = 36,
    MEISO_RUNTIME_BLOB_BEGIN_SIZE = 108,
    MEISO_RUNTIME_BLOB_CHUNK_PREFIX_SIZE = 72,
    MEISO_RUNTIME_BLOB_COMMIT_SIZE = 108,
    MEISO_RUNTIME_BLOB_ABORT_SIZE = 72,
    MEISO_RUNTIME_BLOB_RESULT_SIZE = 84
};

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_BLOB_BEGIN_SIZE ==
        MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE,
    "Blob BEGIN wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_BLOB_CHUNK_PREFIX_SIZE ==
        MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE,
    "Blob CHUNK prefix wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_BLOB_COMMIT_SIZE ==
        MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE,
    "Blob COMMIT wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_BLOB_ABORT_SIZE ==
        MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE,
    "Blob ABORT wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_BLOB_RESULT_SIZE ==
        MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE,
    "Blob RESULT wire size"
);

typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    MeisoSchemaRef capability_schema;
    uint32_t capability_offset;
    uint32_t capability_size;
    uint32_t actual_profile_size;
    MeisoRuntimeDesiredReference desired;
    MeisoRuntimeProductionSnapshot production;
} MeisoRuntimeEndpointSlot;

typedef struct {
    MeisoRuntimeRouteDescriptor value;
} MeisoRuntimeRouteSlot;

typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    uint64_t production_epoch_high_water;
    bool occupied;
    bool current;
} MeisoRuntimeIdentitySlot;

#if defined(_MSC_VER) && !defined(__clang__)
/* C11 flexible array member; keep all other /W4 /WX diagnostics. */
#pragma warning(push)
#pragma warning(disable: 4200)
#endif
struct MeisoRuntime {
    uint64_t magic;
    MeisoSession *session;
    MeisoSessionId session_id;
    MeisoSessionLimits limits;
    size_t arena_size;
    size_t endpoint_offset[2];
    size_t route_offset[2];
    size_t capability_offset[2];
    size_t actual_profile_offset[2];
    size_t identity_offset;
    size_t pending_desired_payload_offset;
    size_t pending_actual_profile_offset;
    size_t tx_offset;
    size_t blob_scratch_offset;
    uint64_t highest_catalog_generation;
    uint64_t pending_catalog_generation;
    uint64_t generation[2];
    uint32_t endpoint_count[2];
    uint32_t route_count[2];
    uint32_t capability_used[2];
    uint32_t max_endpoints;
    uint32_t max_endpoint_history;
    uint32_t max_routes;
    uint32_t max_capability_bytes;
    uint32_t max_desired_profile_bytes_per_endpoint;
    uint32_t max_actual_profile_bytes_per_endpoint;
    uint32_t max_blob_transfer_bytes;
    uint32_t identity_count;
    MeisoSessionRole local_role;
    uint8_t wire_version;
    uint8_t active_bank;
    MeisoRuntimeRouteKey pending_actual_route;
    MeisoDeliveryGeneration pending_actual_delivery;
    MeisoOwnerScope pending_actual_owner;
    uint64_t pending_actual_version;
    uint32_t pending_desired_payload_size;
    uint32_t pending_actual_profile_size;
    MeisoRuntimeProductionSnapshot pending_actual_value;
    MeisoRuntimeBlobTransferRef blob_transfer;
    MeisoRuntimeBlobTransferRef blob_result_transfer;
    uint64_t blob_next_offset;
    uint64_t blob_result_next_offset;
    uint64_t blob_transfer_id_high_water;
    MeisoRuntimeBlobOperation blob_pending_operation;
    MeisoRuntimeBlobResult blob_pending_result;
    bool active_present;
    bool staged_present;
    bool catalog_decision_pending;
    bool pending_desired_receive;
    bool pending_actual_receive;
    bool blob_transfer_active;
    bool blob_begin_ready;
    bool blob_publish_pending;
    bool blob_result_pending;
    bool blob_outbound_terminal_wait;
    bool blob_bridge_active;
    bool invalidated;
    uint8_t storage[];
};
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

typedef struct {
    size_t endpoint_offset[2];
    size_t route_offset[2];
    size_t capability_offset[2];
    size_t actual_profile_offset[2];
    size_t identity_offset;
    size_t pending_desired_payload_offset;
    size_t pending_actual_profile_offset;
    size_t tx_offset;
    size_t blob_scratch_offset;
    size_t delivery_offset[2];
    size_t required;
} MeisoRuntimeArenaLayout;

/* Bounded arithmetic, overlap and canonical wire primitives. */
bool meiso_runtime_private_blob_wire_supported(const MeisoRuntime *runtime);
bool meiso_runtime_private_checked_add(
    size_t lhs, size_t rhs, size_t *out_value
);
bool meiso_runtime_private_checked_multiply(
    size_t lhs, size_t rhs, size_t *out_value
);
bool meiso_runtime_private_align_up(
    size_t value, size_t alignment, size_t *out_value
);
bool meiso_runtime_private_add_aligned_array(
    size_t *cursor,
    size_t alignment,
    size_t count,
    size_t item_size,
    size_t *out_offset
);
bool meiso_runtime_private_ranges_overlap(
    const void *lhs, size_t lhs_size, const void *rhs, size_t rhs_size
);
void meiso_runtime_private_put_u16(uint8_t *target, uint16_t value);
void meiso_runtime_private_put_u32(uint8_t *target, uint32_t value);
void meiso_runtime_private_put_u64(uint8_t *target, uint64_t value);
uint16_t meiso_runtime_private_get_u16(const uint8_t *source);
uint32_t meiso_runtime_private_get_u32(const uint8_t *source);
uint64_t meiso_runtime_private_get_u64(const uint8_t *source);
void meiso_runtime_private_put_endpoint_id(
    uint8_t *target, MeisoEndpointId value
);
MeisoEndpointId meiso_runtime_private_get_endpoint_id(const uint8_t *source);
void meiso_runtime_private_put_resource_id(
    uint8_t *target, MeisoResourceId value
);
MeisoResourceId meiso_runtime_private_get_resource_id(const uint8_t *source);
void meiso_runtime_private_put_schema(uint8_t *target, MeisoSchemaRef value);
MeisoSchemaRef meiso_runtime_private_get_schema(const uint8_t *source);
bool meiso_runtime_private_bytes_zero(const uint8_t *bytes, size_t size);
bool meiso_runtime_private_schema_valid(MeisoSchemaRef value);
bool meiso_runtime_private_reject_reason_valid(
    MeisoRuntimeCatalogRejectReason value
);
void meiso_runtime_private_encode_header(
    const MeisoRuntime *runtime,
    uint8_t *target,
    MeisoRuntimeWireType type,
    uint32_t size
);
bool meiso_runtime_private_decode_header(
    const MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeWireType *out_type
);
void meiso_runtime_private_encode_route_key(
    uint8_t *output, MeisoRuntimeRouteKey key
);
bool meiso_runtime_private_decode_route_key(
    const uint8_t *input, MeisoRuntimeRouteKey *out_key
);
void meiso_runtime_private_canonicalize_content_id(
    const MeisoContentId *source, MeisoContentId *target
);
void meiso_runtime_private_encode_content_id(
    uint8_t *output, const MeisoContentId *value
);
bool meiso_runtime_private_decode_content_id(
    const uint8_t *input, MeisoContentId *out_value
);

/* State payload semantics shared by State, Stream and query modules. */
bool meiso_runtime_private_desired_reference_equal(
    MeisoRuntimeDesiredReference lhs, MeisoRuntimeDesiredReference rhs
);
bool meiso_runtime_private_actual_state_for_route_valid(
    const MeisoRuntimeActualState *state
);
bool meiso_runtime_private_actual_data_active(
    const MeisoRuntimeActualState *state
);

/* Arena access and lifecycle boundaries. */
MeisoRuntimeEndpointSlot *meiso_runtime_private_endpoint_slots(
    MeisoRuntime *runtime, uint8_t bank
);
const MeisoRuntimeEndpointSlot *meiso_runtime_private_endpoint_slots_const(
    const MeisoRuntime *runtime, uint8_t bank
);
MeisoRuntimeRouteSlot *meiso_runtime_private_route_slots(
    MeisoRuntime *runtime, uint8_t bank
);
const MeisoRuntimeRouteSlot *meiso_runtime_private_route_slots_const(
    const MeisoRuntime *runtime, uint8_t bank
);
uint8_t *meiso_runtime_private_delivery_classes(
    MeisoRuntime *runtime, uint8_t bank
);
const uint8_t *meiso_runtime_private_delivery_classes_const(
    const MeisoRuntime *runtime, uint8_t bank
);
uint8_t *meiso_runtime_private_capability_bytes(
    MeisoRuntime *runtime, uint8_t bank
);
const uint8_t *meiso_runtime_private_capability_bytes_const(
    const MeisoRuntime *runtime, uint8_t bank
);
uint8_t *meiso_runtime_private_actual_profile_bytes(
    MeisoRuntime *runtime, uint8_t bank, size_t endpoint_index
);
const uint8_t *meiso_runtime_private_actual_profile_bytes_const(
    const MeisoRuntime *runtime, uint8_t bank, size_t endpoint_index
);
uint8_t *meiso_runtime_private_pending_actual_profile_bytes(
    MeisoRuntime *runtime
);
MeisoRuntimeIdentitySlot *meiso_runtime_private_identity_slots(
    MeisoRuntime *runtime
);
const MeisoRuntimeIdentitySlot *meiso_runtime_private_identity_slots_const(
    const MeisoRuntime *runtime
);
uint8_t *meiso_runtime_private_pending_desired_payload_bytes(
    MeisoRuntime *runtime
);
uint8_t *meiso_runtime_private_tx_bytes(MeisoRuntime *runtime);
uint8_t *meiso_runtime_private_blob_scratch(MeisoRuntime *runtime);
void meiso_runtime_private_clear_blob_transfer(MeisoRuntime *runtime);
uint8_t meiso_runtime_private_staging_bank(const MeisoRuntime *runtime);
bool meiso_runtime_private_layout_valid(const MeisoRuntime *runtime);
void meiso_runtime_private_clear_catalog_bank(
    MeisoRuntime *runtime, uint8_t bank
);
void meiso_runtime_private_clear_catalog_decision(MeisoRuntime *runtime);
void meiso_runtime_private_clear_pending_state_receive(
    MeisoRuntime *runtime
);
MeisoRuntimeStatus meiso_runtime_private_check_bound_mutable_internal(
    MeisoRuntime *runtime, bool preserve_blob_publish
);
MeisoRuntimeStatus meiso_runtime_private_check_bound_mutable(
    MeisoRuntime *runtime
);
MeisoRuntimeStatus meiso_runtime_private_check_bound_const(
    const MeisoRuntime *runtime
);
bool meiso_runtime_private_output_overlaps_runtime_or_session(
    const MeisoRuntime *runtime, const void *output, size_t output_size
);
MeisoRuntimeStatus meiso_runtime_private_submit_bytes(
    MeisoRuntime *runtime, size_t size
);
MeisoRuntimeStatus meiso_runtime_private_blob_tx_preflight(
    const MeisoRuntime *runtime
);

/* Catalog and active-route services. */
int meiso_runtime_private_endpoint_index_in_bank(
    const MeisoRuntime *runtime, uint8_t bank, MeisoEndpointId id
);
int meiso_runtime_private_identity_pair_index(
    const MeisoRuntime *runtime, MeisoEndpointId id, uint64_t incarnation
);
int meiso_runtime_private_route_index_in_bank(
    const MeisoRuntime *runtime, uint8_t bank, MeisoResourceId id
);
MeisoRuntimeStatus meiso_runtime_private_decode_catalog_to_staging(
    MeisoRuntime *runtime, MeisoByteView frame, uint64_t *out_generation
);
void meiso_runtime_private_commit_staged(MeisoRuntime *runtime);
bool meiso_runtime_private_route_key_equal(
    MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs
);
MeisoRuntimeStatus meiso_runtime_private_resolve_active_route(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    MeisoRuntimeRouteSlot **out_route,
    MeisoRuntimeEndpointSlot **out_endpoint
);
MeisoRuntimeStatus meiso_runtime_private_resolve_active_route_const(
    const MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    const MeisoRuntimeRouteSlot **out_route,
    const MeisoRuntimeEndpointSlot **out_endpoint,
    size_t *out_route_index
);
MeisoRuntimeDeliveryClass meiso_runtime_private_route_delivery_at(
    const MeisoRuntime *runtime, uint8_t bank, size_t index
);
bool meiso_runtime_private_blob_transfer_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
);
bool meiso_runtime_private_blob_operation_valid(
    MeisoRuntimeBlobOperation value
);
bool meiso_runtime_private_blob_abort_reason_valid(
    MeisoRuntimeBlobAbortReason value
);
void meiso_runtime_private_set_blob_result(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobOperation operation,
    MeisoRuntimeBlobResult result,
    uint64_t next_offset
);
MeisoOwnerRole meiso_runtime_private_owner_role_for_session(
    MeisoSessionRole role
);

/* Domain receive handlers called only by the common receive boundary. */
MeisoRuntimeStatus meiso_runtime_private_receive_catalog(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_catalog_accept(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_catalog_reject(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_state_offer(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_state_accept(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_stream_sample(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_blob_begin(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_blob_chunk(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_blob_commit(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_blob_abort(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);
MeisoRuntimeStatus meiso_runtime_private_receive_blob_result(
    MeisoRuntime *runtime, MeisoByteView frame, MeisoRuntimeEvent *event
);

#endif
