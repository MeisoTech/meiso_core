#include "meiso_runtime_scheduler.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define SCHEDULER_MAGIC UINT64_C(0x4d52545343484432)

enum {
    SOURCE_CLASS_CONTROL = 1,
    SOURCE_CLASS_DATA = 2
};

typedef struct {
    MeisoRuntimeSchedulerPoll poll;
    MeisoRuntimeSchedulerComplete complete;
    MeisoRuntimeSchedulerBoundaryNotify boundary;
    void *context;
    MeisoRuntimeRouteKey route;
    uint64_t round_ticket;
    MeisoRuntimeSchedulerSourceId id;
    MeisoRuntimeSchedulerWorkKind work_kind;
    uint8_t source_class;
    bool route_binding_required;
    bool route_bound;
    bool round_member;
} SchedulerSource;

struct MeisoRuntimeScheduler {
    uint64_t magic;
    size_t arena_size;
    MeisoRuntime *runtime;
    MeisoSession *session;
    MeisoSessionId session_id;
    size_t control_offset;
    size_t data_offset;
    uint64_t catalog_generation;
    uint64_t successful_slots;
    uint64_t successful_control_slots;
    uint64_t successful_data_slots;
    uint64_t data_rounds;
    uint16_t max_control_sources;
    uint16_t max_data_sources;
    uint16_t control_source_count;
    uint16_t data_source_count;
    uint16_t control_cursor;
    uint16_t data_cursor;
    uint16_t data_round_member_count;
    uint8_t successful_control_burst;
    MeisoSessionRole local_role;
    MeisoRuntimeSchedulerPhase phase;
    MeisoRuntimeSchedulerRxObligation rx_obligation;
    bool active_catalog_present;
    bool sealed;
    bool callback_active;
    bool data_probe_due;
};

typedef struct {
    size_t required;
    size_t control_offset;
    size_t data_offset;
} SchedulerLayout;

static bool checked_add(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

static bool align_up(size_t value, size_t alignment, size_t *out_value)
{
    size_t remainder;
    size_t addition;

    if (out_value == NULL || alignment == 0U) {
        return false;
    }
    remainder = value % alignment;
    addition = remainder == 0U ? 0U : alignment - remainder;
    return checked_add(value, addition, out_value);
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_begin = (uintptr_t)lhs;
    const uintptr_t rhs_begin = (uintptr_t)rhs;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_begin > UINTPTR_MAX - lhs_size ||
        rhs_begin > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

static bool arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % _Alignof(max_align_t) == 0U;
}

static bool calculate_layout(
    uint16_t max_control_sources,
    uint16_t max_data_sources,
    SchedulerLayout *out_layout
)
{
    SchedulerLayout value;
    size_t cursor = sizeof(MeisoRuntimeScheduler);
    size_t bytes;

    if (out_layout == NULL || max_control_sources == 0U ||
        max_data_sources == 0U ||
        (uint32_t)max_control_sources + max_data_sources > UINT16_MAX) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    if (!align_up(cursor, _Alignof(SchedulerSource), &cursor) ||
        !checked_multiply(
            max_control_sources, sizeof(SchedulerSource), &bytes
        )) {
        return false;
    }
    value.control_offset = cursor;
    if (!checked_add(cursor, bytes, &cursor) ||
        !align_up(cursor, _Alignof(SchedulerSource), &cursor) ||
        !checked_multiply(max_data_sources, sizeof(SchedulerSource), &bytes)) {
        return false;
    }
    value.data_offset = cursor;
    if (!checked_add(cursor, bytes, &cursor) ||
        !align_up(cursor, _Alignof(max_align_t), &value.required)) {
        return false;
    }
    *out_layout = value;
    return true;
}

static SchedulerSource *control_sources(MeisoRuntimeScheduler *scheduler)
{
    return (SchedulerSource *)(void *)(
        (uint8_t *)(void *)scheduler + scheduler->control_offset
    );
}

static const SchedulerSource *control_sources_const(
    const MeisoRuntimeScheduler *scheduler
)
{
    return (const SchedulerSource *)(const void *)(
        (const uint8_t *)(const void *)scheduler + scheduler->control_offset
    );
}

static SchedulerSource *data_sources(MeisoRuntimeScheduler *scheduler)
{
    return (SchedulerSource *)(void *)(
        (uint8_t *)(void *)scheduler + scheduler->data_offset
    );
}

static const SchedulerSource *data_sources_const(
    const MeisoRuntimeScheduler *scheduler
)
{
    return (const SchedulerSource *)(const void *)(
        (const uint8_t *)(const void *)scheduler + scheduler->data_offset
    );
}

static bool scheduler_layout_valid(const MeisoRuntimeScheduler *scheduler)
{
    SchedulerLayout layout;

    return scheduler != NULL && scheduler->magic == SCHEDULER_MAGIC &&
        calculate_layout(
            scheduler->max_control_sources,
            scheduler->max_data_sources,
            &layout
        ) && scheduler->arena_size == layout.required &&
        scheduler->control_offset == layout.control_offset &&
        scheduler->data_offset == layout.data_offset &&
        scheduler->control_source_count <= scheduler->max_control_sources &&
        scheduler->data_source_count <= scheduler->max_data_sources &&
        scheduler->control_cursor < scheduler->max_control_sources &&
        scheduler->data_cursor < scheduler->max_data_sources &&
        scheduler->data_round_member_count <=
            scheduler->data_source_count &&
        scheduler->successful_control_burst <=
            MEISO_RUNTIME_SCHEDULER_CONTROL_BURST &&
        scheduler->phase >= MEISO_RUNTIME_SCHEDULER_PHASE_CONFIGURING &&
        scheduler->phase <=
            MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL &&
        scheduler->rx_obligation <=
            MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH;
}

static bool output_overlaps_scheduler(
    const MeisoRuntimeScheduler *scheduler,
    const void *output,
    size_t output_size
)
{
    return scheduler != NULL && ranges_overlap(
        scheduler, scheduler->arena_size, output, output_size
    );
}

static bool route_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static bool route_valid(MeisoRuntimeRouteKey route)
{
    return route.catalog_generation != 0U &&
        meiso_endpoint_id_valid(route.endpoint_id) &&
        route.endpoint_incarnation != 0U &&
        meiso_resource_id_valid(route.resource_id) &&
        meiso_runtime_facet_valid(route.facet);
}

static bool session_runtime_ready(
    MeisoRuntime *runtime,
    MeisoSession *session,
    MeisoRuntimeSnapshot *out_runtime,
    MeisoSessionSnapshot *out_session
)
{
    return runtime != NULL && session != NULL && out_runtime != NULL &&
        out_session != NULL && meiso_runtime_snapshot(runtime, out_runtime) &&
        meiso_session_snapshot(session, out_session) &&
        out_runtime->core_ready &&
        out_session->state == MEISO_SESSION_STATE_READY &&
        out_session->session_id_known &&
        out_runtime->local_role == out_session->local_role &&
        meiso_session_id_equal(
            out_runtime->session_id, out_session->session_id
        ) && memcmp(
            &out_runtime->negotiated_limits,
            &out_session->negotiated_limits,
            sizeof(out_runtime->negotiated_limits)
        ) == 0;
}

static SchedulerSource *source_for_id(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSourceId source_id
)
{
    uint32_t index;

    if (source_id == 0U) {
        return NULL;
    }
    index = (uint32_t)source_id - 1U;
    if (index < scheduler->control_source_count) {
        return &control_sources(scheduler)[index];
    }
    index -= scheduler->max_control_sources;
    if (index < scheduler->data_source_count) {
        return &data_sources(scheduler)[index];
    }
    return NULL;
}

static void clear_data_round(MeisoRuntimeScheduler *scheduler)
{
    SchedulerSource *sources = data_sources(scheduler);
    uint16_t index;

    for (index = 0U; index < scheduler->data_source_count; ++index) {
        sources[index].round_member = false;
        sources[index].round_ticket = 0U;
    }
    scheduler->data_round_member_count = 0U;
    if (scheduler->phase == MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND) {
        scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
    }
}

static void clear_route_bindings(MeisoRuntimeScheduler *scheduler)
{
    SchedulerSource *controls = control_sources(scheduler);
    SchedulerSource *data = data_sources(scheduler);
    uint16_t index;

    clear_data_round(scheduler);
    for (index = 0U; index < scheduler->control_source_count; ++index) {
        controls[index].route_bound = false;
        memset(&controls[index].route, 0, sizeof(controls[index].route));
    }
    for (index = 0U; index < scheduler->data_source_count; ++index) {
        data[index].route_bound = false;
        memset(&data[index].route, 0, sizeof(data[index].route));
    }
}

static void call_boundary(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerBoundary boundary,
    uint64_t generation
)
{
    SchedulerSource *controls = control_sources(scheduler);
    SchedulerSource *data = data_sources(scheduler);
    uint16_t index;

    scheduler->callback_active = true;
    for (index = 0U; index < scheduler->control_source_count; ++index) {
        controls[index].boundary(
            controls[index].context, boundary, generation
        );
    }
    for (index = 0U; index < scheduler->data_source_count; ++index) {
        data[index].boundary(data[index].context, boundary, generation);
    }
    scheduler->callback_active = false;
}

static MeisoRuntimeSchedulerStatus refresh_boundary(
    MeisoRuntimeScheduler *scheduler,
    MeisoSessionSnapshot *out_session
)
{
    MeisoRuntimeSnapshot runtime_snapshot;
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeStatus runtime_status;
    uint64_t generation;
    bool present;

    runtime_status = meiso_runtime_refresh_bound_session(scheduler->runtime);
    if (runtime_status == MEISO_RUNTIME_CONFLICT) {
        return MEISO_RUNTIME_SCHEDULER_REENTRANT;
    }
    if (runtime_status != MEISO_RUNTIME_OK &&
        runtime_status != MEISO_RUNTIME_CORE_NOT_READY &&
        runtime_status != MEISO_RUNTIME_STALE_SESSION) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_STATE;
    }
    if (runtime_status != MEISO_RUNTIME_OK || !session_runtime_ready(
            scheduler->runtime,
            scheduler->session,
            &runtime_snapshot,
            &session_snapshot
        ) || !meiso_session_id_equal(
            scheduler->session_id, session_snapshot.session_id
        )) {
        if (scheduler->phase !=
            MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL) {
            clear_route_bindings(scheduler);
            scheduler->rx_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
            scheduler->data_probe_due = false;
            scheduler->active_catalog_present = false;
            scheduler->catalog_generation = 0U;
            scheduler->phase =
                MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL;
            call_boundary(
                scheduler,
                MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL,
                0U
            );
        }
        return MEISO_RUNTIME_SCHEDULER_STALE_SESSION;
    }
    if (out_session != NULL) {
        *out_session = session_snapshot;
    }
    present = runtime_snapshot.active_catalog_present;
    generation = present ? runtime_snapshot.active_catalog_generation : 0U;
    if (scheduler->sealed &&
        (present != scheduler->active_catalog_present ||
         generation != scheduler->catalog_generation)) {
        clear_route_bindings(scheduler);
        scheduler->active_catalog_present = present;
        scheduler->catalog_generation = generation;
        scheduler->successful_control_burst = 0U;
        scheduler->data_probe_due = false;
        scheduler->phase = present ?
            MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED :
            MEISO_RUNTIME_SCHEDULER_PHASE_READY;
        call_boundary(
            scheduler,
            MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED,
            generation
        );
        if (present) {
            return MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED;
        }
    }
    return MEISO_RUNTIME_SCHEDULER_OK;
}

static MeisoRuntimeSchedulerPollStatus call_poll(
    MeisoRuntimeScheduler *scheduler,
    SchedulerSource *source,
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
    SchedulerSource *source,
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
    if (source_class == SOURCE_CLASS_DATA) {
        return kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE ||
            kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
    }
    return source_class == SOURCE_CLASS_CONTROL &&
        kind >= MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind <= MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
}

static bool work_kind_valid(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind >= MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind <= MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
}

static uint8_t class_for_work_kind(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE ||
        kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK ?
            SOURCE_CLASS_DATA : SOURCE_CLASS_CONTROL;
}

static bool work_kind_has_route(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT;
}

static bool work_valid_for_source(
    const MeisoRuntimeScheduler *scheduler,
    const SchedulerSource *source,
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
    has_route = work_route(work, &route);
    if (has_route && (!route_valid(route) ||
        !scheduler->active_catalog_present ||
        route.catalog_generation != scheduler->catalog_generation)) {
        return false;
    }
    if (source->route_binding_required) {
        return source->route_bound && has_route &&
            route_equal(route, source->route);
    }
    return true;
}

static MeisoRuntimeStatus dispatch_work(
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

static bool counters_available(
    const MeisoRuntimeScheduler *scheduler,
    uint8_t source_class,
    bool new_data_round
)
{
    return scheduler->successful_slots != UINT64_MAX &&
        (source_class != SOURCE_CLASS_CONTROL ||
         scheduler->successful_control_slots != UINT64_MAX) &&
        (source_class != SOURCE_CLASS_DATA ||
         scheduler->successful_data_slots != UINT64_MAX) &&
        (!new_data_round || scheduler->data_rounds != UINT64_MAX);
}

static void fill_completion(
    MeisoRuntimeSchedulerCompletion *completion,
    const SchedulerSource *source,
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
    SchedulerSource *source,
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
    if (source->source_class == SOURCE_CLASS_CONTROL) {
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

static void rollback_new_data_round(MeisoRuntimeScheduler *scheduler)
{
    clear_data_round(scheduler);
    scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
}

static MeisoRuntimeSchedulerStepStatus finish_attempt(
    MeisoRuntimeScheduler *scheduler,
    SchedulerSource *source,
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
            rollback_new_data_round(scheduler);
        }
        return MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY;
    }
    if (source->source_class == SOURCE_CLASS_DATA) {
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
    if (source->source_class == SOURCE_CLASS_CONTROL) {
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
        if (source->source_class == SOURCE_CLASS_CONTROL) {
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

static bool find_control_work(
    MeisoRuntimeScheduler *scheduler,
    SchedulerSource **out_source,
    uint16_t *out_index,
    MeisoRuntimeSchedulerWork *out_work,
    bool *out_source_invalid
)
{
    SchedulerSource *sources = control_sources(scheduler);
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

static bool begin_data_round(
    MeisoRuntimeScheduler *scheduler,
    bool *out_source_invalid
)
{
    SchedulerSource *sources = data_sources(scheduler);
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

static bool find_data_work(
    MeisoRuntimeScheduler *scheduler,
    SchedulerSource **out_source,
    uint16_t *out_index,
    MeisoRuntimeSchedulerWork *out_work,
    bool *out_source_invalid
)
{
    SchedulerSource *sources = data_sources(scheduler);
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

size_t meiso_runtime_scheduler_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_runtime_scheduler_memory_required(
    const MeisoRuntimeSchedulerConfig *config
)
{
    SchedulerLayout layout;
    MeisoRuntimeSnapshot runtime_snapshot;
    MeisoSessionSnapshot session_snapshot;

    if (config == NULL || !calculate_layout(
            config->max_control_sources,
            config->max_data_sources,
            &layout
        ) || !session_runtime_ready(
            config->runtime,
            config->session,
            &runtime_snapshot,
            &session_snapshot
        )) {
        return 0U;
    }
    return layout.required;
}

MeisoRuntimeSchedulerInitStatus meiso_runtime_scheduler_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeSchedulerConfig *config,
    MeisoRuntimeScheduler **out_scheduler
)
{
    SchedulerLayout layout;
    MeisoRuntimeSnapshot runtime_snapshot;
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeScheduler *scheduler;

    if (config == NULL || out_scheduler == NULL || ranges_overlap(
            config, sizeof(*config), out_scheduler, sizeof(*out_scheduler)
        ) || (arena != NULL && ranges_overlap(
            arena, arena_size, config, sizeof(*config)
        )) || (arena != NULL && ranges_overlap(
            arena, arena_size, out_scheduler, sizeof(*out_scheduler)
        ))) {
        return MEISO_RUNTIME_SCHEDULER_INIT_INVALID_ARGUMENT;
    }
    *out_scheduler = NULL;
    if (!calculate_layout(
            config->max_control_sources,
            config->max_data_sources,
            &layout
        ) || config->runtime == NULL || config->session == NULL) {
        return MEISO_RUNTIME_SCHEDULER_INIT_INVALID_CONFIG;
    }
    if (!session_runtime_ready(
            config->runtime,
            config->session,
            &runtime_snapshot,
            &session_snapshot
        )) {
        return MEISO_RUNTIME_SCHEDULER_INIT_CORE_NOT_READY;
    }
    if (arena == NULL) {
        return MEISO_RUNTIME_SCHEDULER_INIT_INVALID_ARGUMENT;
    }
    if (!arena_aligned(arena)) {
        return MEISO_RUNTIME_SCHEDULER_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < layout.required) {
        return MEISO_RUNTIME_SCHEDULER_INIT_ARENA_TOO_SMALL;
    }
    memset(arena, 0, layout.required);
    scheduler = arena;
    scheduler->magic = SCHEDULER_MAGIC;
    scheduler->arena_size = layout.required;
    scheduler->runtime = config->runtime;
    scheduler->session = config->session;
    scheduler->session_id = session_snapshot.session_id;
    scheduler->control_offset = layout.control_offset;
    scheduler->data_offset = layout.data_offset;
    scheduler->max_control_sources = config->max_control_sources;
    scheduler->max_data_sources = config->max_data_sources;
    scheduler->local_role = session_snapshot.local_role;
    scheduler->active_catalog_present =
        runtime_snapshot.active_catalog_present;
    scheduler->catalog_generation = runtime_snapshot.active_catalog_present ?
        runtime_snapshot.active_catalog_generation : 0U;
    scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_CONFIGURING;
    *out_scheduler = scheduler;
    return MEISO_RUNTIME_SCHEDULER_INIT_OK;
}

MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_register_source(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeSchedulerSourceConfig *config,
    MeisoRuntimeSchedulerSourceId *out_source_id
)
{
    SchedulerSource *source;
    uint16_t index;

    if (!scheduler_layout_valid(scheduler) || config == NULL ||
        out_source_id == NULL || output_overlaps_scheduler(
            scheduler, out_source_id, sizeof(*out_source_id)
        ) || ranges_overlap(
            scheduler, scheduler->arena_size, config, sizeof(*config)
        ) || ranges_overlap(
            config, sizeof(*config), out_source_id, sizeof(*out_source_id)
        )) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_SCHEDULER_REENTRANT;
    }
    if (scheduler->sealed ||
        scheduler->phase != MEISO_RUNTIME_SCHEDULER_PHASE_CONFIGURING) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_STATE;
    }
    if (!work_kind_valid(config->work_kind) || config->poll == NULL ||
        config->complete == NULL || config->boundary == NULL ||
        ranges_overlap(
            scheduler, scheduler->arena_size, config->context, 1U
        )) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (class_for_work_kind(config->work_kind) == SOURCE_CLASS_CONTROL) {
        if (scheduler->control_source_count ==
            scheduler->max_control_sources) {
            return MEISO_RUNTIME_SCHEDULER_CAPACITY;
        }
        index = scheduler->control_source_count++;
        source = &control_sources(scheduler)[index];
        source->id = (MeisoRuntimeSchedulerSourceId)(index + 1U);
    } else {
        if (scheduler->data_source_count == scheduler->max_data_sources) {
            return MEISO_RUNTIME_SCHEDULER_CAPACITY;
        }
        index = scheduler->data_source_count++;
        source = &data_sources(scheduler)[index];
        source->id = (MeisoRuntimeSchedulerSourceId)(
            scheduler->max_control_sources + index + 1U
        );
    }
    source->poll = config->poll;
    source->complete = config->complete;
    source->boundary = config->boundary;
    source->context = config->context;
    source->work_kind = config->work_kind;
    source->source_class = class_for_work_kind(config->work_kind);
    source->route_binding_required = work_kind_has_route(config->work_kind);
    *out_source_id = source->id;
    return MEISO_RUNTIME_SCHEDULER_OK;
}

MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_seal(
    MeisoRuntimeScheduler *scheduler
)
{
    if (!scheduler_layout_valid(scheduler)) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_SCHEDULER_REENTRANT;
    }
    if (scheduler->sealed) {
        return MEISO_RUNTIME_SCHEDULER_DUPLICATE;
    }
    scheduler->sealed = true;
    scheduler->phase = scheduler->active_catalog_present ?
        MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED :
        MEISO_RUNTIME_SCHEDULER_PHASE_READY;
    return scheduler->active_catalog_present ?
        MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED :
        MEISO_RUNTIME_SCHEDULER_OK;
}

MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_bind_source_route(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSourceId source_id,
    MeisoRuntimeRouteKey route
)
{
    SchedulerSource *source;
    const SchedulerSource *data;
    MeisoRuntimeRouteDescriptor active_route;
    uint16_t index;

    if (!scheduler_layout_valid(scheduler) || !route_valid(route)) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_SCHEDULER_REENTRANT;
    }
    if (!scheduler->sealed || !scheduler->active_catalog_present ||
        scheduler->phase !=
            MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED ||
        route.catalog_generation != scheduler->catalog_generation) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_STATE;
    }
    if (meiso_runtime_find_route(
            scheduler->runtime, route.resource_id, &active_route
        ) != MEISO_RUNTIME_OK || !meiso_endpoint_id_equal(
            active_route.endpoint_id, route.endpoint_id
        ) || active_route.endpoint_incarnation !=
            route.endpoint_incarnation || !meiso_resource_id_equal(
            active_route.resource.id, route.resource_id
        ) || active_route.facet != route.facet) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    source = source_for_id(scheduler, source_id);
    if (source == NULL || !source->route_binding_required) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (source->source_class == SOURCE_CLASS_DATA) {
        data = data_sources_const(scheduler);
        for (index = 0U; index < scheduler->data_source_count; ++index) {
            if (&data[index] != source && data[index].route_bound &&
                route_equal(data[index].route, route)) {
                return MEISO_RUNTIME_SCHEDULER_DUPLICATE;
            }
        }
    }
    source->route = route;
    source->route_bound = true;
    return MEISO_RUNTIME_SCHEDULER_OK;
}

MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_finish_rebind(
    MeisoRuntimeScheduler *scheduler
)
{
    const SchedulerSource *controls;
    const SchedulerSource *data;
    uint16_t index;

    if (!scheduler_layout_valid(scheduler)) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_SCHEDULER_REENTRANT;
    }
    if (!scheduler->sealed || !scheduler->active_catalog_present ||
        scheduler->phase !=
            MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_STATE;
    }
    controls = control_sources_const(scheduler);
    data = data_sources_const(scheduler);
    for (index = 0U; index < scheduler->control_source_count; ++index) {
        if (controls[index].route_binding_required &&
            !controls[index].route_bound) {
            return MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED;
        }
    }
    for (index = 0U; index < scheduler->data_source_count; ++index) {
        if (!data[index].route_bound) {
            return MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED;
        }
    }
    scheduler->phase = MEISO_RUNTIME_SCHEDULER_PHASE_READY;
    return MEISO_RUNTIME_SCHEDULER_OK;
}

MeisoRuntimeStatus meiso_runtime_scheduler_receive(
    MeisoRuntimeScheduler *scheduler,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
)
{
    MeisoRuntimeStatus status;
    MeisoRuntimeBlobSnapshot blob;

    if (!scheduler_layout_valid(scheduler) || message == NULL ||
        out_event == NULL || output_overlaps_scheduler(
            scheduler, out_event, sizeof(*out_event)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active || !scheduler->sealed ||
        scheduler->phase ==
            MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (scheduler->rx_obligation != MEISO_RUNTIME_SCHEDULER_RX_NONE) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_receive(scheduler->runtime, message, out_event);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_event->type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
        scheduler->rx_obligation =
            MEISO_RUNTIME_SCHEDULER_RX_STATE_COMMIT;
    } else if (out_event->type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
        out_event->blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
        meiso_runtime_blob_snapshot(scheduler->runtime, &blob) &&
        blob.publish_pending) {
        scheduler->rx_obligation =
            MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH;
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_scheduler_commit_state_receive(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
)
{
    MeisoRuntimeStatus status;

    if (!scheduler_layout_valid(scheduler)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (scheduler->rx_obligation !=
        MEISO_RUNTIME_SCHEDULER_RX_STATE_COMMIT) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_commit_state_receive(
        scheduler->runtime, route, offer, receive_status
    );
    scheduler->rx_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
    return status;
}

MeisoRuntimeStatus meiso_runtime_scheduler_publish_blob_receive(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
)
{
    MeisoRuntimeStatus status;
    MeisoRuntimeBlobSnapshot blob;

    if (!scheduler_layout_valid(scheduler) ||
        output_overlaps_scheduler(
            scheduler, out_report, sizeof(*out_report)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (scheduler->rx_obligation !=
        MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_publish_blob_receive(
        scheduler->runtime,
        expected_transfer,
        store,
        verifier,
        verifier_context,
        out_report
    );
    if (meiso_runtime_blob_snapshot(scheduler->runtime, &blob) &&
        !blob.publish_pending) {
        scheduler->rx_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
    }
    return status;
}

MeisoRuntimeSchedulerStepStatus meiso_runtime_scheduler_step(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerStepResult *out_result
)
{
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeSchedulerStatus boundary_status;
    MeisoRuntimeBlobSnapshot blob;
    SchedulerSource *source = NULL;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeBlobTransferRef generated;
    MeisoRuntimeStatus runtime_status;
    uint16_t source_index = 0U;
    bool source_invalid = false;
    bool invalid_seen = false;
    bool control_scanned = false;
    bool data_probe_performed = false;
    bool generated_present = false;
    bool new_data_round = false;
    bool found;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerStepStatus step_status;

    if (!scheduler_layout_valid(scheduler) || out_result == NULL ||
        output_overlaps_scheduler(scheduler, out_result, sizeof(*out_result))) {
        return MEISO_RUNTIME_SCHEDULER_STEP_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT;
    }
    if (!scheduler->sealed) {
        return MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE;
    }
    boundary_status = refresh_boundary(scheduler, &session_snapshot);
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_REENTRANT) {
        return MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT;
    }
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_INVALID_STATE) {
        return MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE;
    }
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_STALE_SESSION) {
        return MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL;
    }
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED ||
        scheduler->phase ==
            MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED) {
        return MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED;
    }
    if (scheduler->rx_obligation != MEISO_RUNTIME_SCHEDULER_RX_NONE ||
        (meiso_runtime_blob_snapshot(scheduler->runtime, &blob) &&
         blob.publish_pending)) {
        return MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION;
    }
    if (scheduler->phase ==
        MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL) {
        return MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL;
    }
    if (session_snapshot.outbound_pending) {
        return MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY;
    }
    memset(&result, 0, sizeof(result));

    if (scheduler->phase == MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND) {
        found = find_data_work(
            scheduler,
            &source,
            &source_index,
            &work,
            &source_invalid
        );
        invalid_seen = source_invalid;
        if (!found) {
            source_invalid = false;
            found = find_control_work(
                scheduler,
                &source,
                &source_index,
                &work,
                &source_invalid
            );
            invalid_seen = invalid_seen || source_invalid;
        }
    } else {
        found = false;
        if (!scheduler->data_probe_due &&
            scheduler->successful_control_burst <
                MEISO_RUNTIME_SCHEDULER_CONTROL_BURST) {
            control_scanned = true;
            found = find_control_work(
                scheduler,
                &source,
                &source_index,
                &work,
                &source_invalid
            );
            invalid_seen = source_invalid;
        }
        if (!found) {
            data_probe_performed = true;
            source_invalid = false;
            if (begin_data_round(scheduler, &source_invalid)) {
                new_data_round = true;
                found = find_data_work(
                    scheduler,
                    &source,
                    &source_index,
                    &work,
                    &source_invalid
                );
            }
            invalid_seen = invalid_seen || source_invalid;
        }
        if (!found && !control_scanned) {
            new_data_round = false;
            source_invalid = false;
            found = find_control_work(
                scheduler,
                &source,
                &source_index,
                &work,
                &source_invalid
            );
            invalid_seen = invalid_seen || source_invalid;
        }
    }
    if (!found) {
        if (data_probe_performed) {
            scheduler->data_probe_due = false;
        }
        return invalid_seen ?
            MEISO_RUNTIME_SCHEDULER_STEP_SOURCE_INVALID :
            MEISO_RUNTIME_SCHEDULER_STEP_IDLE;
    }
    if (!counters_available(
            scheduler, source->source_class, new_data_round
        )) {
        if (new_data_round) {
            rollback_new_data_round(scheduler);
        }
        return MEISO_RUNTIME_SCHEDULER_STEP_COUNTER_EXHAUSTED;
    }
    runtime_status = dispatch_work(
        scheduler,
        &work,
        &generated,
        &generated_present
    );
    step_status = finish_attempt(
        scheduler,
        source,
        source_index,
        &work,
        new_data_round,
        runtime_status,
        &generated,
        generated_present,
        data_probe_performed,
        &result
    );
    if (step_status != MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY) {
        *out_result = result;
    }
    return step_status;
}

bool meiso_runtime_scheduler_snapshot(
    const MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSnapshot *out_snapshot
)
{
    if (!scheduler_layout_valid(scheduler) || out_snapshot == NULL ||
        scheduler->callback_active || output_overlaps_scheduler(
            scheduler, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->session_id = scheduler->session_id;
    out_snapshot->catalog_generation = scheduler->catalog_generation;
    out_snapshot->successful_slots = scheduler->successful_slots;
    out_snapshot->successful_control_slots =
        scheduler->successful_control_slots;
    out_snapshot->successful_data_slots =
        scheduler->successful_data_slots;
    out_snapshot->data_rounds = scheduler->data_rounds;
    out_snapshot->control_source_count = scheduler->control_source_count;
    out_snapshot->data_source_count = scheduler->data_source_count;
    out_snapshot->data_round_member_count =
        scheduler->data_round_member_count;
    out_snapshot->successful_control_burst =
        scheduler->successful_control_burst;
    out_snapshot->phase = scheduler->phase;
    out_snapshot->rx_obligation = scheduler->rx_obligation;
    out_snapshot->data_probe_due = scheduler->data_probe_due;
    out_snapshot->sealed = scheduler->sealed;
    return true;
}
