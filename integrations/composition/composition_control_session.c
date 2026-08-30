#include "meiso_composition_control_session_private.h"

#include <limits.h>
#include <stdalign.h>
#include <string.h>

/* Arena layout, configuration validation, bootstrap, and installed-state validation. */

/*
 * Role-neutral Composition Profile control integration.
 *
 * This module owns the Session -> POSIX adapter -> Runtime -> Scheduler chain
 * shared by Host and Edge. Its SDK-facing CMake identity is
 * Meiso::CompositionControlSession. It does not decode Profile payloads or own
 * application/media policy.
 */
static bool add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool align_size(size_t value, size_t alignment, size_t *out)
{
    size_t remainder;
    size_t addition;

    if (out == NULL || alignment == 0U) {
        return false;
    }
    remainder = value % alignment;
    addition = remainder == 0U ? 0U : alignment - remainder;
    return add_size(value, addition, out);
}

bool meiso_composition_control_session_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    uintptr_t lhs_start;
    uintptr_t rhs_start;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    lhs_start = (uintptr_t)lhs;
    rhs_start = (uintptr_t)rhs;
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static bool kind_valid(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind >= MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind <= MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
}

bool meiso_composition_control_session_private_kind_is_data(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE ||
        kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
}

bool meiso_composition_control_session_private_kind_has_route(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT &&
        kind != MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT;
}

static bool owner_is_local(MeisoOwnerRole owner, MeisoSessionRole role)
{
    return (owner == MEISO_OWNER_ROLE_HOST &&
            role == MEISO_SESSION_ROLE_HOST) ||
        (owner == MEISO_OWNER_ROLE_EDGE &&
         role == MEISO_SESSION_ROLE_EDGE);
}

bool meiso_composition_control_session_private_source_route_semantics_valid(
    MeisoSessionRole role,
    MeisoRuntimeSchedulerWorkKind kind,
    const MeisoCompositionProfileRouteDescriptor *route
)
{
    const MeisoResourceDescriptor *resource;
    bool local_owner;

    if (route == NULL) {
        return false;
    }
    resource = &route->route.resource;
    local_owner = owner_is_local(resource->owner, role);
    switch (kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        return resource->kind == MEISO_RESOURCE_KIND_STATE && local_owner;
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        return resource->kind == MEISO_RESOURCE_KIND_STATE && !local_owner;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        return resource->kind == MEISO_RESOURCE_KIND_STREAM && local_owner &&
            route->delivery == MEISO_RUNTIME_DELIVERY_INLINE;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN:
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT:
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT:
        return role == MEISO_SESSION_ROLE_HOST && local_owner &&
            resource->kind == MEISO_RESOURCE_KIND_BLOB &&
            route->route.facet == MEISO_RUNTIME_FACET_IMMUTABLE;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        return role == MEISO_SESSION_ROLE_EDGE && !local_owner &&
            resource->kind == MEISO_RESOURCE_KIND_BLOB &&
            route->route.facet == MEISO_RUNTIME_FACET_IMMUTABLE;
    default:
        return false;
    }
}

static bool session_config_from_control_session(
    const MeisoCompositionControlSessionConfig *config,
    MeisoSessionConfig *out_config
)
{
    MeisoSessionConfig candidate;

    if (config == NULL || out_config == NULL) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.local_role = config->local_role;
    candidate.local_peer_id = config->local_peer_id;
    candidate.local_incarnation = config->local_incarnation;
    candidate.local_nonce = config->local_nonce;
    candidate.expected_peer_id = config->expected_peer_id;
    candidate.protocol = config->protocol;
    if (meiso_composition_profile_configure_session(&candidate) !=
        MEISO_COMPOSITION_PROFILE_OK) {
        return false;
    }
    *out_config = candidate;
    return true;
}

static bool catalog_and_source_semantics_valid(
    const MeisoCompositionControlSessionConfig *config
)
{
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT
    ];
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    uint8_t capability[
        MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES
    ];
    size_t index;

    if (config == NULL || meiso_composition_profile_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){capability, sizeof(capability)},
            endpoints,
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            routes,
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) != MEISO_COMPOSITION_PROFILE_OK) {
        return false;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        if (meiso_composition_control_session_private_kind_has_route(config->sources[index].work_kind) &&
            !meiso_composition_control_session_private_source_route_semantics_valid(
                config->local_role,
                config->sources[index].work_kind,
                &routes[config->sources[index].route_index]
            )) {
            return false;
        }
    }
    return true;
}

static bool source_configs_shape_valid(
    const MeisoCompositionControlSessionConfig *config,
    uint8_t *out_catalog_index,
    uint16_t *out_control_count,
    uint16_t *out_data_count
)
{
    size_t index;
    size_t other;
    uint8_t catalog_index = MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE;
    uint16_t controls = 0U;
    uint16_t data = 0U;
    MeisoRuntimeSchedulerWorkKind expected_catalog;

    if (config == NULL || out_catalog_index == NULL ||
        out_control_count == NULL || out_data_count == NULL) {
        return false;
    }
    expected_catalog = config->local_role == MEISO_SESSION_ROLE_EDGE ?
        MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER :
        MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT;
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        const MeisoCompositionControlSessionSourceConfig *source =
            &config->sources[index];
        if (!kind_valid(source->work_kind)) {
            return false;
        }
        if (source->work_kind == MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT ||
            (source->work_kind == MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER ||
             source->work_kind ==
                MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT)) {
            if (source->work_kind != expected_catalog ||
                source->route_index != MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE ||
                catalog_index != MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE) {
                return false;
            }
            catalog_index = (uint8_t)index;
        } else if (!meiso_composition_control_session_private_kind_has_route(source->work_kind) ||
                   source->route_index >=
                    MEISO_COMPOSITION_PROFILE_ROUTE_COUNT) {
            return false;
        }
        if (meiso_composition_control_session_private_kind_is_data(source->work_kind)) {
            if (data == UINT16_MAX) {
                return false;
            }
            ++data;
            for (other = 0U; other < index; ++other) {
                if (meiso_composition_control_session_private_kind_is_data(config->sources[other].work_kind) &&
                    config->sources[other].route_index ==
                        source->route_index) {
                    return false;
                }
            }
        } else {
            if (controls == UINT16_MAX) {
                return false;
            }
            ++controls;
        }
    }
    if (catalog_index == MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE ||
        controls == 0U) {
        return false;
    }
    *out_catalog_index = catalog_index;
    *out_control_count = controls;
    *out_data_count = data;
    return true;
}

static bool calculate_layout(
    const MeisoCompositionControlSessionConfig *config,
    MeisoCompositionControlSessionPrivateArenaLayout *out_layout
)
{
    MeisoSessionConfig session_config;
    MeisoCompositionControlSessionPrivateArenaLayout layout;
    uint8_t catalog_index;
    uint16_t controls;
    uint16_t data;
    size_t cursor;
    size_t alignment = alignof(max_align_t);

    if (config == NULL || out_layout == NULL || config->connected_fd < 0 ||
        config->catalog_generation == 0U ||
        config->max_io_bytes_per_step == 0U ||
        config->adapter_reserve_bytes == 0U ||
        config->runtime_reserve_bytes == 0U ||
        config->scheduler_reserve_bytes == 0U ||
        !source_configs_shape_valid(
            config, &catalog_index, &controls, &data
        ) || !session_config_from_control_session(config, &session_config) ||
        !catalog_and_source_semantics_valid(config)) {
        return false;
    }
    (void)catalog_index;
    (void)controls;
    (void)data;
    memset(&layout, 0, sizeof(layout));
    layout.session_bytes = meiso_session_memory_required(&session_config);
    if (layout.session_bytes == 0U ||
        !align_size(sizeof(MeisoCompositionControlSession), alignment, &cursor)) {
        return false;
    }
    layout.session_offset = cursor;
    if (!add_size(cursor, layout.session_bytes, &cursor) ||
        !align_size(cursor, alignment, &cursor)) {
        return false;
    }
    layout.adapter_offset = cursor;
    if (!add_size(cursor, config->adapter_reserve_bytes, &cursor) ||
        !align_size(cursor, alignment, &cursor)) {
        return false;
    }
    layout.runtime_offset = cursor;
    if (!add_size(cursor, config->runtime_reserve_bytes, &cursor) ||
        !align_size(cursor, alignment, &cursor)) {
        return false;
    }
    layout.scheduler_offset = cursor;
    if (!add_size(cursor, config->scheduler_reserve_bytes, &cursor)) {
        return false;
    }
    layout.required = cursor;
    *out_layout = layout;
    return true;
}

void *meiso_composition_control_session_private_arena_at(MeisoCompositionControlSession *control_session, size_t offset)
{
    return (uint8_t *)control_session + offset;
}

static const void *arena_at_const(
    const MeisoCompositionControlSession *control_session,
    size_t offset
)
{
    return (const uint8_t *)control_session + offset;
}

bool meiso_composition_control_session_private_valid(const MeisoCompositionControlSession *control_session)
{
    size_t end;

    return control_session != NULL && control_session->magic == MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_MAGIC &&
        control_session->arena_size >= sizeof(*control_session) && control_session->session != NULL &&
        control_session->adapter != NULL && control_session->session_bytes != 0U &&
        control_session->adapter_reserve_bytes != 0U &&
        control_session->runtime_reserve_bytes != 0U &&
        control_session->scheduler_reserve_bytes != 0U &&
        add_size(control_session->session_offset, control_session->session_bytes, &end) &&
        end <= control_session->arena_size &&
        add_size(control_session->adapter_offset, control_session->adapter_reserve_bytes, &end) &&
        end <= control_session->arena_size &&
        add_size(control_session->runtime_offset, control_session->runtime_reserve_bytes, &end) &&
        end <= control_session->arena_size &&
        add_size(
            control_session->scheduler_offset, control_session->scheduler_reserve_bytes, &end
        ) && end <= control_session->arena_size &&
        control_session->session == arena_at_const(control_session, control_session->session_offset) &&
        control_session->adapter == arena_at_const(control_session, control_session->adapter_offset) &&
        (!control_session->runtime_present ||
         control_session->runtime == arena_at_const(control_session, control_session->runtime_offset)) &&
        (!control_session->scheduler_present ||
         control_session->scheduler == arena_at_const(control_session, control_session->scheduler_offset));
}

size_t meiso_composition_control_session_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_composition_control_session_memory_required(
    const MeisoCompositionControlSessionConfig *config
)
{
    MeisoCompositionControlSessionPrivateArenaLayout layout;

    return calculate_layout(config, &layout) ? layout.required : 0U;
}

MeisoCompositionControlSessionInitStatus meiso_composition_control_session_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionControlSessionConfig *config,
    MeisoCompositionControlSession **out_session
)
{
    MeisoCompositionControlSessionPrivateArenaLayout layout;
    MeisoSessionConfig session_configuration;
    MeisoPosixTcpConfig adapter_configuration;
    MeisoCompositionControlSession *control_session;
    size_t adapter_required;
    size_t index;
    uint8_t catalog_index;
    uint16_t controls;
    uint16_t data;

    if (out_session == NULL || config == NULL ||
        meiso_composition_control_session_private_ranges_overlap(config, sizeof(*config), out_session, sizeof(*out_session)) ||
        (arena != NULL && meiso_composition_control_session_private_ranges_overlap(
            arena, arena_size, out_session, sizeof(*out_session)
        )) || (arena != NULL && meiso_composition_control_session_private_ranges_overlap(
            arena, arena_size, config, sizeof(*config)
        ))) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_INVALID_ARGUMENT;
    }
    *out_session = NULL;
    if (!calculate_layout(config, &layout) ||
        !session_config_from_control_session(config, &session_configuration) ||
        !source_configs_shape_valid(
            config, &catalog_index, &controls, &data
        )) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_INVALID_CONFIG;
    }
    (void)controls;
    (void)data;
    if (arena == NULL || (uintptr_t)arena %
            meiso_composition_control_session_alignment_required() != 0U) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < layout.required) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_ARENA_TOO_SMALL;
    }
    memset(arena, 0, layout.required);
    control_session = arena;
    control_session->arena_size = layout.required;
    control_session->session_offset = layout.session_offset;
    control_session->session_bytes = layout.session_bytes;
    control_session->adapter_offset = layout.adapter_offset;
    control_session->adapter_reserve_bytes = config->adapter_reserve_bytes;
    control_session->runtime_offset = layout.runtime_offset;
    control_session->runtime_reserve_bytes = config->runtime_reserve_bytes;
    control_session->scheduler_offset = layout.scheduler_offset;
    control_session->scheduler_reserve_bytes = config->scheduler_reserve_bytes;
    control_session->local_role = config->local_role;
    control_session->catalog_generation = config->catalog_generation;
    control_session->catalog_source_index = catalog_index;
    control_session->phase = MEISO_COMPOSITION_CONTROL_SESSION_PHASE_HANDSHAKE;
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        control_session->sources[index].owner = control_session;
        control_session->sources[index].work_kind = config->sources[index].work_kind;
        control_session->sources[index].route_index = config->sources[index].route_index;
        control_session->sources[index].source_index = (uint8_t)index;
        control_session->sources[index].internal_catalog = index == catalog_index;
    }
    if (meiso_composition_profile_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){
                control_session->dynamic_capability, sizeof(control_session->dynamic_capability)
            },
            control_session->endpoints,
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            control_session->routes,
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) != MEISO_COMPOSITION_PROFILE_OK) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_INVALID_CONFIG;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        if (meiso_composition_control_session_private_kind_has_route(control_session->sources[index].work_kind) &&
            !meiso_composition_control_session_private_source_route_semantics_valid(
                control_session->local_role,
                control_session->sources[index].work_kind,
                &control_session->routes[control_session->sources[index].route_index]
            )) {
            return MEISO_COMPOSITION_CONTROL_SESSION_INIT_INVALID_CONFIG;
        }
    }
    if (meiso_session_init(
            meiso_composition_control_session_private_arena_at(control_session, control_session->session_offset),
            control_session->session_bytes,
            &session_configuration,
            &control_session->session
        ) != MEISO_SESSION_INIT_OK ||
        meiso_session_start(control_session->session) != MEISO_SESSION_START_STARTED) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_INTERNAL_FAILURE;
    }
    memset(&adapter_configuration, 0, sizeof(adapter_configuration));
    adapter_configuration.connected_fd = config->connected_fd;
    adapter_configuration.session = control_session->session;
    adapter_configuration.max_frame_bytes =
        MEISO_COMPOSITION_PROFILE_MAX_FRAME_BYTES;
    adapter_configuration.max_io_bytes_per_step =
        config->max_io_bytes_per_step;
    adapter_required = meiso_posix_tcp_memory_required(&adapter_configuration);
    if (adapter_required == 0U ||
        adapter_required > control_session->adapter_reserve_bytes) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_ARENA_TOO_SMALL;
    }
    if (meiso_posix_tcp_init(
            meiso_composition_control_session_private_arena_at(control_session, control_session->adapter_offset),
            control_session->adapter_reserve_bytes,
            &adapter_configuration,
            &control_session->adapter
        ) != MEISO_POSIX_TCP_INIT_OK) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INIT_FD_ERROR;
    }
    control_session->adapter_bytes = adapter_required;
    control_session->magic = MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_MAGIC;
    *out_session = control_session;
    return MEISO_COMPOSITION_CONTROL_SESSION_INIT_OK;
}
