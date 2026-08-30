#include "meiso_runtime_private.h"

#include <string.h>

/* Bounded Blob transfer protocol, reassembly and result state machine. */

static MeisoRuntimeStatus resolve_blob_route(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    MeisoRuntimeRouteSlot **out_route
)
{
    MeisoRuntimeEndpointSlot *endpoint;
    MeisoRuntimeStatus status;

    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    status = meiso_runtime_private_resolve_active_route(runtime, key, out_route, &endpoint);
    (void)endpoint;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if ((*out_route)->value.facet != MEISO_RUNTIME_FACET_IMMUTABLE) {
        return MEISO_RUNTIME_FACET_MISMATCH;
    }
    if ((*out_route)->value.resource.kind != MEISO_RESOURCE_KIND_BLOB) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if ((*out_route)->value.resource.owner != MEISO_OWNER_ROLE_HOST) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_send_blob_begin(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    const MeisoContentId *content_id,
    uint64_t total_size,
    MeisoRuntimeBlobTransferRef *out_transfer
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeRouteSlot *route;
    uint8_t *body;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (out_transfer == NULL || !meiso_content_id_valid(content_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_transfer, sizeof(*out_transfer)
        ) || meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size, content_id, sizeof(*content_id)
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (runtime->blob_transfer_active || runtime->blob_result_pending ||
        runtime->blob_publish_pending) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    status = resolve_blob_route(runtime, route_key, &route);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (total_size > runtime->max_blob_transfer_bytes ||
        total_size > route->value.resource.max_payload_size) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    if (runtime->blob_transfer_id_high_water == UINT64_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }
    status = meiso_runtime_private_blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    memset(&transfer, 0, sizeof(transfer));
    transfer.route = route_key;
    transfer.transfer_id = runtime->blob_transfer_id_high_water + 1U;
    meiso_runtime_private_canonicalize_content_id(content_id, &transfer.content_id);
    transfer.total_size = total_size;
    body = meiso_runtime_private_tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    meiso_runtime_private_encode_route_key(body, transfer.route);
    meiso_runtime_private_put_u64(body + 56U, transfer.transfer_id);
    meiso_runtime_private_encode_content_id(body + 64U, &transfer.content_id);
    meiso_runtime_private_put_u64(body + 100U, transfer.total_size);
    meiso_runtime_private_encode_header(
        runtime,
        meiso_runtime_private_tx_bytes(runtime),
        MEISO_RUNTIME_WIRE_BLOB_BEGIN,
        MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE
    );
    status = meiso_runtime_private_submit_bytes(
        runtime, MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE
    );
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_transfer = transfer;
        runtime->blob_transfer_id_high_water = transfer.transfer_id;
        runtime->blob_transfer_active = true;
        runtime->blob_begin_ready = false;
        runtime->blob_next_offset = 0U;
        *out_transfer = transfer;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_blob_chunk(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t offset,
    MeisoByteView payload
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    uint8_t *body;
    uint64_t end;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (transfer == NULL || !meiso_byte_view_valid(payload) ||
        payload.size == 0U ||
        payload.size > MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ||
        meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size, transfer, sizeof(*transfer)
        ) || meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size, payload.data, payload.size
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!meiso_runtime_private_blob_transfer_equal(transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!runtime->blob_begin_ready ||
        runtime->blob_outbound_terminal_wait) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (offset != runtime->blob_next_offset ||
        payload.size > UINT64_MAX - offset) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    end = offset + payload.size;
    if (end > transfer->total_size ||
        (payload.size < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE &&
         end != transfer->total_size)) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    status = meiso_runtime_private_blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    size = MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE + payload.size;
    body = meiso_runtime_private_tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    meiso_runtime_private_encode_route_key(body, transfer->route);
    meiso_runtime_private_put_u64(body + 56U, transfer->transfer_id);
    meiso_runtime_private_put_u64(body + 64U, offset);
    memmove(body + MEISO_RUNTIME_BLOB_CHUNK_PREFIX_SIZE, payload.data, payload.size);
    meiso_runtime_private_encode_header(runtime, meiso_runtime_private_tx_bytes(runtime), MEISO_RUNTIME_WIRE_BLOB_CHUNK, (uint32_t)size);
    status = meiso_runtime_private_submit_bytes(runtime, size);
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_next_offset = end;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_blob_commit(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    uint8_t *body;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (transfer == NULL || meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size, transfer, sizeof(*transfer)
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!meiso_runtime_private_blob_transfer_equal(transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!runtime->blob_begin_ready ||
        runtime->blob_outbound_terminal_wait) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (runtime->blob_next_offset != transfer->total_size) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    status = meiso_runtime_private_blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    body = meiso_runtime_private_tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    meiso_runtime_private_encode_route_key(body, transfer->route);
    meiso_runtime_private_put_u64(body + 56U, transfer->transfer_id);
    meiso_runtime_private_encode_content_id(body + 64U, &transfer->content_id);
    meiso_runtime_private_put_u64(body + 100U, transfer->total_size);
    meiso_runtime_private_encode_header(
        runtime,
        meiso_runtime_private_tx_bytes(runtime),
        MEISO_RUNTIME_WIRE_BLOB_COMMIT,
        MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE
    );
    status = meiso_runtime_private_submit_bytes(
        runtime, MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE
    );
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_outbound_terminal_wait = true;
        runtime->blob_pending_operation = MEISO_RUNTIME_BLOB_COMMIT;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_blob_abort(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobAbortReason reason
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    uint8_t *body;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (transfer == NULL || !meiso_runtime_private_blob_abort_reason_valid(reason) ||
        meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size, transfer, sizeof(*transfer)
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!meiso_runtime_private_blob_transfer_equal(transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_outbound_terminal_wait) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    status = meiso_runtime_private_blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    body = meiso_runtime_private_tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    meiso_runtime_private_encode_route_key(body, transfer->route);
    meiso_runtime_private_put_u64(body + 56U, transfer->transfer_id);
    body[64U] = reason;
    memset(body + 65U, 0, 7U);
    meiso_runtime_private_encode_header(
        runtime,
        meiso_runtime_private_tx_bytes(runtime),
        MEISO_RUNTIME_WIRE_BLOB_ABORT,
        MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE
    );
    status = meiso_runtime_private_submit_bytes(runtime, MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE);
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_outbound_terminal_wait = true;
        runtime->blob_pending_operation = MEISO_RUNTIME_BLOB_ABORT;
    }
    return status;
}
void meiso_runtime_private_set_blob_result(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobOperation operation,
    MeisoRuntimeBlobResult result,
    uint64_t next_offset
)
{
    runtime->blob_result_transfer = *transfer;
    runtime->blob_pending_operation = operation;
    runtime->blob_pending_result = result;
    runtime->blob_result_next_offset = next_offset;
    runtime->blob_result_pending = true;
}

static void fill_blob_request_event(
    MeisoRuntimeEvent *event,
    MeisoRuntimeBlobOperation operation,
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t offset,
    MeisoByteView payload
)
{
    event->type = MEISO_RUNTIME_EVENT_BLOB_REQUEST;
    event->catalog_generation = transfer->route.catalog_generation;
    event->route = transfer->route;
    event->blob_operation = operation;
    event->blob_transfer = *transfer;
    event->blob_offset = offset;
    event->blob_payload = payload;
}

static bool blob_result_is_terminal(MeisoRuntimeBlobResult result)
{
    return result >= MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
        result <= MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
}

static bool blob_result_reference_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_runtime_private_route_key_equal(lhs->route, rhs->route) &&
        lhs->transfer_id == rhs->transfer_id &&
        lhs->total_size == rhs->total_size;
}

static bool blob_result_combination_valid(
    MeisoRuntimeBlobOperation operation,
    MeisoRuntimeBlobResult result
)
{
    if (!meiso_runtime_private_blob_operation_valid(operation)) {
        return false;
    }
    if (operation == MEISO_RUNTIME_BLOB_BEGIN) {
        return result == MEISO_RUNTIME_BLOB_RESULT_READY ||
            (result >= MEISO_RUNTIME_BLOB_RESULT_BUSY &&
             result <= MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE);
    }
    if (operation == MEISO_RUNTIME_BLOB_CHUNK) {
        return result == MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY ||
            result == MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
    }
    if (operation == MEISO_RUNTIME_BLOB_COMMIT) {
        return result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
            result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE ||
            (result >= MEISO_RUNTIME_BLOB_RESULT_BUSY &&
             result <= MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE);
    }
    return result == MEISO_RUNTIME_BLOB_RESULT_ABORTED;
}

MeisoRuntimeStatus meiso_runtime_private_receive_blob_begin(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeStatus status;
    bool content_valid;

    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &transfer.route) || body[67U] != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(&transfer.content_id, 0, sizeof(transfer.content_id));
    transfer.transfer_id = meiso_runtime_private_get_u64(body + 56U);
    content_valid = meiso_runtime_private_decode_content_id(body + 64U, &transfer.content_id);
    transfer.total_size = meiso_runtime_private_get_u64(body + 100U);
    if (transfer.transfer_id == 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    status = resolve_blob_route(runtime, transfer.route, &route);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->blob_result_pending || runtime->blob_publish_pending) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (runtime->blob_transfer_active &&
        transfer.transfer_id == runtime->blob_transfer.transfer_id) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (transfer.transfer_id <= runtime->blob_transfer_id_high_water) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_CONFLICT,
            0U
        );
        fill_blob_request_event(
            event,
            MEISO_RUNTIME_BLOB_BEGIN,
            &transfer,
            0U,
            (MeisoByteView){NULL, 0U}
        );
        return MEISO_RUNTIME_OK;
    }
    runtime->blob_transfer_id_high_water = transfer.transfer_id;
    if (!content_valid) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID,
            0U
        );
    } else if (runtime->blob_transfer_active) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_BUSY,
            0U
        );
    } else if (transfer.total_size > runtime->max_blob_transfer_bytes ||
               transfer.total_size > route->value.resource.max_payload_size ||
               (sizeof(size_t) < sizeof(uint64_t) &&
                transfer.total_size > (uint64_t)SIZE_MAX)) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_CAPACITY,
            0U
        );
    } else {
        if (runtime->max_blob_transfer_bytes != 0U) {
            memset(
                meiso_runtime_private_blob_scratch(runtime),
                0,
                runtime->max_blob_transfer_bytes
            );
        }
        runtime->blob_transfer = transfer;
        runtime->blob_next_offset = 0U;
        runtime->blob_transfer_active = true;
        runtime->blob_begin_ready = false;
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_READY,
            0U
        );
    }
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_BEGIN,
        &transfer,
        0U,
        (MeisoByteView){NULL, 0U}
    );
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_receive_blob_chunk(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey route;
    MeisoByteView payload;
    uint64_t transfer_id;
    uint64_t offset;
    uint64_t end;
    MeisoRuntimeRouteSlot *route_slot;
    MeisoRuntimeStatus status;

    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size <= MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE ||
        frame.size > MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +
                MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &route)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer_id = meiso_runtime_private_get_u64(body + 56U);
    offset = meiso_runtime_private_get_u64(body + 64U);
    payload.data = body + MEISO_RUNTIME_BLOB_CHUNK_PREFIX_SIZE;
    payload.size = frame.size - MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE;
    status = resolve_blob_route(runtime, route, &route_slot);
    (void)route_slot;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!meiso_runtime_private_route_key_equal(route, runtime->blob_transfer.route) ||
        transfer_id != runtime->blob_transfer.transfer_id) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_result_pending || runtime->blob_publish_pending ||
        !runtime->blob_begin_ready) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (offset != runtime->blob_next_offset ||
        payload.size > UINT64_MAX - offset) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &runtime->blob_transfer,
            MEISO_RUNTIME_BLOB_CHUNK,
            MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY,
            runtime->blob_next_offset
        );
    } else {
        end = offset + payload.size;
        if (end > runtime->blob_transfer.total_size ||
            (payload.size < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE &&
             end != runtime->blob_transfer.total_size)) {
            meiso_runtime_private_set_blob_result(
                runtime,
                &runtime->blob_transfer,
                MEISO_RUNTIME_BLOB_CHUNK,
                MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY,
                runtime->blob_next_offset
            );
        } else {
            memmove(meiso_runtime_private_blob_scratch(runtime) + (size_t)offset,
                    payload.data, payload.size);
            runtime->blob_next_offset = end;
        }
    }
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_CHUNK,
        &runtime->blob_transfer,
        offset,
        payload
    );
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_receive_blob_commit(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeStatus status;
    MeisoByteView content;

    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &transfer.route) ||
        !meiso_runtime_private_decode_content_id(body + 64U, &transfer.content_id)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer.transfer_id = meiso_runtime_private_get_u64(body + 56U);
    transfer.total_size = meiso_runtime_private_get_u64(body + 100U);
    status = resolve_blob_route(runtime, transfer.route, &route);
    (void)route;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!meiso_runtime_private_blob_transfer_equal(&transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_result_pending || runtime->blob_publish_pending ||
        !runtime->blob_begin_ready) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    content.data = meiso_runtime_private_blob_scratch(runtime);
    content.size = (size_t)transfer.total_size;
    if (runtime->blob_next_offset != transfer.total_size) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_COMMIT,
            MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH,
            runtime->blob_next_offset
        );
    } else if (!meiso_runtime_blob_sha256_verify(
                   &transfer.content_id, content, NULL
               )) {
        meiso_runtime_private_set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_COMMIT,
            MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED,
            runtime->blob_next_offset
        );
    } else {
        runtime->blob_publish_pending = true;
        runtime->blob_pending_operation = MEISO_RUNTIME_BLOB_COMMIT;
    }
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_COMMIT,
        &transfer,
        0U,
        content
    );
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_receive_blob_abort(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey route;
    uint64_t transfer_id;
    MeisoRuntimeRouteSlot *route_slot;
    MeisoRuntimeStatus status;

    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &route) ||
        !meiso_runtime_private_blob_abort_reason_valid(body[64U]) ||
        !meiso_runtime_private_bytes_zero(body + 65U, 7U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer_id = meiso_runtime_private_get_u64(body + 56U);
    status = resolve_blob_route(runtime, route, &route_slot);
    (void)route_slot;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!meiso_runtime_private_route_key_equal(route, runtime->blob_transfer.route) ||
        transfer_id != runtime->blob_transfer.transfer_id) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_publish_pending ||
        (runtime->blob_result_pending &&
         runtime->blob_pending_result != MEISO_RUNTIME_BLOB_RESULT_READY)) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    memset(meiso_runtime_private_blob_scratch(runtime), 0, runtime->max_blob_transfer_bytes);
    meiso_runtime_private_set_blob_result(
        runtime,
        &runtime->blob_transfer,
        MEISO_RUNTIME_BLOB_ABORT,
        MEISO_RUNTIME_BLOB_RESULT_ABORTED,
        runtime->blob_next_offset
    );
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_ABORT,
        &runtime->blob_transfer,
        runtime->blob_next_offset,
        (MeisoByteView){NULL, 0U}
    );
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_receive_blob_result(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeBlobOperation operation;
    MeisoRuntimeBlobResult result;
    uint64_t transfer_id;
    uint64_t next_offset;
    uint64_t total_size;
    MeisoRuntimeBlobTransferRef transfer;
    bool terminal;
    bool canonical_prefix;

    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &route) || !meiso_runtime_private_bytes_zero(body + 66U, 2U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer_id = meiso_runtime_private_get_u64(body + 56U);
    operation = body[64U];
    result = body[65U];
    next_offset = meiso_runtime_private_get_u64(body + 68U);
    total_size = meiso_runtime_private_get_u64(body + 76U);
    if (!blob_result_combination_valid(operation, result)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    transfer = runtime->blob_transfer;
    if (!meiso_runtime_private_route_key_equal(route, transfer.route) ||
        transfer_id != transfer.transfer_id ||
        total_size != transfer.total_size || next_offset > total_size) {
        return MEISO_RUNTIME_CONFLICT;
    }
    terminal = blob_result_is_terminal(result);
    canonical_prefix = next_offset <= runtime->blob_next_offset &&
        (next_offset == total_size ||
         next_offset % MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE == 0U);
    if (operation == MEISO_RUNTIME_BLOB_BEGIN) {
        if (next_offset != 0U) {
            return MEISO_RUNTIME_MALFORMED;
        }
        if (result == MEISO_RUNTIME_BLOB_RESULT_READY) {
            if (runtime->blob_begin_ready) {
                return MEISO_RUNTIME_CONFLICT;
            }
        } else if (runtime->blob_begin_ready) {
            return MEISO_RUNTIME_CONFLICT;
        }
    } else if (operation == MEISO_RUNTIME_BLOB_CHUNK) {
        if (!runtime->blob_begin_ready || runtime->blob_next_offset == 0U ||
            !canonical_prefix) {
            return MEISO_RUNTIME_CONFLICT;
        }
    } else if (operation == MEISO_RUNTIME_BLOB_COMMIT) {
        if (!runtime->blob_outbound_terminal_wait ||
            runtime->blob_pending_operation != MEISO_RUNTIME_BLOB_COMMIT ||
            ((result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
              result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE) ?
                next_offset != total_size : !canonical_prefix)) {
            return MEISO_RUNTIME_CONFLICT;
        }
    } else if (!runtime->blob_outbound_terminal_wait ||
               runtime->blob_pending_operation != MEISO_RUNTIME_BLOB_ABORT ||
               !canonical_prefix) {
        return MEISO_RUNTIME_CONFLICT;
    }
    event->type = MEISO_RUNTIME_EVENT_BLOB_RESULT;
    event->catalog_generation = route.catalog_generation;
    event->route = route;
    event->blob_operation = operation;
    event->blob_transfer = transfer;
    event->blob_result = result;
    event->blob_next_offset = next_offset;
    if (terminal) {
        meiso_runtime_private_clear_blob_transfer(runtime);
    } else if (!runtime->blob_outbound_terminal_wait) {
        runtime->blob_begin_ready = true;
    }
    return MEISO_RUNTIME_OK;
}
static void clear_pending_blob_result(MeisoRuntime *runtime)
{
    memset(
        &runtime->blob_result_transfer,
        0,
        sizeof(runtime->blob_result_transfer)
    );
    runtime->blob_result_next_offset = 0U;
    runtime->blob_pending_operation = 0U;
    runtime->blob_pending_result = 0U;
    runtime->blob_result_pending = false;
}

MeisoRuntimeStatus meiso_runtime_send_blob_result(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoRuntimeBlobOperation expected_operation
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    uint8_t *body;
    bool result_for_active;
    MeisoRuntimeBlobResult result;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (expected_transfer == NULL ||
        !meiso_runtime_private_blob_operation_valid(expected_operation) || meiso_runtime_private_ranges_overlap(
            runtime,
            runtime->arena_size,
            expected_transfer,
            sizeof(*expected_transfer)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->blob_result_pending) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (expected_operation != runtime->blob_pending_operation ||
        !blob_result_reference_equal(
            expected_transfer, &runtime->blob_result_transfer
        )) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_private_blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    body = meiso_runtime_private_tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    meiso_runtime_private_encode_route_key(body, runtime->blob_result_transfer.route);
    meiso_runtime_private_put_u64(body + 56U, runtime->blob_result_transfer.transfer_id);
    body[64U] = runtime->blob_pending_operation;
    body[65U] = runtime->blob_pending_result;
    memset(body + 66U, 0, 2U);
    meiso_runtime_private_put_u64(body + 68U, runtime->blob_result_next_offset);
    meiso_runtime_private_put_u64(body + 76U, runtime->blob_result_transfer.total_size);
    meiso_runtime_private_encode_header(
        runtime,
        meiso_runtime_private_tx_bytes(runtime),
        MEISO_RUNTIME_WIRE_BLOB_RESULT,
        MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE
    );
    status = meiso_runtime_private_submit_bytes(runtime, MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    result = runtime->blob_pending_result;
    result_for_active = runtime->blob_transfer_active &&
        meiso_runtime_private_blob_transfer_equal(
            &runtime->blob_result_transfer, &runtime->blob_transfer
        );
    if (result == MEISO_RUNTIME_BLOB_RESULT_READY) {
        if (result_for_active) {
            runtime->blob_begin_ready = true;
        }
        clear_pending_blob_result(runtime);
    } else if (result_for_active) {
        meiso_runtime_private_clear_blob_transfer(runtime);
    } else {
        clear_pending_blob_result(runtime);
    }
    return MEISO_RUNTIME_OK;
}
