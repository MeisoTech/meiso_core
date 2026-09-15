#include "meiso_composition_media_transport_private.h"

/* Lifecycle owns validated arena construction, borrowed views and teardown. */

#include <limits.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool resource_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool pointer_aligned(const void *pointer, size_t alignment)
{
    return pointer != NULL && alignment != 0U &&
           ((uintptr_t)pointer % alignment) == 0U;
}

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}
static bool active_route_mask_valid(uint8_t configured_mask)
{
    return configured_mask != 0U &&
           (configured_mask &
            (uint8_t)~MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK) == 0U;
}
static bool
inactive_route_capacity_valid(const MeisoCompositionMediaRouteConfig *route,
                              size_t route_index)
{
    const MeisoCompositionMediaSource expected_source =
        route_index == 0U ? MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW
                          : MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    const uint32_t max_access_unit_bytes =
        route_index == 0U
            ? MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES
            : MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES;
    return route->capability.source == expected_source &&
           route->capability.max_access_unit_bytes != 0U &&
           route->capability.max_access_unit_bytes <= max_access_unit_bytes &&
           route->capability.max_fragment_payload_bytes != 0U &&
           route->capability.max_fragment_payload_bytes <=
               MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE;
}

static bool route_config_valid(const MeisoCompositionMediaOpen *scope,
                               const MeisoCompositionMediaRouteConfig *route,
                               size_t route_index)
{
    uint8_t capability_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    uint8_t binding_wire[MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE];
    const MeisoEndpointId *expected_endpoint =
        route_index == 0U ? meiso_composition_render_media_endpoint()
                          : meiso_composition_camera_media_endpoint();
    const MeisoResourceDescriptor *resources;
    const MeisoCompositionMediaSource expected_source =
        route_index == 0U ? MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW
                          : MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    size_t resource_count = 0U;
    size_t wire_size = 0U;

    resources = route_index == 0U
                    ? meiso_composition_render_media_resources(&resource_count)
                    : meiso_composition_camera_media_resources(&resource_count);
    if (resources == NULL || resource_count != 3U ||
        expected_endpoint == NULL || route->binding.source != expected_source ||
        route->capability.source != expected_source ||
        !meiso_composition_media_transport_private_open_equal(
            scope,
            &(MeisoCompositionMediaOpen){route->binding.session_id,
                                         route->binding.catalog_generation}) ||
        !endpoint_equal(route->binding.endpoint_id, *expected_endpoint) ||
        !resource_equal(route->binding.resource_id, resources[2].id) ||
        route->binding.endpoint_incarnation == 0U ||
        route->binding.production_epoch == 0U ||
        route->binding.media_stream_epoch == 0U ||
        !meiso_session_nonce_valid(route->binding.channel_nonce) ||
        !meiso_composition_media_desired_supported(&route->capability,
                                                   &route->effective)) {
        return false;
    }
    if (meiso_composition_media_capability_encode(
            &route->capability,
            (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)},
            &wire_size) != MEISO_COMPOSITION_PROFILE_OK ||
        wire_size != sizeof(capability_wire) ||
        meiso_composition_media_binding_encode(
            &route->binding,
            (MeisoMutableBuffer){binding_wire, sizeof(binding_wire)},
            &wire_size) != MEISO_COMPOSITION_PROFILE_OK ||
        wire_size != sizeof(binding_wire)) {
        return false;
    }
    if (route->effective.codec == MEISO_COMPOSITION_MEDIA_CODEC_JPEG) return true;
    return meiso_composition_media_transport_private_access_unit_bytes(
               &route->effective) != 0U &&
           meiso_composition_media_transport_private_access_unit_bytes(
               &route->effective) <= route->capability.max_access_unit_bytes;
}

static bool config_valid(const MeisoCompositionMediaTransportConfig *config)
{
    uint8_t open_wire[MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE];
    uint8_t active_route_mask;
    size_t wire_size = 0U;
    if (config == NULL ||
        (config->local_role != MEISO_SESSION_ROLE_HOST &&
         config->local_role != MEISO_SESSION_ROLE_EDGE) ||
        !active_route_mask_valid(config->active_route_mask) ||
        meiso_composition_media_open_encode(
            &config->scope, (MeisoMutableBuffer){open_wire, sizeof(open_wire)},
            &wire_size) != MEISO_COMPOSITION_PROFILE_OK ||
        wire_size != sizeof(open_wire)) {
        return false;
    }
    active_route_mask = config->active_route_mask;
    return (meiso_composition_media_transport_private_route_selected(
                active_route_mask, 0U)
                ? route_config_valid(&config->scope, &config->routes[0], 0U)
                : inactive_route_capacity_valid(&config->routes[0], 0U)) &&
           (meiso_composition_media_transport_private_route_selected(
                active_route_mask, 1U)
                ? route_config_valid(&config->scope, &config->routes[1], 1U)
                : inactive_route_capacity_valid(&config->routes[1], 1U));
}

static bool
calculate_required(const MeisoCompositionMediaTransportConfig *config,
                   size_t *out_required, uint32_t *out_packet_capacity)
{
    size_t required = MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES;
    size_t payload_bytes = 0U;
    uint32_t max_fragment = 0U;
    size_t i;
    if (!config_valid(config)) {
        return false;
    }
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        const uint32_t capacity =
            config->routes[i].capability.max_access_unit_bytes;
        if (!checked_add_size(payload_bytes, capacity, &payload_bytes) ||
            !checked_add_size(payload_bytes, capacity, &payload_bytes)) {
            return false;
        }
        if (config->routes[i].capability.max_fragment_payload_bytes >
            max_fragment) {
            max_fragment =
                config->routes[i].capability.max_fragment_payload_bytes;
        }
    }
    if (!checked_add_size(required, payload_bytes, &required) ||
        !checked_add_size(required,
                          MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE,
                          &required) ||
        !checked_add_size(required, max_fragment, &required)) {
        return false;
    }
    *out_required = required;
    *out_packet_capacity =
        MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + max_fragment;
    return true;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_memory_required(
    const MeisoCompositionMediaTransportConfig *config, size_t *out_size,
    size_t *out_alignment)
{
    size_t required = 0U;
    uint32_t packet_capacity = 0U;
    if (config == NULL || out_size == NULL || out_alignment == NULL ||
        meiso_composition_media_transport_private_ranges_overlap(
            config, sizeof(*config), out_size, sizeof(*out_size)) ||
        meiso_composition_media_transport_private_ranges_overlap(
            config, sizeof(*config), out_alignment, sizeof(*out_alignment)) ||
        meiso_composition_media_transport_private_ranges_overlap(
            out_size, sizeof(*out_size), out_alignment,
            sizeof(*out_alignment))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!calculate_required(config, &required, &packet_capacity)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG;
    }
    (void)packet_capacity;
    *out_size = required;
    *out_alignment = alignof(max_align_t);
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus meiso_composition_media_transport_init(
    const MeisoCompositionMediaTransportConfig *config,
    MeisoMutableBuffer arena, MeisoCompositionMediaTransport **out_transport)
{
    MeisoCompositionMediaTransport value;
    MeisoCompositionMediaTransport *transport;
    size_t required = 0U;
    uint32_t packet_capacity = 0U;
    size_t offset = MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES;
    size_t i;
    if (config == NULL || arena.data == NULL || out_transport == NULL ||
        meiso_composition_media_transport_private_ranges_overlap(
            config, sizeof(*config), arena.data, arena.capacity) ||
        meiso_composition_media_transport_private_ranges_overlap(
            out_transport, sizeof(*out_transport), arena.data,
            arena.capacity) ||
        meiso_composition_media_transport_private_ranges_overlap(
            config, sizeof(*config), out_transport, sizeof(*out_transport))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!calculate_required(config, &required, &packet_capacity)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG;
    }
    if (!pointer_aligned(arena.data, alignof(max_align_t)) ||
        arena.capacity < required) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY;
    }

    memset(&value, 0, sizeof(value));
    value.magic = MEISO_COMPOSITION_MEDIA_TRANSPORT_MAGIC;
    value.local_role = config->local_role;
    value.phase = config->local_role == MEISO_SESSION_ROLE_HOST
                      ? MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING
                      : MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN;
    value.scope = config->scope;
    value.active_route_mask = config->active_route_mask;
    value.arena_begin = arena.data;
    value.arena_extent = arena.capacity;
    value.required_bytes = required;
    value.packet_scratch_capacity = packet_capacity;
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        const uint32_t capacity =
            config->routes[i].capability.max_access_unit_bytes;
        value.routes[i].config = config->routes[i];
        value.routes[i].primary.bytes = arena.data + offset;
        value.routes[i].primary.capacity = capacity;
        offset += capacity;
        value.routes[i].latest.bytes = arena.data + offset;
        value.routes[i].latest.capacity = capacity;
        offset += capacity;
    }
    value.packet_scratch = arena.data + offset;
    offset += packet_capacity;
    if (offset != required) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG;
    }

    memset(arena.data, 0, required);
    transport = (MeisoCompositionMediaTransport *)arena.data;
    *transport = value;
    *out_transport = transport;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}
static void unit_from_slot(const MeisoCompositionMediaTransportFrameSlot *slot,
                           MeisoCompositionMediaUnit *out_unit)
{
    const MeisoCompositionMediaTransportFrameMetadata *metadata =
        &slot->metadata;
    memset(out_unit, 0, sizeof(*out_unit));
    out_unit->binding = metadata->binding;
    out_unit->frame_sequence = metadata->frame_sequence;
    out_unit->source_time = metadata->source_time;
    out_unit->applied_scene_owner = metadata->applied_scene_owner;
    out_unit->applied_scene_state_version =
        metadata->applied_scene_state_version;
    out_unit->active_plan_generation = metadata->active_plan_generation;
    out_unit->world_generation = metadata->world_generation;
    out_unit->dependency_sequence = metadata->dependency_sequence;
    out_unit->producer_dropped_frames = metadata->producer_dropped_frames;
    out_unit->pixel_format = metadata->pixel_format;
    out_unit->codec = metadata->codec;
    out_unit->width = metadata->width;
    out_unit->height = metadata->height;
    out_unit->flags = metadata->flags;
    out_unit->access_unit.data = slot->bytes;
    out_unit->access_unit.size = metadata->access_unit_bytes;
}
MeisoCompositionMediaTransportStatus meiso_composition_media_transport_latest(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest)
{
    MeisoCompositionMediaLatestView view;
    const int route_index =
        meiso_composition_media_transport_private_route_index(source);
    const MeisoCompositionMediaTransportRouteState *route;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        out_latest == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, out_latest, sizeof(*out_latest))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST || route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!meiso_composition_media_transport_private_route_selected(
            transport->active_route_mask, (size_t)route_index)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    if (!route->latest.occupied) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME;
    }
    memset(&view, 0, sizeof(view));
    view.ref.binding = route->latest.metadata.binding;
    view.ref.frame_sequence = route->latest.metadata.frame_sequence;
    unit_from_slot(&route->latest, &view.unit);
    *out_latest = view;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_release_latest(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaFrameRef *expected_frame)
{
    const int route_index =
        expected_frame != NULL
            ? meiso_composition_media_transport_private_route_index(
                  expected_frame->binding.source)
            : -1;
    MeisoCompositionMediaTransportRouteState *route;
    MeisoCompositionMediaFrameRef current;
    MeisoCompositionMediaTransportStatus binding_status;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        expected_frame == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, expected_frame, sizeof(*expected_frame))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST || route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!meiso_composition_media_transport_private_route_selected(
            transport->active_route_mask, (size_t)route_index)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    binding_status =
        meiso_composition_media_transport_private_binding_difference(
            &route->config.binding, &expected_frame->binding);
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if (!route->latest.occupied) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME;
    }
    current.binding = route->latest.metadata.binding;
    current.frame_sequence = route->latest.metadata.frame_sequence;
    if (!meiso_composition_media_transport_private_frame_ref_equal(
            expected_frame, &current)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
    }
    route->latest.occupied = false;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_terminate(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaOpen *expected_scope,
    MeisoCompositionMediaTerminalReason reason)
{
    size_t i;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        expected_scope == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, expected_scope, sizeof(*expected_scope))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!meiso_composition_media_transport_private_terminal_reason_valid(
            reason)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (!meiso_composition_media_transport_private_open_equal(&transport->scope,
                                                              expected_scope)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        MeisoCompositionMediaTransportRouteState *route = &transport->routes[i];
        memset(route->primary.bytes, 0, route->primary.capacity);
        memset(route->latest.bytes, 0, route->latest.capacity);
        route->primary.occupied = false;
        route->latest.occupied = false;
        route->primary_offset = 0U;
    }
    memset(transport->packet_scratch, 0, transport->packet_scratch_capacity);
    transport->outbound_packet_pending = false;
    transport->outbound_packet_size = 0U;
    memset(&transport->outbound_packet_ref, 0,
           sizeof(transport->outbound_packet_ref));
    transport->bound_route_mask = 0U;
    transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL;
    transport->terminal_reason = reason;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus meiso_composition_media_transport_snapshot(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaTransportSnapshot *out_snapshot)
{
    MeisoCompositionMediaTransportSnapshot snapshot;
    size_t i;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        out_snapshot == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, out_snapshot, sizeof(*out_snapshot))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.local_role = transport->local_role;
    snapshot.scope = transport->scope;
    snapshot.phase = transport->phase;
    snapshot.terminal_reason = transport->terminal_reason;
    snapshot.configured_active_route_mask = transport->active_route_mask;
    snapshot.bound_route_mask = transport->bound_route_mask;
    snapshot.fragment_rr_cursor = transport->fragment_rr_cursor;
    snapshot.outbound_packet_pending = transport->outbound_packet_pending;
    snapshot.outbound_packet_kind = transport->outbound_packet_pending
                                        ? transport->outbound_packet_ref.kind
                                        : 0U;
    snapshot.outbound_packet_serial_high_water =
        transport->outbound_packet_serial_high_water;
    snapshot.arena_bytes = transport->required_bytes;
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        const MeisoCompositionMediaTransportRouteState *route =
            &transport->routes[i];
        MeisoCompositionMediaRouteSnapshot *out_route = &snapshot.routes[i];
        out_route->source = i == 0U
                                ? MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW
                                : MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        out_route->send_in_flight =
            transport->local_role == MEISO_SESSION_ROLE_EDGE &&
            route->primary.occupied;
        out_route->send_latest =
            transport->local_role == MEISO_SESSION_ROLE_EDGE &&
            route->latest.occupied;
        out_route->send_frame_sequence =
            out_route->send_in_flight ? route->primary.metadata.frame_sequence
                                      : 0U;
        out_route->send_fragment_offset =
            out_route->send_in_flight ? route->primary_offset : 0U;
        out_route->submitted_sequence_high_water =
            route->submitted_sequence_high_water;
        out_route->transport_whole_frames_dropped =
            route->transport_whole_frames_dropped;
        out_route->receive_reassembly =
            transport->local_role == MEISO_SESSION_ROLE_HOST &&
            route->primary.occupied;
        out_route->receive_latest =
            transport->local_role == MEISO_SESSION_ROLE_HOST &&
            route->latest.occupied;
        out_route->receive_frame_sequence =
            out_route->receive_reassembly
                ? route->primary.metadata.frame_sequence
                : 0U;
        out_route->receive_next_offset =
            out_route->receive_reassembly ? route->primary_offset : 0U;
        out_route->published_sequence_high_water =
            route->published_sequence_high_water;
        out_route->sequence_gap_frames = route->sequence_gap_frames;
        out_route->producer_drop_high_water = route->producer_drop_high_water;
        out_route->reassembly_frames_dropped = route->reassembly_frames_dropped;
        out_route->unread_latest_frames_dropped =
            route->unread_latest_frames_dropped;
    }
    *out_snapshot = snapshot;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}
