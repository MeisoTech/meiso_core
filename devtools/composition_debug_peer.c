#include "composition_debug_peer.h"

#include <errno.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>

#define DEBUG_PEER_MAGIC UINT64_C(0x4344504545525630) /* CDPEERV0 */

enum {
    PROFILE_MAX_ENDPOINTS = MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
    PROFILE_MAX_ROUTES = MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT,
    PROFILE_MAX_CAPABILITY_BYTES =
        MEISO_COMPOSITION_SESSION_V3_CAPABILITY_BYTES,
    PROFILE_MAX_DESIRED_PROFILE_BYTES =
        MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE,
    PROFILE_MAX_ACTUAL_PROFILE_BYTES =
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE,
    PROFILE_MAX_BLOB_TRANSFER_BYTES =
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE
};

typedef struct {
    MeisoCompositionDebugPeer *owner;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerCompletion completion;
    MeisoRuntimeSchedulerSourceId source_id;
    MeisoRuntimeSchedulerWorkKind work_kind;
    uint8_t route_index;
    uint8_t source_index;
    bool pending;
    bool completion_pending;
    bool internal_catalog;
} DebugTxSource;

struct MeisoCompositionDebugPeer {
    uint64_t magic;
    size_t arena_size;
    size_t session_offset;
    size_t session_bytes;
    size_t adapter_offset;
    size_t adapter_reserve_bytes;
    size_t adapter_bytes;
    size_t runtime_offset;
    size_t runtime_reserve_bytes;
    size_t runtime_bytes;
    size_t scheduler_offset;
    size_t scheduler_reserve_bytes;
    size_t scheduler_bytes;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
    MeisoRuntimeScheduler *scheduler;
    MeisoSessionRole local_role;
    uint64_t catalog_generation;
    uint64_t next_ticket;
    uint64_t boundary_serial;
    uint64_t last_boundary_generation;
    MeisoRuntimeSchedulerBoundary last_boundary;
    MeisoCompositionDebugPeerPhase phase;
    uint8_t catalog_source_index;
    bool runtime_present;
    bool scheduler_present;
    bool terminal;
    bool callback_active;
    uint8_t pump_cursor;
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    uint8_t dynamic_capability[
        MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES
    ];
    DebugTxSource sources[MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT];
};

typedef struct {
    size_t required;
    size_t session_offset;
    size_t session_bytes;
    size_t adapter_offset;
    size_t runtime_offset;
    size_t scheduler_offset;
} DebugPeerLayout;

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

static bool ranges_overlap(
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

static bool kind_is_data(MeisoRuntimeSchedulerWorkKind kind)
{
    return kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE ||
        kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
}

static bool kind_has_route(MeisoRuntimeSchedulerWorkKind kind)
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

static bool source_route_semantics_valid(
    MeisoSessionRole role,
    MeisoRuntimeSchedulerWorkKind kind,
    const MeisoCompositionSessionV3RouteDescriptor *route
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

static bool session_config_from_peer(
    const MeisoCompositionDebugPeerConfig *config,
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
    if (meiso_composition_session_v3_configure_session(&candidate) !=
        MEISO_COMPOSITION_SESSION_V3_OK) {
        return false;
    }
    *out_config = candidate;
    return true;
}

static bool catalog_and_source_semantics_valid(
    const MeisoCompositionDebugPeerConfig *config
)
{
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    uint8_t capability[
        MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES
    ];
    size_t index;

    if (config == NULL || meiso_composition_session_v3_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){capability, sizeof(capability)},
            endpoints,
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            routes,
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) != MEISO_COMPOSITION_SESSION_V3_OK) {
        return false;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        if (kind_has_route(config->sources[index].work_kind) &&
            !source_route_semantics_valid(
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
    const MeisoCompositionDebugPeerConfig *config,
    uint8_t *out_catalog_index,
    uint16_t *out_control_count,
    uint16_t *out_data_count
)
{
    size_t index;
    size_t other;
    uint8_t catalog_index = MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE;
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
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        const MeisoCompositionDebugPeerSourceConfig *source =
            &config->sources[index];
        if (!kind_valid(source->work_kind)) {
            return false;
        }
        if (source->work_kind == MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT ||
            (source->work_kind == MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER ||
             source->work_kind ==
                MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT)) {
            if (source->work_kind != expected_catalog ||
                source->route_index != MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE ||
                catalog_index != MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE) {
                return false;
            }
            catalog_index = (uint8_t)index;
        } else if (!kind_has_route(source->work_kind) ||
                   source->route_index >=
                    MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT) {
            return false;
        }
        if (kind_is_data(source->work_kind)) {
            if (data == UINT16_MAX) {
                return false;
            }
            ++data;
            for (other = 0U; other < index; ++other) {
                if (kind_is_data(config->sources[other].work_kind) &&
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
    if (catalog_index == MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE ||
        controls == 0U) {
        return false;
    }
    *out_catalog_index = catalog_index;
    *out_control_count = controls;
    *out_data_count = data;
    return true;
}

static bool calculate_layout(
    const MeisoCompositionDebugPeerConfig *config,
    DebugPeerLayout *out_layout
)
{
    MeisoSessionConfig session_config;
    DebugPeerLayout layout;
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
        ) || !session_config_from_peer(config, &session_config) ||
        !catalog_and_source_semantics_valid(config)) {
        return false;
    }
    (void)catalog_index;
    (void)controls;
    (void)data;
    memset(&layout, 0, sizeof(layout));
    layout.session_bytes = meiso_session_memory_required(&session_config);
    if (layout.session_bytes == 0U ||
        !align_size(sizeof(MeisoCompositionDebugPeer), alignment, &cursor)) {
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

static void *arena_at(MeisoCompositionDebugPeer *peer, size_t offset)
{
    return (uint8_t *)peer + offset;
}

static const void *arena_at_const(
    const MeisoCompositionDebugPeer *peer,
    size_t offset
)
{
    return (const uint8_t *)peer + offset;
}

static bool peer_valid(const MeisoCompositionDebugPeer *peer)
{
    size_t end;

    return peer != NULL && peer->magic == DEBUG_PEER_MAGIC &&
        peer->arena_size >= sizeof(*peer) && peer->session != NULL &&
        peer->adapter != NULL && peer->session_bytes != 0U &&
        peer->adapter_reserve_bytes != 0U &&
        peer->runtime_reserve_bytes != 0U &&
        peer->scheduler_reserve_bytes != 0U &&
        add_size(peer->session_offset, peer->session_bytes, &end) &&
        end <= peer->arena_size &&
        add_size(peer->adapter_offset, peer->adapter_reserve_bytes, &end) &&
        end <= peer->arena_size &&
        add_size(peer->runtime_offset, peer->runtime_reserve_bytes, &end) &&
        end <= peer->arena_size &&
        add_size(
            peer->scheduler_offset, peer->scheduler_reserve_bytes, &end
        ) && end <= peer->arena_size &&
        peer->session == arena_at_const(peer, peer->session_offset) &&
        peer->adapter == arena_at_const(peer, peer->adapter_offset) &&
        (!peer->runtime_present ||
         peer->runtime == arena_at_const(peer, peer->runtime_offset)) &&
        (!peer->scheduler_present ||
         peer->scheduler == arena_at_const(peer, peer->scheduler_offset));
}

static void clear_source(DebugTxSource *source)
{
    if (source != NULL) {
        memset(&source->work, 0, sizeof(source->work));
        memset(&source->completion, 0, sizeof(source->completion));
        source->pending = false;
        source->completion_pending = false;
    }
}

static void clear_all_sources(MeisoCompositionDebugPeer *peer)
{
    size_t index;

    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        clear_source(&peer->sources[index]);
    }
}

static void mark_terminal(MeisoCompositionDebugPeer *peer)
{
    if (peer != NULL && !peer->terminal) {
        clear_all_sources(peer);
        peer->terminal = true;
        peer->phase = MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL;
    }
}

static void terminate_peer(
    MeisoCompositionDebugPeer *peer,
    int error_number
)
{
    MeisoRuntimeSchedulerStepResult ignored;

    if (peer == NULL || peer->terminal) {
        return;
    }
    if (peer->adapter != NULL) {
        meiso_posix_tcp_abort(peer->adapter, error_number);
    }
    if (peer->scheduler_present) {
        (void)meiso_runtime_scheduler_step(peer->scheduler, &ignored);
    } else if (peer->runtime_present) {
        (void)meiso_runtime_refresh_bound_session(peer->runtime);
    }
    mark_terminal(peer);
}

static MeisoRuntimeRouteKey route_key_for_index(
    const MeisoCompositionDebugPeer *peer,
    uint8_t route_index
)
{
    const MeisoRuntimeRouteDescriptor *route =
        &peer->routes[route_index].route;
    MeisoRuntimeRouteKey key;

    memset(&key, 0, sizeof(key));
    key.catalog_generation = peer->catalog_generation;
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

static bool work_borrows_peer_arena(
    const MeisoCompositionDebugPeer *peer,
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
    return ranges_overlap(peer, peer->arena_size, first.data, first.size) ||
        ranges_overlap(peer, peer->arena_size, second.data, second.size);
}

static MeisoRuntimeSchedulerPollStatus source_poll(
    void *context,
    MeisoRuntimeSchedulerWork *out_work
)
{
    DebugTxSource *source = context;

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
    DebugTxSource *source = context;

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
    DebugTxSource *source = context;

    if (source == NULL || source->owner == NULL) {
        return;
    }
    source->owner->callback_active = true;
    clear_source(source);
    if (source->internal_catalog) {
        if (source->owner->boundary_serial == UINT64_MAX) {
            source->owner->terminal = true;
            source->owner->phase =
                MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL;
        } else {
            source->owner->boundary_serial++;
            source->owner->last_boundary = boundary;
            source->owner->last_boundary_generation = catalog_generation;
        }
    }
    if (boundary == MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL) {
        source->owner->terminal = true;
        source->owner->phase =
            MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL;
    } else if (!source->owner->terminal && boundary ==
               MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED) {
        source->owner->phase =
            MEISO_COMPOSITION_DEBUG_PEER_PHASE_REBIND;
    }
    source->owner->callback_active = false;
}

static bool queue_internal_catalog(MeisoCompositionDebugPeer *peer)
{
    DebugTxSource *source;

    if (peer->next_ticket == UINT64_MAX) {
        return false;
    }
    source = &peer->sources[peer->catalog_source_index];
    if (source->pending || source->completion_pending) {
        return false;
    }
    memset(&source->work, 0, sizeof(source->work));
    source->work.ticket = peer->next_ticket + 1U;
    source->work.kind = source->work_kind;
    if (peer->local_role == MEISO_SESSION_ROLE_EDGE) {
        source->work.value.catalog_offer.generation =
            peer->catalog_generation;
        source->work.value.catalog_offer.endpoints = peer->endpoints;
        source->work.value.catalog_offer.endpoint_count =
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT;
        source->work.value.catalog_offer.routes = NULL;
        source->work.value.catalog_offer.route_count =
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT;
        source->work.value.catalog_offer.routes_v4 = peer->routes;
    }
    peer->next_ticket++;
    source->pending = true;
    return true;
}

static bool runtime_config(
    MeisoCompositionDebugPeer *peer,
    MeisoRuntimeV4Config *out_config
)
{
    MeisoRuntimeV4Config config;

    if (peer == NULL || out_config == NULL) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.base.session = peer->session;
    config.base.max_endpoints = PROFILE_MAX_ENDPOINTS;
    config.base.max_endpoint_history =
        MEISO_COMPOSITION_SESSION_V3_MAX_ENDPOINT_HISTORY;
    config.base.max_routes = PROFILE_MAX_ROUTES;
    config.base.max_capability_bytes = PROFILE_MAX_CAPABILITY_BYTES;
    config.base.max_desired_profile_bytes_per_endpoint =
        PROFILE_MAX_DESIRED_PROFILE_BYTES;
    config.base.max_actual_profile_bytes_per_endpoint =
        PROFILE_MAX_ACTUAL_PROFILE_BYTES;
    config.max_blob_transfer_bytes = PROFILE_MAX_BLOB_TRANSFER_BYTES;
    *out_config = config;
    return true;
}

static bool activate_runtime(MeisoCompositionDebugPeer *peer)
{
    MeisoRuntimeV4Config runtime_configuration;
    MeisoRuntimeSchedulerConfig scheduler_configuration;
    MeisoRuntimeSchedulerSourceConfig source_configuration;
    MeisoRuntimeSchedulerStatus scheduler_status;
    size_t required;
    size_t index;
    uint16_t controls = 0U;
    uint16_t data = 0U;

    if (!runtime_config(peer, &runtime_configuration)) {
        return false;
    }
    required = meiso_runtime_v4_memory_required(&runtime_configuration);
    if (required == 0U || required > peer->runtime_reserve_bytes ||
        meiso_runtime_v4_init(
            arena_at(peer, peer->runtime_offset),
            peer->runtime_reserve_bytes,
            &runtime_configuration,
            &peer->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return false;
    }
    peer->runtime_bytes = required;
    peer->runtime_present = true;
    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        if (kind_is_data(peer->sources[index].work_kind)) {
            ++data;
        } else {
            ++controls;
        }
    }
    memset(&scheduler_configuration, 0, sizeof(scheduler_configuration));
    scheduler_configuration.runtime = peer->runtime;
    scheduler_configuration.session = peer->session;
    scheduler_configuration.max_control_sources = controls;
    scheduler_configuration.max_data_sources = data;
    required = meiso_runtime_scheduler_memory_required(
        &scheduler_configuration
    );
    if (required == 0U || required > peer->scheduler_reserve_bytes ||
        meiso_runtime_scheduler_init(
            arena_at(peer, peer->scheduler_offset),
            peer->scheduler_reserve_bytes,
            &scheduler_configuration,
            &peer->scheduler
        ) != MEISO_RUNTIME_SCHEDULER_INIT_OK) {
        return false;
    }
    peer->scheduler_bytes = required;
    peer->scheduler_present = true;
    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        DebugTxSource *source = &peer->sources[index];
        memset(&source_configuration, 0, sizeof(source_configuration));
        source_configuration.work_kind = source->work_kind;
        source_configuration.poll = source_poll;
        source_configuration.complete = source_complete;
        source_configuration.boundary = source_boundary;
        source_configuration.context = source;
        if (meiso_runtime_scheduler_register_source(
                peer->scheduler, &source_configuration, &source->source_id
            ) != MEISO_RUNTIME_SCHEDULER_OK) {
            return false;
        }
    }
    scheduler_status = meiso_runtime_scheduler_seal(peer->scheduler);
    if (scheduler_status != MEISO_RUNTIME_SCHEDULER_OK) {
        return false;
    }
    peer->phase = MEISO_COMPOSITION_DEBUG_PEER_PHASE_CATALOG;
    if (peer->local_role == MEISO_SESSION_ROLE_EDGE &&
        !queue_internal_catalog(peer)) {
        return false;
    }
    return true;
}

static bool rebind_sources(MeisoCompositionDebugPeer *peer)
{
    size_t index;
    MeisoRuntimeSchedulerStatus status;

    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        const DebugTxSource *source = &peer->sources[index];
        if (kind_has_route(source->work_kind)) {
            status = meiso_runtime_scheduler_bind_source_route(
                peer->scheduler,
                source->source_id,
                route_key_for_index(peer, source->route_index)
            );
            if (status != MEISO_RUNTIME_SCHEDULER_OK) {
                return false;
            }
        }
    }
    if (meiso_runtime_scheduler_finish_rebind(peer->scheduler) !=
        MEISO_RUNTIME_SCHEDULER_OK) {
        return false;
    }
    peer->phase = MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY;
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

static bool staged_catalog_exact(MeisoCompositionDebugPeer *peer)
{
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeV4RouteDescriptor route;
    size_t index;

    if (!meiso_runtime_snapshot(peer->runtime, &snapshot) ||
        !snapshot.staged_catalog_present ||
        !snapshot.catalog_decision_pending ||
        snapshot.staged_catalog_generation != peer->catalog_generation ||
        snapshot.staged_endpoint_count !=
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT ||
        snapshot.staged_route_count !=
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT) {
        return false;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT; ++index) {
        if (meiso_runtime_staged_endpoint_at(
                peer->runtime, index, &endpoint
            ) != MEISO_RUNTIME_OK ||
            !endpoint_equal(&endpoint, &peer->endpoints[index])) {
            return false;
        }
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT; ++index) {
        if (meiso_runtime_v4_staged_route_at(
                peer->runtime, index, &route
            ) != MEISO_RUNTIME_OK ||
            !v4_route_equal(&route, &peer->routes[index])) {
            return false;
        }
    }
    return true;
}

size_t meiso_composition_debug_peer_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_composition_debug_peer_memory_required(
    const MeisoCompositionDebugPeerConfig *config
)
{
    DebugPeerLayout layout;

    return calculate_layout(config, &layout) ? layout.required : 0U;
}

MeisoCompositionDebugPeerInitStatus meiso_composition_debug_peer_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionDebugPeerConfig *config,
    MeisoCompositionDebugPeer **out_peer
)
{
    DebugPeerLayout layout;
    MeisoSessionConfig session_configuration;
    MeisoPosixTcpConfig adapter_configuration;
    MeisoCompositionDebugPeer *peer;
    size_t adapter_required;
    size_t index;
    uint8_t catalog_index;
    uint16_t controls;
    uint16_t data;

    if (out_peer == NULL || config == NULL ||
        ranges_overlap(config, sizeof(*config), out_peer, sizeof(*out_peer)) ||
        (arena != NULL && ranges_overlap(
            arena, arena_size, out_peer, sizeof(*out_peer)
        )) || (arena != NULL && ranges_overlap(
            arena, arena_size, config, sizeof(*config)
        ))) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_ARGUMENT;
    }
    *out_peer = NULL;
    if (!calculate_layout(config, &layout) ||
        !session_config_from_peer(config, &session_configuration) ||
        !source_configs_shape_valid(
            config, &catalog_index, &controls, &data
        )) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_CONFIG;
    }
    (void)controls;
    (void)data;
    if (arena == NULL || (uintptr_t)arena %
            meiso_composition_debug_peer_alignment_required() != 0U) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < layout.required) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_ARENA_TOO_SMALL;
    }
    memset(arena, 0, layout.required);
    peer = arena;
    peer->arena_size = layout.required;
    peer->session_offset = layout.session_offset;
    peer->session_bytes = layout.session_bytes;
    peer->adapter_offset = layout.adapter_offset;
    peer->adapter_reserve_bytes = config->adapter_reserve_bytes;
    peer->runtime_offset = layout.runtime_offset;
    peer->runtime_reserve_bytes = config->runtime_reserve_bytes;
    peer->scheduler_offset = layout.scheduler_offset;
    peer->scheduler_reserve_bytes = config->scheduler_reserve_bytes;
    peer->local_role = config->local_role;
    peer->catalog_generation = config->catalog_generation;
    peer->catalog_source_index = catalog_index;
    peer->phase = MEISO_COMPOSITION_DEBUG_PEER_PHASE_HANDSHAKE;
    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        peer->sources[index].owner = peer;
        peer->sources[index].work_kind = config->sources[index].work_kind;
        peer->sources[index].route_index = config->sources[index].route_index;
        peer->sources[index].source_index = (uint8_t)index;
        peer->sources[index].internal_catalog = index == catalog_index;
    }
    if (meiso_composition_session_v3_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){
                peer->dynamic_capability, sizeof(peer->dynamic_capability)
            },
            peer->endpoints,
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            peer->routes,
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) != MEISO_COMPOSITION_SESSION_V3_OK) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_CONFIG;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        if (kind_has_route(peer->sources[index].work_kind) &&
            !source_route_semantics_valid(
                peer->local_role,
                peer->sources[index].work_kind,
                &peer->routes[peer->sources[index].route_index]
            )) {
            return MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_CONFIG;
        }
    }
    if (meiso_session_init(
            arena_at(peer, peer->session_offset),
            peer->session_bytes,
            &session_configuration,
            &peer->session
        ) != MEISO_SESSION_INIT_OK ||
        meiso_session_start(peer->session) != MEISO_SESSION_START_STARTED) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_INTERNAL_FAILURE;
    }
    memset(&adapter_configuration, 0, sizeof(adapter_configuration));
    adapter_configuration.connected_fd = config->connected_fd;
    adapter_configuration.session = peer->session;
    adapter_configuration.max_frame_bytes =
        MEISO_COMPOSITION_SESSION_V3_MAX_FRAME_BYTES;
    adapter_configuration.max_io_bytes_per_step =
        config->max_io_bytes_per_step;
    adapter_required = meiso_posix_tcp_memory_required(&adapter_configuration);
    if (adapter_required == 0U ||
        adapter_required > peer->adapter_reserve_bytes) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_ARENA_TOO_SMALL;
    }
    if (meiso_posix_tcp_init(
            arena_at(peer, peer->adapter_offset),
            peer->adapter_reserve_bytes,
            &adapter_configuration,
            &peer->adapter
        ) != MEISO_POSIX_TCP_INIT_OK) {
        return MEISO_COMPOSITION_DEBUG_PEER_INIT_FD_ERROR;
    }
    peer->adapter_bytes = adapter_required;
    peer->magic = DEBUG_PEER_MAGIC;
    *out_peer = peer;
    return MEISO_COMPOSITION_DEBUG_PEER_INIT_OK;
}

MeisoCompositionDebugPeerStatus meiso_composition_debug_peer_queue_work(
    MeisoCompositionDebugPeer *peer,
    uint8_t source_index,
    const MeisoRuntimeSchedulerWork *work
)
{
    DebugTxSource *source;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey expected_route;

    if (!peer_valid(peer) || work == NULL || source_index >=
            MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT ||
        ranges_overlap(peer, peer->arena_size, work, sizeof(*work))) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT;
    }
    if (peer->callback_active) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    if (peer->terminal) {
        return MEISO_COMPOSITION_DEBUG_PEER_TERMINAL;
    }
    if (peer->phase ==
        MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION) {
        return MEISO_COMPOSITION_DEBUG_PEER_RX_OBLIGATION;
    }
    if (!peer->scheduler_present ||
        peer->phase != MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    source = &peer->sources[source_index];
    if (source->internal_catalog) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    if (source->pending || source->completion_pending) {
        return MEISO_COMPOSITION_DEBUG_PEER_SOURCE_BUSY;
    }
    if (work->ticket != 0U || work->kind != source->work_kind) {
        return MEISO_COMPOSITION_DEBUG_PEER_SOURCE_KIND_MISMATCH;
    }
    if (!work_route(work, &actual_route)) {
        return MEISO_COMPOSITION_DEBUG_PEER_ROUTE_MISMATCH;
    }
    expected_route = route_key_for_index(peer, source->route_index);
    if (!route_equal(actual_route, expected_route)) {
        return MEISO_COMPOSITION_DEBUG_PEER_ROUTE_MISMATCH;
    }
    if (work_borrows_peer_arena(peer, work)) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT;
    }
    if (peer->next_ticket == UINT64_MAX) {
        return MEISO_COMPOSITION_DEBUG_PEER_COUNTER_EXHAUSTED;
    }
    source->work = *work;
    source->work.ticket = peer->next_ticket + 1U;
    peer->next_ticket++;
    source->pending = true;
    return MEISO_COMPOSITION_DEBUG_PEER_OK;
}

MeisoCompositionDebugPeerStatus meiso_composition_debug_peer_take_completion(
    MeisoCompositionDebugPeer *peer,
    uint8_t source_index,
    MeisoRuntimeSchedulerCompletion *out_completion
)
{
    DebugTxSource *source;

    if (!peer_valid(peer) || out_completion == NULL || source_index >=
            MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT ||
        ranges_overlap(
            peer, peer->arena_size, out_completion, sizeof(*out_completion)
        )) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT;
    }
    if (peer->callback_active) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    if (peer->terminal) {
        return MEISO_COMPOSITION_DEBUG_PEER_TERMINAL;
    }
    source = &peer->sources[source_index];
    if (source->internal_catalog || !source->completion_pending) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    *out_completion = source->completion;
    memset(&source->completion, 0, sizeof(source->completion));
    source->completion_pending = false;
    return MEISO_COMPOSITION_DEBUG_PEER_OK;
}

static bool session_is_ready(MeisoCompositionDebugPeer *peer)
{
    MeisoSessionSnapshot snapshot;

    return meiso_session_snapshot(peer->session, &snapshot) &&
        snapshot.state == MEISO_SESSION_STATE_READY &&
        snapshot.session_id_known;
}

static bool scheduler_has_obligation(
    MeisoCompositionDebugPeer *peer,
    MeisoRuntimeSchedulerRxObligation *out_obligation
)
{
    MeisoRuntimeSchedulerSnapshot snapshot;

    if (!peer->scheduler_present ||
        !meiso_runtime_scheduler_snapshot(peer->scheduler, &snapshot)) {
        if (out_obligation != NULL) {
            *out_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
        }
        return false;
    }
    if (out_obligation != NULL) {
        *out_obligation = snapshot.rx_obligation;
    }
    return snapshot.rx_obligation != MEISO_RUNTIME_SCHEDULER_RX_NONE;
}

static MeisoCompositionDebugPeerPumpStatus pump_read(
    MeisoCompositionDebugPeer *peer,
    MeisoCompositionDebugPeerPumpResult *out_result
)
{
    MeisoSessionRuntimeMessage message;
    MeisoRuntimeEvent event;
    MeisoCompositionDebugPeerPumpResult result;
    MeisoPosixTcpReadStatus read_status;
    MeisoRuntimeStatus runtime_status;
    MeisoRuntimeSchedulerRxObligation obligation;

    memset(&message, 0, sizeof(message));
    memset(&event, 0, sizeof(event));
    memset(&result, 0, sizeof(result));
    read_status = meiso_posix_tcp_on_readable(peer->adapter, &message);
    result.read_attempted = true;
    result.read_status = read_status;
    switch (read_status) {
    case MEISO_POSIX_TCP_READ_PROGRESS:
    case MEISO_POSIX_TCP_READ_SESSION_PROGRESS:
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS;
    case MEISO_POSIX_TCP_READ_WOULD_BLOCK:
    case MEISO_POSIX_TCP_READ_INTERRUPTED:
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK;
    case MEISO_POSIX_TCP_READ_RUNTIME:
        if (!peer->scheduler_present) {
            terminate_peer(peer, EPROTO);
            return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
        }
        runtime_status = meiso_runtime_scheduler_receive(
            peer->scheduler, &message, &event
        );
        if (runtime_status != MEISO_RUNTIME_OK) {
            terminate_peer(peer, EPROTO);
            return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
        }
        if (event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED) {
            if (peer->local_role != MEISO_SESSION_ROLE_HOST ||
                event.catalog_generation != peer->catalog_generation ||
                !staged_catalog_exact(peer) ||
                !queue_internal_catalog(peer)) {
                terminate_peer(peer, EPROTO);
                return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
            }
        } else if (event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
                   (peer->local_role != MEISO_SESSION_ROLE_EDGE ||
                    event.catalog_generation != peer->catalog_generation)) {
            terminate_peer(peer, EPROTO);
            return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
        }
        result.event = event;
        result.event_present = true;
        if (scheduler_has_obligation(peer, &obligation)) {
            result.rx_obligation = obligation;
            peer->phase =
                MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION;
        }
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT;
    case MEISO_POSIX_TCP_READ_PEER_CLOSED:
    case MEISO_POSIX_TCP_READ_SESSION_FAILED:
    case MEISO_POSIX_TCP_READ_TRANSPORT_LOST:
        terminate_peer(peer, EPROTO);
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL;
    case MEISO_POSIX_TCP_READ_INVALID_ARGUMENT:
    default:
        terminate_peer(peer, EPROTO);
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
    }
}

static MeisoCompositionDebugPeerPumpStatus pump_write(
    MeisoCompositionDebugPeer *peer,
    MeisoCompositionDebugPeerPumpResult *out_result
)
{
    MeisoCompositionDebugPeerPumpResult result;
    MeisoPosixTcpWriteStatus write_status;

    memset(&result, 0, sizeof(result));
    write_status = meiso_posix_tcp_on_writable(peer->adapter);
    result.write_attempted = true;
    result.write_status = write_status;
    switch (write_status) {
    case MEISO_POSIX_TCP_WRITE_PROGRESS:
    case MEISO_POSIX_TCP_WRITE_FRAME_SENT:
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS;
    case MEISO_POSIX_TCP_WRITE_WOULD_BLOCK:
    case MEISO_POSIX_TCP_WRITE_INTERRUPTED:
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK;
    case MEISO_POSIX_TCP_WRITE_IDLE:
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE;
    case MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST:
    case MEISO_POSIX_TCP_WRITE_SESSION_INVALID:
        terminate_peer(peer, EPROTO);
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL;
    case MEISO_POSIX_TCP_WRITE_INVALID_ARGUMENT:
    default:
        terminate_peer(peer, EPROTO);
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
    }
}

static MeisoCompositionDebugPeerPumpStatus pump_scheduler(
    MeisoCompositionDebugPeer *peer,
    MeisoCompositionDebugPeerPumpResult *out_result
)
{
    MeisoRuntimeSchedulerStepResult step;
    MeisoCompositionDebugPeerPumpResult result;
    MeisoRuntimeSchedulerStepStatus status;

    memset(&step, 0, sizeof(step));
    memset(&result, 0, sizeof(result));
    status = meiso_runtime_scheduler_step(peer->scheduler, &step);
    switch (status) {
    case MEISO_RUNTIME_SCHEDULER_STEP_SENT:
        result.scheduler_step = step;
        result.scheduler_step_present = true;
        for (uint8_t index = 0U;
             index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
            if (!peer->sources[index].internal_catalog &&
                peer->sources[index].source_id ==
                step.completion.source_id) {
                result.completed_source_index = index;
                result.completed_source_present = true;
                break;
            }
        }
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS;
    case MEISO_RUNTIME_SCHEDULER_STEP_IDLE:
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE;
    case MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY:
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_TX_BUSY;
    case MEISO_RUNTIME_SCHEDULER_STEP_REJECTED:
        result.scheduler_step = step;
        result.scheduler_step_present = true;
        for (uint8_t index = 0U;
             index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
            if (!peer->sources[index].internal_catalog &&
                peer->sources[index].source_id ==
                step.completion.source_id) {
                result.completed_source_index = index;
                result.completed_source_present = true;
                break;
            }
        }
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_SOURCE_REJECTED;
    case MEISO_RUNTIME_SCHEDULER_STEP_SOURCE_INVALID:
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_SOURCE_INVALID;
    case MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED:
        if (!rebind_sources(peer)) {
            terminate_peer(peer, EPROTO);
            return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
        }
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_REBIND_PROGRESS;
    case MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION:
        peer->phase = MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_RX_OBLIGATION;
    case MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL:
        terminate_peer(peer, EPROTO);
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL;
    case MEISO_RUNTIME_SCHEDULER_STEP_COUNTER_EXHAUSTED:
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_COUNTER_EXHAUSTED;
    case MEISO_RUNTIME_SCHEDULER_STEP_INVALID_ARGUMENT:
    case MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE:
    case MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT:
    default:
        terminate_peer(peer, EPROTO);
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
    }
}

MeisoCompositionDebugPeerPumpStatus meiso_composition_debug_peer_pump(
    MeisoCompositionDebugPeer *peer,
    bool readable,
    bool writable,
    MeisoCompositionDebugPeerPumpResult *out_result
)
{
    MeisoRuntimeSchedulerRxObligation obligation;
    uint8_t scan;

    if (!peer_valid(peer) || out_result == NULL || ranges_overlap(
            peer, peer->arena_size, out_result, sizeof(*out_result)
        )) {
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INVALID_ARGUMENT;
    }
    if (peer->callback_active) {
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INVALID_ARGUMENT;
    }
    if (peer->terminal) {
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL;
    }
    if (scheduler_has_obligation(peer, &obligation)) {
        peer->phase = MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_RX_OBLIGATION;
    }
    if (!peer->runtime_present && session_is_ready(peer)) {
        MeisoCompositionDebugPeerPumpResult result;
        memset(&result, 0, sizeof(result));
        if (!activate_runtime(peer)) {
            terminate_peer(peer, EPROTO);
            return MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE;
        }
        *out_result = result;
        return MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS;
    }
    for (scan = 0U; scan < 3U; ++scan) {
        const uint8_t candidate = (uint8_t)((peer->pump_cursor + scan) % 3U);
        if (candidate == 0U && readable &&
            meiso_posix_tcp_wants_read(peer->adapter)) {
            peer->pump_cursor = 1U;
            return pump_read(peer, out_result);
        }
        if (candidate == 1U && writable &&
            meiso_posix_tcp_wants_write(peer->adapter)) {
            peer->pump_cursor = 2U;
            return pump_write(peer, out_result);
        }
        if (candidate == 2U && peer->scheduler_present) {
            peer->pump_cursor = 0U;
            return pump_scheduler(peer, out_result);
        }
    }
    if (!peer->scheduler_present) {
        MeisoSessionSnapshot snapshot;
        if (!meiso_session_snapshot(peer->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            terminate_peer(peer, EPROTO);
            return MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL;
        }
    }
    {
        MeisoCompositionDebugPeerPumpResult result;
        memset(&result, 0, sizeof(result));
        *out_result = result;
    }
    return MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE;
}

static MeisoCompositionDebugPeerStatus runtime_result_status(
    MeisoCompositionDebugPeer *peer,
    MeisoRuntimeStatus status
)
{
    if (status == MEISO_RUNTIME_OK) {
        MeisoRuntimeSchedulerSnapshot snapshot;
        if (meiso_runtime_scheduler_snapshot(peer->scheduler, &snapshot) &&
            snapshot.rx_obligation == MEISO_RUNTIME_SCHEDULER_RX_NONE) {
            peer->phase = snapshot.phase ==
                MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED ?
                    MEISO_COMPOSITION_DEBUG_PEER_PHASE_REBIND :
                    MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY;
        }
        return MEISO_COMPOSITION_DEBUG_PEER_OK;
    }
    if (status == MEISO_RUNTIME_CORE_NOT_READY ||
        status == MEISO_RUNTIME_STALE_SESSION) {
        terminate_peer(peer, EPROTO);
        return MEISO_COMPOSITION_DEBUG_PEER_TERMINAL;
    }
    return status == MEISO_RUNTIME_INVALID_ARGUMENT ?
        MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT :
        MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
}

MeisoCompositionDebugPeerStatus
meiso_composition_debug_peer_commit_state_receive(
    MeisoCompositionDebugPeer *peer,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
)
{
    if (!peer_valid(peer) || offer == NULL || ranges_overlap(
            peer, peer->arena_size, offer, sizeof(*offer)
        )) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT;
    }
    if (peer->callback_active) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    if (peer->terminal) {
        return MEISO_COMPOSITION_DEBUG_PEER_TERMINAL;
    }
    if (!peer->scheduler_present || peer->phase !=
            MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    return runtime_result_status(
        peer,
        meiso_runtime_scheduler_commit_state_receive(
            peer->scheduler, route, offer, receive_status
        )
    );
}

MeisoCompositionDebugPeerStatus
meiso_composition_debug_peer_publish_blob_receive(
    MeisoCompositionDebugPeer *peer,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
)
{
    MeisoRuntimeStatus status;

    if (!peer_valid(peer) || expected_transfer == NULL || store == NULL ||
        verifier == NULL || out_report == NULL || ranges_overlap(
            peer, peer->arena_size, expected_transfer,
            sizeof(*expected_transfer)
        ) || ranges_overlap(
            peer, peer->arena_size, out_report, sizeof(*out_report)
        ) || ranges_overlap(peer, peer->arena_size, store, 1U) ||
        ranges_overlap(peer, peer->arena_size, verifier_context, 1U) ||
        ranges_overlap(
            expected_transfer, sizeof(*expected_transfer),
            out_report, sizeof(*out_report)
        ) || ranges_overlap(store, 1U, out_report, sizeof(*out_report)) ||
        ranges_overlap(
            verifier_context, 1U, out_report, sizeof(*out_report)
        )) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT;
    }
    if (peer->callback_active) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    if (peer->terminal) {
        return MEISO_COMPOSITION_DEBUG_PEER_TERMINAL;
    }
    if (!peer->scheduler_present || peer->phase !=
            MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION) {
        return MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    }
    peer->callback_active = true;
    status = meiso_runtime_scheduler_publish_blob_receive(
        peer->scheduler,
        expected_transfer,
        store,
        verifier,
        verifier_context,
        out_report
    );
    peer->callback_active = false;
    return runtime_result_status(peer, status);
}

bool meiso_composition_debug_peer_wants_read(
    const MeisoCompositionDebugPeer *peer
)
{
    return peer_valid(peer) && !peer->callback_active && !peer->terminal &&
        meiso_posix_tcp_wants_read(peer->adapter);
}

bool meiso_composition_debug_peer_wants_write(
    const MeisoCompositionDebugPeer *peer
)
{
    return peer_valid(peer) && !peer->callback_active && !peer->terminal &&
        meiso_posix_tcp_wants_write(peer->adapter);
}

bool meiso_composition_debug_peer_route_key(
    const MeisoCompositionDebugPeer *peer,
    uint8_t route_index,
    MeisoRuntimeRouteKey *out_route
)
{
    MeisoRuntimeSchedulerSnapshot scheduler_snapshot;

    if (!peer_valid(peer) || peer->callback_active || peer->terminal ||
        out_route == NULL || route_index >=
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT ||
        ranges_overlap(peer, peer->arena_size, out_route, sizeof(*out_route)) ||
        !peer->scheduler_present || !meiso_runtime_scheduler_snapshot(
            peer->scheduler, &scheduler_snapshot
        ) || (scheduler_snapshot.phase !=
                MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
              scheduler_snapshot.phase !=
                MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND) ||
        scheduler_snapshot.catalog_generation != peer->catalog_generation) {
        return false;
    }
    *out_route = route_key_for_index(peer, route_index);
    return true;
}

bool meiso_composition_debug_peer_snapshot(
    const MeisoCompositionDebugPeer *peer,
    MeisoCompositionDebugPeerSnapshot *out_snapshot
)
{
    MeisoCompositionDebugPeerSnapshot snapshot;
    size_t index;

    if (!peer_valid(peer) || peer->callback_active || out_snapshot == NULL || ranges_overlap(
            peer, peer->arena_size, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (!meiso_session_snapshot(peer->session, &snapshot.session)) {
        return false;
    }
    snapshot.phase = peer->phase;
    snapshot.catalog_generation = peer->catalog_generation;
    snapshot.next_ticket = peer->next_ticket;
    snapshot.boundary_serial = peer->boundary_serial;
    snapshot.last_boundary_generation = peer->last_boundary_generation;
    snapshot.last_boundary = peer->last_boundary;
    snapshot.arena_bytes = peer->arena_size;
    snapshot.session_bytes = peer->session_bytes;
    snapshot.adapter_reserve_bytes = peer->adapter_reserve_bytes;
    snapshot.adapter_bytes = peer->adapter_bytes;
    snapshot.runtime_reserve_bytes = peer->runtime_reserve_bytes;
    snapshot.runtime_bytes = peer->runtime_bytes;
    snapshot.scheduler_reserve_bytes = peer->scheduler_reserve_bytes;
    snapshot.scheduler_bytes = peer->scheduler_bytes;
    snapshot.runtime_present = peer->runtime_present;
    snapshot.scheduler_present = peer->scheduler_present;
    snapshot.terminal = peer->terminal;
    if (peer->runtime_present &&
        !meiso_runtime_snapshot(peer->runtime, &snapshot.runtime)) {
        return false;
    }
    if (peer->scheduler_present && !meiso_runtime_scheduler_snapshot(
            peer->scheduler, &snapshot.scheduler
        )) {
        return false;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT; ++index) {
        if (peer->sources[index].pending ||
            peer->sources[index].completion_pending) {
            ++snapshot.pending_source_count;
        }
    }
    *out_snapshot = snapshot;
    return true;
}

void meiso_composition_debug_peer_abort(
    MeisoCompositionDebugPeer *peer,
    int error_number
)
{
    MeisoRuntimeSchedulerStepResult ignored;

    if (!peer_valid(peer) || peer->terminal || peer->callback_active) {
        return;
    }
    meiso_posix_tcp_abort(peer->adapter, error_number);
    if (peer->scheduler_present) {
        (void)meiso_runtime_scheduler_step(peer->scheduler, &ignored);
    }
    mark_terminal(peer);
}
