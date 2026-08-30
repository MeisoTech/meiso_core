#include "meiso_runtime_scheduler_private.h"

#include <string.h>

/* Work validation, callback isolation, dispatch and fair source selection. */

static MeisoRuntimeSchedulerPollStatus call_poll(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource *source,
    MeisoRuntimeSchedulerWork *out_work
)
{
    MeisoRuntimeSchedulerPollStatus status;

    memset(out_work, 0, sizeof(*out_work));
    scheduler->callback_active = true;
    status = source->poll(source->context, out_work);
    scheduler->callback_active = false;
    return status;
}

static void call_complete(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource *source,
    const MeisoRuntimeSchedulerCompletion *completion
)
{
    scheduler->callback_active = true;
    source->complete(source->context, completion);
    scheduler->callback_active = false;
}

static bool work_route(
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeRouteKey *out_route
)
{
    if (work == NULL || out_route == NULL) {
        return false;
    }
    switch (work->kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        *out_route = work->value.state_offer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        *out_route = work->value.state_accept.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        *out_route = work->value.stream_sample.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN:
        *out_route = work->value.blob_begin.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        *out_route = work->value.blob_chunk.transfer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT:
        *out_route = work->value.blob_commit.transfer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT:
        *out_route = work->value.blob_abort.transfer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        *out_route = work->value.blob_result.transfer.route;
        return true;
    default:
        memset(out_route, 0, sizeof(*out_route));
        return false;
    }
}

static bool work_class_valid(
    uint8_t source_class,
    MeisoRuntimeSchedulerWorkKind kind
)
{
    if (source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_DATA) {
        return kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE ||
            kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
    }
    return source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL &&
        kind >= MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind <= MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
}

bool meiso_runtime_scheduler_private_work_kind_valid(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind >= MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind <= MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
}

uint8_t meiso_runtime_scheduler_private_class_for_work_kind(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE ||
        kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK ?
            MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_DATA : MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL;
}

bool meiso_runtime_scheduler_private_work_kind_has_route(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT;
}

static bool work_valid_for_source(
    const MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeSchedulerSource *source,
    const MeisoRuntimeSchedulerWork *work
)
{
    MeisoRuntimeRouteKey route;
    bool has_route;

    if (work == NULL || work->ticket == 0U ||
        work->kind != source->work_kind ||
        !work_class_valid(source->source_class, work->kind)) {
        return false;
    }
    if (work->kind == MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        ((scheduler->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          (work->value.catalog_offer.routes != NULL ||
           work->value.catalog_offer.routes_v4 == NULL)) ||
         (scheduler->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4 &&
          (work->value.catalog_offer.routes == NULL ||
           work->value.catalog_offer.routes_v4 != NULL)))) {
        return false;
    }
    has_route = work_route(work, &route);
    if (has_route && (!meiso_runtime_scheduler_private_route_valid(route) ||
        !scheduler->active_catalog_present ||
        route.catalog_generation != scheduler->catalog_generation)) {
        return false;
    }
    if (source->route_binding_required) {
        return source->route_bound && has_route &&
            meiso_runtime_scheduler_private_route_equal(route, source->route);
    }
    return true;
}

MeisoRuntimeStatus meiso_runtime_scheduler_private_dispatch_work(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeBlobTransferRef *out_generated,
    bool *out_generated_present
)
{
    *out_generated_present = false;
    memset(out_generated, 0, sizeof(*out_generated));
    switch (work->kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER:
        if (scheduler->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
            return meiso_runtime_v4_offer_catalog(
                scheduler->runtime,
                work->value.catalog_offer.generation,
                work->value.catalog_offer.endpoints,
                work->value.catalog_offer.endpoint_count,
                work->value.catalog_offer.routes_v4,
                work->value.catalog_offer.route_count
            );
        }
        return meiso_runtime_offer_catalog(
            scheduler->runtime,
            work->value.catalog_offer.generation,
            work->value.catalog_offer.endpoints,
            work->value.catalog_offer.endpoint_count,
            work->value.catalog_offer.routes,
            work->value.catalog_offer.route_count
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT:
        return meiso_runtime_accept_catalog(scheduler->runtime);
    case MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT:
        return meiso_runtime_reject_catalog(
            scheduler->runtime, work->value.catalog_reject.reason
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        return meiso_runtime_send_state_offer(
            scheduler->runtime,
            work->value.state_offer.route,
            &work->value.state_offer.offer
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        return meiso_runtime_send_state_accept(
            scheduler->runtime,
            work->value.state_accept.route,
            work->value.state_accept.generation,
            work->value.state_accept.owner,
            work->value.state_accept.version
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        return meiso_runtime_send_stream_sample(
            scheduler->runtime,
            work->value.stream_sample.route,
            work->value.stream_sample.production_epoch,
            &work->value.stream_sample.sample
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN: {
        MeisoRuntimeStatus status = meiso_runtime_send_blob_begin(
            scheduler->runtime,
            work->value.blob_begin.route,
            &work->value.blob_begin.content_id,
            work->value.blob_begin.total_size,
            out_generated
        );
        if (status == MEISO_RUNTIME_OK) {
            *out_generated_present = true;
        }
        return status;
    }
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        return meiso_runtime_send_blob_chunk(
            scheduler->runtime,
            &work->value.blob_chunk.transfer,
            work->value.blob_chunk.offset,
            work->value.blob_chunk.payload
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT:
        return meiso_runtime_send_blob_commit(
            scheduler->runtime, &work->value.blob_commit.transfer
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT:
        return meiso_runtime_send_blob_abort(
            scheduler->runtime,
            &work->value.blob_abort.transfer,
            work->value.blob_abort.reason
        );
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        return meiso_runtime_send_blob_result(
            scheduler->runtime,
            &work->value.blob_result.transfer,
            work->value.blob_result.operation
        );
    default:
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
}

bool meiso_runtime_scheduler_private_counters_available(
    const MeisoRuntimeScheduler *scheduler,
    uint8_t source_class,
    bool new_data_round
)
{
    return scheduler->successful_slots != UINT64_MAX &&
        (source_class != MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL ||
         scheduler->successful_control_slots != UINT64_MAX) &&
        (source_class != MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_DATA ||
         scheduler->successful_data_slots != UINT64_MAX) &&
        (!new_data_round || scheduler->data_rounds != UINT64_MAX);
}

static void fill_completion(
    MeisoRuntimeSchedulerCompletion *completion,
    const MeisoRuntimeSchedulerSource *source,
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeStatus runtime_status,
    const MeisoRuntimeBlobTransferRef *generated,
    bool generated_present
)
{
    memset(completion, 0, sizeof(*completion));
    completion->source_id = source->id;
    completion->ticket = work->ticket;
    completion->kind = work->kind;
    completion->runtime_status = runtime_status;
    completion->code = runtime_status == MEISO_RUNTIME_OK ?
        MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT :
        MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED;
    completion->generated_blob_transfer_present = generated_present;
    if (generated_present) {
        completion->generated_blob_transfer = *generated;
    }
}

static void complete_invalid_ready(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource *source,
    uint16_t source_index,
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeSchedulerStepResult *out_result
)
{
    MeisoRuntimeSchedulerCompletion completion;
    MeisoRuntimeBlobTransferRef generated;

    memset(&generated, 0, sizeof(generated));
    fill_completion(
        &completion,
        source,
        work,
        MEISO_RUNTIME_INVALID_ARGUMENT,
        &generated,
        false
    );
    if (source->source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL) {
        scheduler->control_cursor = (uint16_t)(
            (source_index + 1U) % scheduler->control_source_count
        );
    } else {
        scheduler->data_cursor = (uint16_t)(
            (source_index + 1U) % scheduler->data_source_count
        );
    }
    out_result->completion = completion;
    out_result->successful_slot = scheduler->successful_slots;
    out_result->data_round = scheduler->data_rounds;
    call_complete(scheduler, source, &completion);
}

void meiso_runtime_scheduler_private_rollback_new_data_round(MeisoRuntimeScheduler *scheduler)
{
    meiso_runtime_scheduler_private_clear_data_round(scheduler);
    scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
}

MeisoRuntimeSchedulerStepStatus meiso_runtime_scheduler_private_finish_attempt(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource *source,
    uint16_t source_index,
    const MeisoRuntimeSchedulerWork *work,
    bool new_data_round,
    MeisoRuntimeStatus runtime_status,
    const MeisoRuntimeBlobTransferRef *generated,
    bool generated_present,
    bool data_probe_performed,
    MeisoRuntimeSchedulerStepResult *out_result
)
{
    MeisoRuntimeSchedulerCompletion completion;

    if (runtime_status == MEISO_RUNTIME_TX_BUSY) {
        if (new_data_round) {
            meiso_runtime_scheduler_private_rollback_new_data_round(scheduler);
        }
        return MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY;
    }
    if (source->source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_DATA) {
        scheduler->data_probe_due = false;
    } else if (runtime_status != MEISO_RUNTIME_OK) {
        scheduler->data_probe_due = true;
    } else if (data_probe_performed) {
        scheduler->data_probe_due = false;
    }
    fill_completion(
        &completion,
        source,
        work,
        runtime_status,
        generated,
        generated_present
    );
    if (source->source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL) {
        scheduler->control_cursor = (uint16_t)(
            (source_index + 1U) % scheduler->control_source_count
        );
    } else {
        scheduler->data_cursor = (uint16_t)(
            (source_index + 1U) % scheduler->data_source_count
        );
        source->round_member = false;
        source->round_ticket = 0U;
        --scheduler->data_round_member_count;
        if (new_data_round) {
            ++scheduler->data_rounds;
        }
        if (scheduler->data_round_member_count == 0U) {
            scheduler->successful_control_burst = 0U;
            scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
        }
    }
    if (runtime_status == MEISO_RUNTIME_OK) {
        ++scheduler->successful_slots;
        if (source->source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL) {
            ++scheduler->successful_control_slots;
            if (scheduler->successful_control_burst <
                MEISO_RUNTIME_SCHEDULER_CONTROL_BURST) {
                ++scheduler->successful_control_burst;
            }
        } else {
            ++scheduler->successful_data_slots;
        }
    }
    out_result->completion = completion;
    out_result->successful_slot = scheduler->successful_slots;
    out_result->data_round = scheduler->data_rounds;
    call_complete(scheduler, source, &completion);
    return runtime_status == MEISO_RUNTIME_OK ?
        MEISO_RUNTIME_SCHEDULER_STEP_SENT :
        MEISO_RUNTIME_SCHEDULER_STEP_REJECTED;
}

bool meiso_runtime_scheduler_private_find_control_work(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource **out_source,
    uint16_t *out_index,
    MeisoRuntimeSchedulerWork *out_work,
    bool *out_source_invalid
)
{
    MeisoRuntimeSchedulerSource *sources = meiso_runtime_scheduler_private_control_sources(scheduler);
    uint16_t scan;
    const uint16_t scan_start = scheduler->control_cursor;
    bool invalid_seen = false;
    uint16_t invalid_index = 0U;

    *out_source_invalid = false;
    for (scan = 0U; scan < scheduler->control_source_count; ++scan) {
        uint16_t index = (uint16_t)(
            (scan_start + scan) %
            scheduler->control_source_count
        );
        MeisoRuntimeSchedulerPollStatus poll = call_poll(
            scheduler, &sources[index], out_work
        );
        if (poll == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY) {
            continue;
        }
        if (poll == MEISO_RUNTIME_SCHEDULER_POLL_INVALID) {
            invalid_seen = true;
            invalid_index = index;
            continue;
        }
        if (poll != MEISO_RUNTIME_SCHEDULER_POLL_READY) {
            invalid_seen = true;
            invalid_index = index;
            continue;
        }
        if (!work_valid_for_source(scheduler, &sources[index], out_work)) {
            MeisoRuntimeSchedulerStepResult ignored;
            invalid_seen = true;
            invalid_index = index;
            if (out_work->ticket != 0U) {
                memset(&ignored, 0, sizeof(ignored));
                complete_invalid_ready(
                    scheduler, &sources[index], index, out_work, &ignored
                );
            }
            continue;
        }
        *out_source = &sources[index];
        *out_index = index;
        return true;
    }
    if (invalid_seen) {
        scheduler->control_cursor = (uint16_t)(
            (invalid_index + 1U) % scheduler->control_source_count
        );
        *out_source_invalid = true;
    }
    return false;
}

bool meiso_runtime_scheduler_private_begin_data_round(
    MeisoRuntimeScheduler *scheduler,
    bool *out_source_invalid
)
{
    MeisoRuntimeSchedulerSource *sources = meiso_runtime_scheduler_private_data_sources(scheduler);
    uint16_t scan;
    const uint16_t scan_start = scheduler->data_cursor;
    bool invalid_seen = false;
    uint16_t invalid_index = 0U;

    *out_source_invalid = false;
    for (scan = 0U; scan < scheduler->data_source_count; ++scan) {
        uint16_t index = (uint16_t)(
            (scan_start + scan) % scheduler->data_source_count
        );
        MeisoRuntimeSchedulerWork work;
        MeisoRuntimeSchedulerPollStatus poll = call_poll(
            scheduler, &sources[index], &work
        );
        if (poll == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY) {
            continue;
        }
        if (poll == MEISO_RUNTIME_SCHEDULER_POLL_INVALID) {
            invalid_seen = true;
            invalid_index = index;
            continue;
        }
        if (poll != MEISO_RUNTIME_SCHEDULER_POLL_READY) {
            invalid_seen = true;
            invalid_index = index;
            continue;
        }
        if (!work_valid_for_source(scheduler, &sources[index], &work)) {
            MeisoRuntimeSchedulerStepResult ignored;
            invalid_seen = true;
            invalid_index = index;
            if (work.ticket != 0U) {
                memset(&ignored, 0, sizeof(ignored));
                complete_invalid_ready(
                    scheduler, &sources[index], index, &work, &ignored
                );
            }
            continue;
        }
        sources[index].round_member = true;
        sources[index].round_ticket = work.ticket;
        ++scheduler->data_round_member_count;
    }
    if (invalid_seen) {
        scheduler->data_cursor = (uint16_t)(
            (invalid_index + 1U) % scheduler->data_source_count
        );
    }
    if (scheduler->data_round_member_count == 0U) {
        *out_source_invalid = invalid_seen;
        return false;
    }
    scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND;
    return true;
}

bool meiso_runtime_scheduler_private_find_data_work(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource **out_source,
    uint16_t *out_index,
    MeisoRuntimeSchedulerWork *out_work,
    bool *out_source_invalid
)
{
    MeisoRuntimeSchedulerSource *sources = meiso_runtime_scheduler_private_data_sources(scheduler);
    uint16_t scan;
    const uint16_t scan_start = scheduler->data_cursor;
    bool invalid_seen = false;

    *out_source_invalid = false;
    for (scan = 0U; scan < scheduler->data_source_count; ++scan) {
        uint16_t index = (uint16_t)(
            (scan_start + scan) % scheduler->data_source_count
        );
        MeisoRuntimeSchedulerPollStatus poll;
        if (!sources[index].round_member) {
            continue;
        }
        poll = call_poll(scheduler, &sources[index], out_work);
        if (poll != MEISO_RUNTIME_SCHEDULER_POLL_READY ||
            out_work->ticket != sources[index].round_ticket ||
            !work_valid_for_source(scheduler, &sources[index], out_work)) {
            MeisoRuntimeSchedulerStepResult ignored;
            invalid_seen = true;
            if (poll == MEISO_RUNTIME_SCHEDULER_POLL_READY &&
                out_work->ticket != 0U) {
                memset(&ignored, 0, sizeof(ignored));
                complete_invalid_ready(
                    scheduler, &sources[index], index, out_work, &ignored
                );
            }
            sources[index].round_member = false;
            sources[index].round_ticket = 0U;
            --scheduler->data_round_member_count;
            scheduler->data_cursor = (uint16_t)(
                (index + 1U) % scheduler->data_source_count
            );
            if (scheduler->data_round_member_count == 0U) {
                scheduler->successful_control_burst = 0U;
                scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
            }
            continue;
        }
        *out_source = &sources[index];
        *out_index = index;
        return true;
    }
    scheduler->data_round_member_count = 0U;
    scheduler->successful_control_burst = 0U;
    scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
    *out_source_invalid = invalid_seen;
    return false;
}
