#include "meiso_runtime_private.h"

#include <string.h>

/* Common validated receive boundary and wire-type dispatch. */

MeisoRuntimeStatus meiso_runtime_receive(
    MeisoRuntime *runtime,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
)
{
    MeisoRuntimeStatus status;
    uint8_t type;

    /*
     * Reject a structurally canonical STREAM_SAMPLE with an impossible
     * delivery or producer direction through the const path, before the
     * ordinary receive boundary can clear a pending State/Blob obligation or
     * caller output.
     */
    if (meiso_runtime_private_layout_valid(runtime) &&
        runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
        meiso_runtime_private_check_bound_const(runtime) == MEISO_RUNTIME_OK && message != NULL &&
        out_event != NULL && meiso_byte_view_valid(message->payload) &&
        meiso_session_id_equal(message->session_id, runtime->session_id) &&
        !meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_event, sizeof(*out_event)
        ) && !meiso_runtime_private_ranges_overlap(
            out_event,
            sizeof(*out_event),
            message->payload.data,
            message->payload.size
        ) && meiso_runtime_private_decode_header(runtime, message->payload, &type) &&
        type == MEISO_RUNTIME_WIRE_STREAM_SAMPLE) {
        MeisoRuntimeEvent ignored_event;

        status = meiso_runtime_private_receive_stream_sample(
            runtime, message->payload, &ignored_event
        );
        if (status == MEISO_RUNTIME_DELIVERY_MISMATCH ||
            status == MEISO_RUNTIME_WRONG_ROLE ||
            status == MEISO_RUNTIME_OWNER_MISMATCH) {
            return status;
        }
    }

    status = meiso_runtime_private_check_bound_mutable(runtime);

    if (status != MEISO_RUNTIME_OK || runtime == NULL) {
        return status != MEISO_RUNTIME_OK ? status :
            MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
    if (message == NULL || out_event == NULL ||
        !meiso_byte_view_valid(message->payload) ||
        !meiso_session_id_equal(message->session_id, runtime->session_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_event, sizeof(*out_event)
        ) || meiso_runtime_private_ranges_overlap(
            out_event,
            sizeof(*out_event),
            message->payload.data,
            message->payload.size
        )) {
        return message != NULL &&
            !meiso_session_id_equal(message->session_id, runtime->session_id) ?
            MEISO_RUNTIME_STALE_SESSION : MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (message->payload.size >= MEISO_RUNTIME_WIRE_HEADER_SIZE &&
        message->payload.size <= UINT32_MAX &&
        message->payload.data[0] == (uint8_t)'M' &&
        message->payload.data[1] == (uint8_t)'R' &&
        message->payload.data[2] == (uint8_t)'T' &&
        message->payload.data[3] == (uint8_t)'0' &&
        message->payload.data[6] == 0U &&
        message->payload.data[7] == 0U &&
        meiso_runtime_private_get_u32(message->payload.data + 8U) == message->payload.size &&
        message->payload.data[4] != runtime->wire_version) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (!meiso_runtime_private_decode_header(runtime, message->payload, &type)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(out_event, 0, sizeof(*out_event));
    switch (type) {
    case MEISO_RUNTIME_WIRE_CATALOG:
        return meiso_runtime_private_receive_catalog(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_CATALOG_ACCEPT:
        return meiso_runtime_private_receive_catalog_accept(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_CATALOG_REJECT:
        return meiso_runtime_private_receive_catalog_reject(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_STATE_OFFER:
        return meiso_runtime_private_receive_state_offer(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_STATE_ACCEPT:
        return meiso_runtime_private_receive_state_accept(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_STREAM_SAMPLE:
        return meiso_runtime_private_receive_stream_sample(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_BLOB_BEGIN:
        return meiso_runtime_private_receive_blob_begin(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_BLOB_CHUNK:
        return meiso_runtime_private_receive_blob_chunk(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_BLOB_COMMIT:
        return meiso_runtime_private_receive_blob_commit(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_BLOB_ABORT:
        return meiso_runtime_private_receive_blob_abort(runtime, message->payload, out_event);
    case MEISO_RUNTIME_WIRE_BLOB_RESULT:
        return meiso_runtime_private_receive_blob_result(runtime, message->payload, out_event);
    default:
        return MEISO_RUNTIME_MALFORMED;
    }
}
