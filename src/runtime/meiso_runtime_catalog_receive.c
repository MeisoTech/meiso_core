#include "meiso_runtime_private.h"

#include <string.h>

/* Incoming Catalog offer/decision handlers used by the receive boundary. */

MeisoRuntimeStatus meiso_runtime_private_receive_catalog(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    uint64_t generation = 0U;
    MeisoRuntimeStatus status;

    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_CATALOG_PENDING;
    }
    status = meiso_runtime_private_decode_catalog_to_staging(runtime, frame, &generation);
    if (status == MEISO_RUNTIME_OK) {
        event->type = MEISO_RUNTIME_EVENT_CATALOG_OFFERED;
        event->catalog_generation = generation;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_private_receive_catalog_accept(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    uint64_t generation;

    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size !=
        MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_ACCEPT_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = meiso_runtime_private_get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (!runtime->catalog_decision_pending || !runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation == 0U ||
        generation != runtime->pending_catalog_generation ||
        generation != runtime->generation[meiso_runtime_private_staging_bank(runtime)]) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    meiso_runtime_private_commit_staged(runtime);
    event->type = MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED;
    event->catalog_generation = generation;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_receive_catalog_reject(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t bank = meiso_runtime_private_staging_bank(runtime);
    uint64_t generation;
    MeisoRuntimeCatalogRejectReason reason;

    if (frame.size != MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_REJECT_SIZE ||
        !meiso_runtime_private_bytes_zero(
            frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 9U,
            3U
        )) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = meiso_runtime_private_get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    reason = frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U];
    if (!meiso_runtime_private_reject_reason_valid(reason)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation == 0U || generation != runtime->pending_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    runtime->staged_present = false;
    meiso_runtime_private_clear_catalog_bank(runtime, bank);
    meiso_runtime_private_clear_catalog_decision(runtime);
    event->type = MEISO_RUNTIME_EVENT_CATALOG_REJECTED;
    event->catalog_generation = generation;
    event->reject_reason = reason;
    return MEISO_RUNTIME_OK;
}
