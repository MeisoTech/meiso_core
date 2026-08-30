#include "meiso_runtime_scheduler_private.h"

#include <limits.h>
#include <string.h>

/* Scheduler arena layout, source registry and route-binding lifecycle. */

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

bool meiso_runtime_scheduler_private_ranges_overlap(
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
    MeisoRuntimeSchedulerArenaLayout *out_layout
)
{
    MeisoRuntimeSchedulerArenaLayout value;
    size_t cursor = sizeof(MeisoRuntimeScheduler);
    size_t bytes;

    if (out_layout == NULL || max_control_sources == 0U ||
        max_data_sources == 0U ||
        (uint32_t)max_control_sources + max_data_sources > UINT16_MAX) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    if (!align_up(cursor, _Alignof(MeisoRuntimeSchedulerSource), &cursor) ||
        !checked_multiply(
            max_control_sources, sizeof(MeisoRuntimeSchedulerSource), &bytes
        )) {
        return false;
    }
    value.control_offset = cursor;
    if (!checked_add(cursor, bytes, &cursor) ||
        !align_up(cursor, _Alignof(MeisoRuntimeSchedulerSource), &cursor) ||
        !checked_multiply(max_data_sources, sizeof(MeisoRuntimeSchedulerSource), &bytes)) {
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

MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_control_sources(MeisoRuntimeScheduler *scheduler)
{
    return (MeisoRuntimeSchedulerSource *)(void *)(
        (uint8_t *)(void *)scheduler + scheduler->control_offset
    );
}

const MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_control_sources_const(
    const MeisoRuntimeScheduler *scheduler
)
{
    return (const MeisoRuntimeSchedulerSource *)(const void *)(
        (const uint8_t *)(const void *)scheduler + scheduler->control_offset
    );
}

MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_data_sources(MeisoRuntimeScheduler *scheduler)
{
    return (MeisoRuntimeSchedulerSource *)(void *)(
        (uint8_t *)(void *)scheduler + scheduler->data_offset
    );
}

const MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_data_sources_const(
    const MeisoRuntimeScheduler *scheduler
)
{
    return (const MeisoRuntimeSchedulerSource *)(const void *)(
        (const uint8_t *)(const void *)scheduler + scheduler->data_offset
    );
}

bool meiso_runtime_scheduler_private_layout_valid(const MeisoRuntimeScheduler *scheduler)
{
    MeisoRuntimeSchedulerArenaLayout layout;

    return scheduler != NULL && scheduler->magic == MEISO_RUNTIME_SCHEDULER_MAGIC &&
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
            MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH &&
        scheduler->wire_version >= MEISO_RUNTIME_WIRE_VERSION_V2 &&
        scheduler->wire_version <= MEISO_RUNTIME_WIRE_VERSION_V4;
}

bool meiso_runtime_scheduler_private_output_overlaps(
    const MeisoRuntimeScheduler *scheduler,
    const void *output,
    size_t output_size
)
{
    return scheduler != NULL && meiso_runtime_scheduler_private_ranges_overlap(
        scheduler, scheduler->arena_size, output, output_size
    );
}

bool meiso_runtime_scheduler_private_route_equal(
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

bool meiso_runtime_scheduler_private_route_valid(MeisoRuntimeRouteKey route)
{
    return route.catalog_generation != 0U &&
        meiso_endpoint_id_valid(route.endpoint_id) &&
        route.endpoint_incarnation != 0U &&
        meiso_resource_id_valid(route.resource_id) &&
        meiso_runtime_facet_valid(route.facet);
}

bool meiso_runtime_scheduler_private_session_runtime_ready(
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

MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_source_for_id(
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
        return &meiso_runtime_scheduler_private_control_sources(scheduler)[index];
    }
    index -= scheduler->max_control_sources;
    if (index < scheduler->data_source_count) {
        return &meiso_runtime_scheduler_private_data_sources(scheduler)[index];
    }
    return NULL;
}

void meiso_runtime_scheduler_private_clear_data_round(MeisoRuntimeScheduler *scheduler)
{
    MeisoRuntimeSchedulerSource *sources = meiso_runtime_scheduler_private_data_sources(scheduler);
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

void meiso_runtime_scheduler_private_clear_route_bindings(MeisoRuntimeScheduler *scheduler)
{
    MeisoRuntimeSchedulerSource *controls = meiso_runtime_scheduler_private_control_sources(scheduler);
    MeisoRuntimeSchedulerSource *data = meiso_runtime_scheduler_private_data_sources(scheduler);
    uint16_t index;

    meiso_runtime_scheduler_private_clear_data_round(scheduler);
    for (index = 0U; index < scheduler->control_source_count; ++index) {
        controls[index].route_bound = false;
        memset(&controls[index].route, 0, sizeof(controls[index].route));
    }
    for (index = 0U; index < scheduler->data_source_count; ++index) {
        data[index].route_bound = false;
        memset(&data[index].route, 0, sizeof(data[index].route));
    }
}
size_t meiso_runtime_scheduler_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_runtime_scheduler_memory_required(
    const MeisoRuntimeSchedulerConfig *config
)
{
    MeisoRuntimeSchedulerArenaLayout layout;
    MeisoRuntimeSnapshot runtime_snapshot;
    MeisoSessionSnapshot session_snapshot;

    if (config == NULL || !calculate_layout(
            config->max_control_sources,
            config->max_data_sources,
            &layout
        ) || !meiso_runtime_scheduler_private_session_runtime_ready(
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
    MeisoRuntimeSchedulerArenaLayout layout;
    MeisoRuntimeSnapshot runtime_snapshot;
    MeisoRuntimeBlobSnapshot blob_snapshot;
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeScheduler *scheduler;

    if (config == NULL || out_scheduler == NULL || meiso_runtime_scheduler_private_ranges_overlap(
            config, sizeof(*config), out_scheduler, sizeof(*out_scheduler)
        ) || (arena != NULL && meiso_runtime_scheduler_private_ranges_overlap(
            arena, arena_size, config, sizeof(*config)
        )) || (arena != NULL && meiso_runtime_scheduler_private_ranges_overlap(
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
    if (!meiso_runtime_scheduler_private_session_runtime_ready(
            config->runtime,
            config->session,
            &runtime_snapshot,
            &session_snapshot
        ) || !meiso_runtime_blob_snapshot(
            config->runtime, &blob_snapshot
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
    scheduler->magic = MEISO_RUNTIME_SCHEDULER_MAGIC;
    scheduler->arena_size = layout.required;
    scheduler->runtime = config->runtime;
    scheduler->session = config->session;
    scheduler->session_id = session_snapshot.session_id;
    scheduler->control_offset = layout.control_offset;
    scheduler->data_offset = layout.data_offset;
    scheduler->max_control_sources = config->max_control_sources;
    scheduler->max_data_sources = config->max_data_sources;
    scheduler->local_role = session_snapshot.local_role;
    scheduler->wire_version = blob_snapshot.wire_version;
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
    MeisoRuntimeSchedulerSource *source;
    uint16_t index;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler) || config == NULL ||
        out_source_id == NULL || meiso_runtime_scheduler_private_output_overlaps(
            scheduler, out_source_id, sizeof(*out_source_id)
        ) || meiso_runtime_scheduler_private_ranges_overlap(
            scheduler, scheduler->arena_size, config, sizeof(*config)
        ) || meiso_runtime_scheduler_private_ranges_overlap(
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
    if (!meiso_runtime_scheduler_private_work_kind_valid(config->work_kind) || config->poll == NULL ||
        config->complete == NULL || config->boundary == NULL ||
        meiso_runtime_scheduler_private_ranges_overlap(
            scheduler, scheduler->arena_size, config->context, 1U
        )) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (meiso_runtime_scheduler_private_class_for_work_kind(config->work_kind) == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL) {
        if (scheduler->control_source_count ==
            scheduler->max_control_sources) {
            return MEISO_RUNTIME_SCHEDULER_CAPACITY;
        }
        index = scheduler->control_source_count++;
        source = &meiso_runtime_scheduler_private_control_sources(scheduler)[index];
        source->id = (MeisoRuntimeSchedulerSourceId)(index + 1U);
    } else {
        if (scheduler->data_source_count == scheduler->max_data_sources) {
            return MEISO_RUNTIME_SCHEDULER_CAPACITY;
        }
        index = scheduler->data_source_count++;
        source = &meiso_runtime_scheduler_private_data_sources(scheduler)[index];
        source->id = (MeisoRuntimeSchedulerSourceId)(
            scheduler->max_control_sources + index + 1U
        );
    }
    source->poll = config->poll;
    source->complete = config->complete;
    source->boundary = config->boundary;
    source->context = config->context;
    source->work_kind = config->work_kind;
    source->source_class = meiso_runtime_scheduler_private_class_for_work_kind(config->work_kind);
    source->route_binding_required = meiso_runtime_scheduler_private_work_kind_has_route(config->work_kind);
    *out_source_id = source->id;
    return MEISO_RUNTIME_SCHEDULER_OK;
}

MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_seal(
    MeisoRuntimeScheduler *scheduler
)
{
    if (!meiso_runtime_scheduler_private_layout_valid(scheduler)) {
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
    MeisoRuntimeSchedulerSource *source;
    const MeisoRuntimeSchedulerSource *data;
    MeisoRuntimeRouteDescriptor active_route;
    MeisoRuntimeV4RouteDescriptor active_route_v4;
    MeisoRuntimeStatus route_status;
    uint16_t index;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler) || !meiso_runtime_scheduler_private_route_valid(route)) {
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
    if (scheduler->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        route_status = meiso_runtime_v4_find_route(
            scheduler->runtime, route.resource_id, &active_route_v4
        );
        if (route_status == MEISO_RUNTIME_OK) {
            active_route = active_route_v4.route;
        }
    } else {
        route_status = meiso_runtime_find_route(
            scheduler->runtime, route.resource_id, &active_route
        );
    }
    if (route_status != MEISO_RUNTIME_OK || !meiso_endpoint_id_equal(
            active_route.endpoint_id, route.endpoint_id
        ) || active_route.endpoint_incarnation !=
            route.endpoint_incarnation || !meiso_resource_id_equal(
            active_route.resource.id, route.resource_id
        ) || active_route.facet != route.facet) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    source = meiso_runtime_scheduler_private_source_for_id(scheduler, source_id);
    if (source == NULL || !source->route_binding_required) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT;
    }
    if (source->source_class == MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_DATA) {
        data = meiso_runtime_scheduler_private_data_sources_const(scheduler);
        for (index = 0U; index < scheduler->data_source_count; ++index) {
            if (&data[index] != source && data[index].route_bound &&
                meiso_runtime_scheduler_private_route_equal(data[index].route, route)) {
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
    const MeisoRuntimeSchedulerSource *controls;
    const MeisoRuntimeSchedulerSource *data;
    uint16_t index;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler)) {
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
    controls = meiso_runtime_scheduler_private_control_sources_const(scheduler);
    data = meiso_runtime_scheduler_private_data_sources_const(scheduler);
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
