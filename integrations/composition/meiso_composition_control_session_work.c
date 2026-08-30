#include "meiso_composition_control_session_private.h"

#include <string.h>

/* Catalog activation, fixed source binding, and work/completion queues. */

static void clear_source(MeisoCompositionControlSessionPrivateTxSource *source)
{
    if (source != NULL) {
        memset(&source->work, 0, sizeof(source->work));
        memset(&source->completion, 0, sizeof(source->completion));
        source->pending = false;
        source->completion_pending = false;
    }
}
static void clear_all_sources(MeisoCompositionControlSession *control_session)
{
    size_t index;

    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        clear_source(&control_session->sources[index]);
    }
}

void meiso_composition_control_session_private_mark_terminal(MeisoCompositionControlSession *control_session)
{
    if (control_session != NULL && !control_session->terminal) {
        clear_all_sources(control_session);
        control_session->terminal = true;
        control_session->phase = MEISO_COMPOSITION_CONTROL_SESSION_PHASE_TERMINAL;
    }
}

void meiso_composition_control_session_private_terminate(
    MeisoCompositionControlSession *control_session,
    int error_number
)
{
    MeisoRuntimeSchedulerStepResult ignored;

    if (control_session == NULL || control_session->terminal) {
        return;
    }
    if (control_session->adapter != NULL) {
        meiso_posix_tcp_abort(control_session->adapter, error_number);
    }
    if (control_session->scheduler_present) {
        (void)meiso_runtime_scheduler_step(control_session->scheduler, &ignored);
    } else if (control_session->runtime_present) {
        (void)meiso_runtime_refresh_bound_session(control_session->runtime);
    }
    meiso_composition_control_session_private_mark_terminal(control_session);
}

static MeisoRuntimeRouteKey route_key_for_index(
    const MeisoCompositionControlSession *control_session,
    uint8_t route_index
)
{
    const MeisoRuntimeRouteDescriptor *route =
        &control_session->routes[route_index].route;
    MeisoRuntimeRouteKey key;

    memset(&key, 0, sizeof(key));
    key.catalog_generation = control_session->catalog_generation;
    key.endpoint_id = route->endpoint_id;
    key.endpoint_incarnation = route->endpoint_incarnation;
    key.resource_id = route->resource.id;
    key.facet = route->facet;
    return key;
}

static bool route_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
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

static bool work_borrows_control_session_arena(
    const MeisoCompositionControlSession *control_session,
    const MeisoRuntimeSchedulerWork *work
)
{
    MeisoByteView first = {NULL, 0U};
    MeisoByteView second = {NULL, 0U};

    switch (work->kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        first = work->value.state_offer.offer.state.payload;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        first = work->value.stream_sample.sample.dependency;
        second = work->value.stream_sample.sample.payload;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        first = work->value.blob_chunk.payload;
        break;
    default:
        break;
    }
    return meiso_composition_control_session_private_ranges_overlap(control_session, control_session->arena_size, first.data, first.size) ||
        meiso_composition_control_session_private_ranges_overlap(control_session, control_session->arena_size, second.data, second.size);
}

static MeisoRuntimeSchedulerPollStatus source_poll(
    void *context,
    MeisoRuntimeSchedulerWork *out_work
)
{
    MeisoCompositionControlSessionPrivateTxSource *source = context;

    if (source == NULL || out_work == NULL) {
        return MEISO_RUNTIME_SCHEDULER_POLL_INVALID;
    }
    if (!source->pending) {
        return MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    }
    *out_work = source->work;
    return MEISO_RUNTIME_SCHEDULER_POLL_READY;
}

static void source_complete(
    void *context,
    const MeisoRuntimeSchedulerCompletion *completion
)
{
    MeisoCompositionControlSessionPrivateTxSource *source = context;

    if (source == NULL || completion == NULL || source->owner == NULL) {
        return;
    }
    source->owner->callback_active = true;
    if (source->pending && completion->source_id == source->source_id &&
        completion->ticket == source->work.ticket &&
        completion->kind == source->work.kind) {
        source->pending = false;
        memset(&source->work, 0, sizeof(source->work));
        if (!source->internal_catalog) {
            source->completion = *completion;
            source->completion_pending = true;
        }
    }
    source->owner->callback_active = false;
}

static void source_boundary(
    void *context,
    MeisoRuntimeSchedulerBoundary boundary,
    uint64_t catalog_generation
)
{
    MeisoCompositionControlSessionPrivateTxSource *source = context;

    if (source == NULL || source->owner == NULL) {
        return;
    }
    source->owner->callback_active = true;
    clear_source(source);
    if (source->internal_catalog) {
        if (source->owner->boundary_serial == UINT64_MAX) {
            source->owner->terminal = true;
            source->owner->phase =
                MEISO_COMPOSITION_CONTROL_SESSION_PHASE_TERMINAL;
        } else {
            source->owner->boundary_serial++;
            source->owner->last_boundary = boundary;
            source->owner->last_boundary_generation = catalog_generation;
        }
    }
    if (boundary == MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL) {
        source->owner->terminal = true;
        source->owner->phase =
            MEISO_COMPOSITION_CONTROL_SESSION_PHASE_TERMINAL;
    } else if (!source->owner->terminal && boundary ==
               MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED) {
        source->owner->phase =
            MEISO_COMPOSITION_CONTROL_SESSION_PHASE_REBIND;
    }
    source->owner->callback_active = false;
}

bool meiso_composition_control_session_private_queue_internal_catalog(MeisoCompositionControlSession *control_session)
{
    MeisoCompositionControlSessionPrivateTxSource *source;

    if (control_session->next_ticket == UINT64_MAX) {
        return false;
    }
    source = &control_session->sources[control_session->catalog_source_index];
    if (source->pending || source->completion_pending) {
        return false;
    }
    memset(&source->work, 0, sizeof(source->work));
    source->work.ticket = control_session->next_ticket + 1U;
    source->work.kind = source->work_kind;
    if (control_session->local_role == MEISO_SESSION_ROLE_EDGE) {
        source->work.value.catalog_offer.generation =
            control_session->catalog_generation;
        source->work.value.catalog_offer.endpoints = control_session->endpoints;
        source->work.value.catalog_offer.endpoint_count =
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT;
        source->work.value.catalog_offer.routes = NULL;
        source->work.value.catalog_offer.route_count =
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT;
        source->work.value.catalog_offer.routes_v4 = control_session->routes;
    }
    control_session->next_ticket++;
    source->pending = true;
    return true;
}

static bool runtime_config(
    MeisoCompositionControlSession *control_session,
    MeisoRuntimeV4Config *out_config
)
{
    MeisoRuntimeV4Config config;

    if (control_session == NULL || out_config == NULL) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.base.session = control_session->session;
    config.base.max_endpoints = MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_ENDPOINTS;
    config.base.max_endpoint_history =
        MEISO_COMPOSITION_PROFILE_MAX_ENDPOINT_HISTORY;
    config.base.max_routes = MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_ROUTES;
    config.base.max_capability_bytes = MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_CAPABILITY_BYTES;
    config.base.max_desired_profile_bytes_per_endpoint =
        MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_DESIRED_PROFILE_BYTES;
    config.base.max_actual_profile_bytes_per_endpoint =
        MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_ACTUAL_PROFILE_BYTES;
    config.max_blob_transfer_bytes = MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_BLOB_TRANSFER_BYTES;
    *out_config = config;
    return true;
}

bool meiso_composition_control_session_private_activate_runtime(MeisoCompositionControlSession *control_session)
{
    MeisoRuntimeV4Config runtime_configuration;
    MeisoRuntimeSchedulerConfig scheduler_configuration;
    MeisoRuntimeSchedulerSourceConfig source_configuration;
    MeisoRuntimeSchedulerStatus scheduler_status;
    size_t required;
    size_t index;
    uint16_t controls = 0U;
    uint16_t data = 0U;

    if (!runtime_config(control_session, &runtime_configuration)) {
        return false;
    }
    required = meiso_runtime_v4_memory_required(&runtime_configuration);
    if (required == 0U || required > control_session->runtime_reserve_bytes ||
        meiso_runtime_v4_init(
            meiso_composition_control_session_private_arena_at(control_session, control_session->runtime_offset),
            control_session->runtime_reserve_bytes,
            &runtime_configuration,
            &control_session->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return false;
    }
    control_session->runtime_bytes = required;
    control_session->runtime_present = true;
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        if (meiso_composition_control_session_private_kind_is_data(control_session->sources[index].work_kind)) {
            ++data;
        } else {
            ++controls;
        }
    }
    memset(&scheduler_configuration, 0, sizeof(scheduler_configuration));
    scheduler_configuration.runtime = control_session->runtime;
    scheduler_configuration.session = control_session->session;
    scheduler_configuration.max_control_sources = controls;
    scheduler_configuration.max_data_sources = data;
    required = meiso_runtime_scheduler_memory_required(
        &scheduler_configuration
    );
    if (required == 0U || required > control_session->scheduler_reserve_bytes ||
        meiso_runtime_scheduler_init(
            meiso_composition_control_session_private_arena_at(control_session, control_session->scheduler_offset),
            control_session->scheduler_reserve_bytes,
            &scheduler_configuration,
            &control_session->scheduler
        ) != MEISO_RUNTIME_SCHEDULER_INIT_OK) {
        return false;
    }
    control_session->scheduler_bytes = required;
    control_session->scheduler_present = true;
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        MeisoCompositionControlSessionPrivateTxSource *source = &control_session->sources[index];
        memset(&source_configuration, 0, sizeof(source_configuration));
        source_configuration.work_kind = source->work_kind;
        source_configuration.poll = source_poll;
        source_configuration.complete = source_complete;
        source_configuration.boundary = source_boundary;
        source_configuration.context = source;
        if (meiso_runtime_scheduler_register_source(
                control_session->scheduler, &source_configuration, &source->source_id
            ) != MEISO_RUNTIME_SCHEDULER_OK) {
            return false;
        }
    }
    scheduler_status = meiso_runtime_scheduler_seal(control_session->scheduler);
    if (scheduler_status != MEISO_RUNTIME_SCHEDULER_OK) {
        return false;
    }
    control_session->phase = MEISO_COMPOSITION_CONTROL_SESSION_PHASE_CATALOG;
    if (control_session->local_role == MEISO_SESSION_ROLE_EDGE &&
        !meiso_composition_control_session_private_queue_internal_catalog(control_session)) {
        return false;
    }
    return true;
}

bool meiso_composition_control_session_private_rebind_sources(MeisoCompositionControlSession *control_session)
{
    size_t index;
    MeisoRuntimeSchedulerStatus status;

    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        const MeisoCompositionControlSessionPrivateTxSource *source = &control_session->sources[index];
        if (meiso_composition_control_session_private_kind_has_route(source->work_kind)) {
            status = meiso_runtime_scheduler_bind_source_route(
                control_session->scheduler,
                source->source_id,
                route_key_for_index(control_session, source->route_index)
            );
            if (status != MEISO_RUNTIME_SCHEDULER_OK) {
                return false;
            }
        }
    }
    if (meiso_runtime_scheduler_finish_rebind(control_session->scheduler) !=
        MEISO_RUNTIME_SCHEDULER_OK) {
        return false;
    }
    control_session->phase = MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY;
    return true;
}

static bool endpoint_equal(
    const MeisoRuntimeEndpointDescriptor *lhs,
    const MeisoRuntimeEndpointDescriptor *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_endpoint_id_equal(lhs->id, rhs->id) &&
        lhs->incarnation == rhs->incarnation &&
        meiso_schema_ref_equal(
            lhs->capability_schema, rhs->capability_schema
        ) && lhs->capability.size == rhs->capability.size &&
        (lhs->capability.size == 0U ||
         (lhs->capability.data != NULL && rhs->capability.data != NULL &&
          memcmp(
              lhs->capability.data,
              rhs->capability.data,
              lhs->capability.size
          ) == 0));
}

static bool v4_route_equal(
    const MeisoRuntimeV4RouteDescriptor *lhs,
    const MeisoRuntimeV4RouteDescriptor *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_endpoint_id_equal(
            lhs->route.endpoint_id, rhs->route.endpoint_id
        ) && lhs->route.endpoint_incarnation ==
            rhs->route.endpoint_incarnation &&
        meiso_resource_descriptor_equal(
            &lhs->route.resource, &rhs->route.resource
        ) && lhs->route.facet == rhs->route.facet &&
        lhs->delivery == rhs->delivery;
}

bool meiso_composition_control_session_private_staged_catalog_exact(MeisoCompositionControlSession *control_session)
{
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeV4RouteDescriptor route;
    size_t index;

    if (!meiso_runtime_snapshot(control_session->runtime, &snapshot) ||
        !snapshot.staged_catalog_present ||
        !snapshot.catalog_decision_pending ||
        snapshot.staged_catalog_generation != control_session->catalog_generation ||
        snapshot.staged_endpoint_count !=
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT ||
        snapshot.staged_route_count !=
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT) {
        return false;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT; ++index) {
        if (meiso_runtime_staged_endpoint_at(
                control_session->runtime, index, &endpoint
            ) != MEISO_RUNTIME_OK ||
            !endpoint_equal(&endpoint, &control_session->endpoints[index])) {
            return false;
        }
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_PROFILE_ROUTE_COUNT; ++index) {
        if (meiso_runtime_v4_staged_route_at(
                control_session->runtime, index, &route
            ) != MEISO_RUNTIME_OK ||
            !v4_route_equal(&route, &control_session->routes[index])) {
            return false;
        }
    }
    return true;
}

MeisoCompositionControlSessionStatus meiso_composition_control_session_queue_work(
    MeisoCompositionControlSession *control_session,
    uint8_t source_index,
    const MeisoRuntimeSchedulerWork *work
)
{
    MeisoCompositionControlSessionPrivateTxSource *source;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey expected_route;

    if (!meiso_composition_control_session_private_valid(control_session) || work == NULL || source_index >=
            MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT ||
        meiso_composition_control_session_private_ranges_overlap(control_session, control_session->arena_size, work, sizeof(*work))) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    if (control_session->callback_active) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    if (control_session->terminal) {
        return MEISO_COMPOSITION_CONTROL_SESSION_TERMINAL;
    }
    if (control_session->phase ==
        MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION) {
        return MEISO_COMPOSITION_CONTROL_SESSION_RX_OBLIGATION;
    }
    if (!control_session->scheduler_present ||
        control_session->phase != MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    source = &control_session->sources[source_index];
    if (source->internal_catalog) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    if (source->pending || source->completion_pending) {
        return MEISO_COMPOSITION_CONTROL_SESSION_SOURCE_BUSY;
    }
    if (work->ticket != 0U || work->kind != source->work_kind) {
        return MEISO_COMPOSITION_CONTROL_SESSION_SOURCE_KIND_MISMATCH;
    }
    if (!work_route(work, &actual_route)) {
        return MEISO_COMPOSITION_CONTROL_SESSION_ROUTE_MISMATCH;
    }
    expected_route = route_key_for_index(control_session, source->route_index);
    if (!route_equal(actual_route, expected_route)) {
        return MEISO_COMPOSITION_CONTROL_SESSION_ROUTE_MISMATCH;
    }
    if (work_borrows_control_session_arena(control_session, work)) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    if (control_session->next_ticket == UINT64_MAX) {
        return MEISO_COMPOSITION_CONTROL_SESSION_COUNTER_EXHAUSTED;
    }
    source->work = *work;
    source->work.ticket = control_session->next_ticket + 1U;
    control_session->next_ticket++;
    source->pending = true;
    return MEISO_COMPOSITION_CONTROL_SESSION_OK;
}

MeisoCompositionControlSessionStatus meiso_composition_control_session_take_completion(
    MeisoCompositionControlSession *control_session,
    uint8_t source_index,
    MeisoRuntimeSchedulerCompletion *out_completion
)
{
    MeisoCompositionControlSessionPrivateTxSource *source;

    if (!meiso_composition_control_session_private_valid(control_session) || out_completion == NULL || source_index >=
            MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT ||
        meiso_composition_control_session_private_ranges_overlap(
            control_session, control_session->arena_size, out_completion, sizeof(*out_completion)
        )) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    if (control_session->callback_active) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    if (control_session->terminal) {
        return MEISO_COMPOSITION_CONTROL_SESSION_TERMINAL;
    }
    source = &control_session->sources[source_index];
    if (source->internal_catalog || !source->completion_pending) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    *out_completion = source->completion;
    memset(&source->completion, 0, sizeof(source->completion));
    source->completion_pending = false;
    return MEISO_COMPOSITION_CONTROL_SESSION_OK;
}

bool meiso_composition_control_session_route_key(
    const MeisoCompositionControlSession *control_session,
    uint8_t route_index,
    MeisoRuntimeRouteKey *out_route
)
{
    MeisoRuntimeSchedulerSnapshot scheduler_snapshot;

    if (!meiso_composition_control_session_private_valid(control_session) || control_session->callback_active || control_session->terminal ||
        out_route == NULL || route_index >=
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT ||
        meiso_composition_control_session_private_ranges_overlap(control_session, control_session->arena_size, out_route, sizeof(*out_route)) ||
        !control_session->scheduler_present || !meiso_runtime_scheduler_snapshot(
            control_session->scheduler, &scheduler_snapshot
        ) || (scheduler_snapshot.phase !=
                MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
              scheduler_snapshot.phase !=
                MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND) ||
        scheduler_snapshot.catalog_generation != control_session->catalog_generation) {
        return false;
    }
    *out_route = route_key_for_index(control_session, route_index);
    return true;
}
