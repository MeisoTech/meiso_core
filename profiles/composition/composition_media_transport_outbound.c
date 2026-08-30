#include "meiso_composition_media_transport_private.h"

/* Outbound scheduling owns OPEN/BIND emission, frame queuing and progress. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void
metadata_from_unit(const MeisoCompositionMediaUnit *unit,
                   uint64_t effective_drops,
                   MeisoCompositionMediaTransportFrameMetadata *out_metadata)
{
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->binding = unit->binding;
    out_metadata->frame_sequence = unit->frame_sequence;
    out_metadata->source_time = unit->source_time;
    out_metadata->applied_scene_owner = unit->applied_scene_owner;
    out_metadata->applied_scene_state_version =
        unit->applied_scene_state_version;
    out_metadata->active_plan_generation = unit->active_plan_generation;
    out_metadata->world_generation = unit->world_generation;
    out_metadata->dependency_sequence = unit->dependency_sequence;
    out_metadata->producer_dropped_frames = effective_drops;
    out_metadata->pixel_format = unit->pixel_format;
    out_metadata->codec = unit->codec;
    out_metadata->width = unit->width;
    out_metadata->height = unit->height;
    out_metadata->flags = unit->flags;
    out_metadata->access_unit_bytes = (uint32_t)unit->access_unit.size;
}

static void
frame_from_primary(const MeisoCompositionMediaTransportRouteState *route,
                   uint32_t fragment_size,
                   MeisoCompositionMediaFrame *out_frame)
{
    const MeisoCompositionMediaTransportFrameMetadata *metadata =
        &route->primary.metadata;
    const uint32_t offset = route->primary_offset;
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->binding = metadata->binding;
    out_frame->frame_sequence = metadata->frame_sequence;
    out_frame->source_time = metadata->source_time;
    out_frame->applied_scene_owner = metadata->applied_scene_owner;
    out_frame->applied_scene_state_version =
        metadata->applied_scene_state_version;
    out_frame->active_plan_generation = metadata->active_plan_generation;
    out_frame->world_generation = metadata->world_generation;
    out_frame->dependency_sequence = metadata->dependency_sequence;
    out_frame->producer_dropped_frames = metadata->producer_dropped_frames;
    out_frame->pixel_format = metadata->pixel_format;
    out_frame->codec = metadata->codec;
    out_frame->width = metadata->width;
    out_frame->height = metadata->height;
    out_frame->access_unit_bytes = metadata->access_unit_bytes;
    out_frame->fragment_offset = offset;
    out_frame->fragment.data = route->primary.bytes + offset;
    out_frame->fragment.size = fragment_size;
    out_frame->flags = metadata->flags;
    if (offset == 0U) {
        out_frame->flags |= MEISO_COMPOSITION_MEDIA_FRAME_BEGIN;
    }
    if (offset + fragment_size == metadata->access_unit_bytes) {
        out_frame->flags |= MEISO_COMPOSITION_MEDIA_FRAME_END;
    }
}

static MeisoCompositionMediaTransportStatus cache_outbound_packet(
    MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaPacketKind kind, MeisoCompositionMediaSource source,
    uint64_t frame_sequence, uint32_t fragment_offset, size_t packet_size,
    MeisoCompositionMediaPacketView *out_packet)
{
    MeisoCompositionMediaPacketRef ref;
    MeisoCompositionMediaPacketView view;
    uint64_t serial;
    if (!meiso_composition_media_transport_private_checked_add_u64(
            transport->outbound_packet_serial_high_water, 1U, &serial)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (packet_size > UINT32_MAX) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY;
    }
    memset(&ref, 0, sizeof(ref));
    ref.session_id = transport->scope.session_id;
    ref.catalog_generation = transport->scope.catalog_generation;
    ref.serial = serial;
    ref.kind = kind;
    ref.source = source;
    ref.frame_sequence = frame_sequence;
    ref.fragment_offset = fragment_offset;
    ref.packet_bytes = (uint32_t)packet_size;
    view.ref = ref;
    view.bytes.data = transport->packet_scratch;
    view.bytes.size = packet_size;

    transport->outbound_packet_ref = ref;
    transport->outbound_packet_size = packet_size;
    transport->outbound_packet_serial_high_water = serial;
    transport->outbound_packet_pending = true;
    *out_packet = view;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_next_outbound(
    MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaPacketView *out_packet)
{
    MeisoCompositionProfileStatus codec_status;
    size_t packet_size = 0U;
    size_t scan;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        out_packet == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, out_packet, sizeof(*out_packet))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->outbound_packet_pending) {
        MeisoCompositionMediaPacketView view;
        view.ref = transport->outbound_packet_ref;
        view.bytes.data = transport->packet_scratch;
        view.bytes.size = transport->outbound_packet_size;
        *out_packet = view;
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
    }
    if (transport->outbound_packet_serial_high_water == UINT64_MAX) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }

    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING) {
        codec_status = meiso_composition_media_open_encode(
            &transport->scope,
            (MeisoMutableBuffer){transport->packet_scratch,
                                 transport->packet_scratch_capacity},
            &packet_size);
        if (codec_status != MEISO_COMPOSITION_PROFILE_OK) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        return cache_outbound_packet(transport,
                                     MEISO_COMPOSITION_MEDIA_PACKET_OPEN, 0U,
                                     0U, 0U, packet_size, out_packet);
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING) {
        const MeisoCompositionMediaTransportRouteState *route;
        if (transport->bind_send_index >=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT ||
            !meiso_composition_media_transport_private_route_selected(
                transport->active_route_mask, transport->bind_send_index)) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        route = &transport->routes[transport->bind_send_index];
        codec_status = meiso_composition_media_binding_encode(
            &route->config.binding,
            (MeisoMutableBuffer){transport->packet_scratch,
                                 transport->packet_scratch_capacity},
            &packet_size);
        if (codec_status != MEISO_COMPOSITION_PROFILE_OK) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        return cache_outbound_packet(
            transport, MEISO_COMPOSITION_MEDIA_PACKET_BIND,
            route->config.binding.source, 0U, 0U, packet_size, out_packet);
    }
    if (transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET;
    }

    for (scan = 0U; scan < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT;
         ++scan) {
        const size_t route_index =
            (transport->fragment_rr_cursor + scan) %
            MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT;
        MeisoCompositionMediaTransportRouteState *route =
            &transport->routes[route_index];
        MeisoCompositionMediaFrame frame;
        uint32_t remaining;
        uint32_t fragment_size;
        if (!meiso_composition_media_transport_private_route_selected(
                transport->active_route_mask, route_index) ||
            !route->primary.occupied) {
            continue;
        }
        if (route->primary_offset >=
            route->primary.metadata.access_unit_bytes) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        remaining =
            route->primary.metadata.access_unit_bytes - route->primary_offset;
        fragment_size =
            remaining < route->config.capability.max_fragment_payload_bytes
                ? remaining
                : route->config.capability.max_fragment_payload_bytes;
        frame_from_primary(route, fragment_size, &frame);
        codec_status = meiso_composition_media_frame_encode(
            &frame,
            (MeisoMutableBuffer){transport->packet_scratch,
                                 transport->packet_scratch_capacity},
            &packet_size);
        if (codec_status != MEISO_COMPOSITION_PROFILE_OK) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        return cache_outbound_packet(
            transport, MEISO_COMPOSITION_MEDIA_PACKET_FRAME,
            route->config.binding.source,
            route->primary.metadata.frame_sequence, route->primary_offset,
            packet_size, out_packet);
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET;
}

static void
promote_latest_to_primary(MeisoCompositionMediaTransportRouteState *route)
{
    MeisoCompositionMediaTransportFrameSlot temporary = route->primary;
    route->primary.occupied = false;
    route->primary_offset = 0U;
    if (route->latest.occupied) {
        route->primary = route->latest;
        route->latest = temporary;
        route->latest.occupied = false;
    }
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_complete_outbound(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaPacketRef *expected_packet)
{
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        expected_packet == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, expected_packet, sizeof(*expected_packet))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (!transport->outbound_packet_pending) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!meiso_composition_media_transport_private_packet_ref_equal(
            expected_packet, &transport->outbound_packet_ref)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
    }

    if (expected_packet->kind == MEISO_COMPOSITION_MEDIA_PACKET_OPEN) {
        if (transport->phase !=
            MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_WAIT_BIND;
    } else if (expected_packet->kind == MEISO_COMPOSITION_MEDIA_PACKET_BIND) {
        uint8_t next_route_index;
        if (transport->phase !=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING ||
            transport->bind_send_index >=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT ||
            !meiso_composition_media_transport_private_route_selected(
                transport->active_route_mask, transport->bind_send_index) ||
            transport->routes[transport->bind_send_index]
                    .config.binding.source != expected_packet->source) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        transport->bound_route_mask |=
            meiso_composition_media_transport_private_route_bit(
                transport->bind_send_index);
        if (meiso_composition_media_transport_private_next_selected_route(
                transport->active_route_mask,
                (size_t)transport->bind_send_index + 1U, &next_route_index)) {
            transport->bind_send_index = next_route_index;
        } else {
            transport->bind_send_index =
                MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT;
            transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE;
        }
    } else if (expected_packet->kind == MEISO_COMPOSITION_MEDIA_PACKET_FRAME) {
        const int route_index =
            meiso_composition_media_transport_private_route_index(
                expected_packet->source);
        MeisoCompositionMediaTransportRouteState *route;
        const uint32_t fragment_size =
            expected_packet->packet_bytes -
            MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
        uint32_t next_offset;
        if (transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE ||
            route_index < 0 ||
            !meiso_composition_media_transport_private_route_selected(
                transport->active_route_mask, (size_t)route_index) ||
            expected_packet->packet_bytes <=
                MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        route = &transport->routes[(size_t)route_index];
        if (!route->primary.occupied ||
            route->primary.metadata.frame_sequence !=
                expected_packet->frame_sequence ||
            route->primary_offset != expected_packet->fragment_offset ||
            fragment_size > route->primary.metadata.access_unit_bytes -
                                route->primary_offset) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        next_offset = route->primary_offset + fragment_size;
        route->primary_offset = next_offset;
        if (next_offset == route->primary.metadata.access_unit_bytes) {
            promote_latest_to_primary(route);
        }
        if (!meiso_composition_media_transport_private_next_selected_route(
                transport->active_route_mask,
                ((size_t)route_index + 1U) %
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT,
                &transport->fragment_rr_cursor) &&
            !meiso_composition_media_transport_private_next_selected_route(
                transport->active_route_mask, 0U,
                &transport->fragment_rr_cursor)) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
    } else {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    transport->outbound_packet_pending = false;
    transport->outbound_packet_size = 0U;
    memset(&transport->outbound_packet_ref, 0,
           sizeof(transport->outbound_packet_ref));
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus meiso_composition_media_transport_submit(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaUnit *unit)
{
    const int route_index =
        unit != NULL ? meiso_composition_media_transport_private_route_index(
                           unit->binding.source)
                     : -1;
    MeisoCompositionMediaTransportRouteState *route;
    MeisoCompositionMediaTransportFrameSlot *destination;
    MeisoCompositionMediaTransportFrameMetadata metadata;
    MeisoCompositionMediaTransportStatus binding_status;
    uint64_t new_transport_drops;
    bool replace_latest;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        unit == NULL ||
        meiso_composition_media_transport_private_overlaps(transport, unit,
                                                           sizeof(*unit)) ||
        unit->access_unit.data == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, unit->access_unit.data, unit->access_unit.size)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE ||
        route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!meiso_composition_media_transport_private_route_selected(
            transport->active_route_mask, (size_t)route_index)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    binding_status =
        meiso_composition_media_transport_private_binding_difference(
            &route->config.binding, &unit->binding);
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if (!meiso_composition_media_transport_private_desired_equal(
            &route->config.effective,
            &(MeisoCompositionMediaDesired){
                unit->pixel_format, unit->codec, unit->width, unit->height,
                route->config.effective.fps_millihz,
                route->config.effective.bitrate_bps,
                route->config.effective.gop_frames,
                route->config.effective.recovery_flags}) ||
        unit->frame_sequence == 0U ||
        !meiso_time_point_valid(unit->source_time) ||
        !meiso_composition_media_transport_private_scene_fields_valid(unit) ||
        unit->dependency_sequence != 0U ||
        (unit->flags & MEISO_COMPOSITION_MEDIA_FRAME_KEY) == 0U ||
        (unit->flags & (MeisoCompositionMediaFrameFlags) ~(
                           MEISO_COMPOSITION_MEDIA_FRAME_KEY |
                           MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U ||
        unit->access_unit.size !=
            meiso_composition_media_transport_private_access_unit_bytes(
                &route->config.effective) ||
        unit->access_unit.size > route->primary.capacity) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (unit->frame_sequence <= route->submitted_sequence_high_water ||
        unit->producer_dropped_frames <
            route->submitted_producer_drop_high_water) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }

    replace_latest = route->primary.occupied && route->latest.occupied;
    new_transport_drops = route->transport_whole_frames_dropped;
    if (replace_latest &&
        !meiso_composition_media_transport_private_checked_add_u64(
            new_transport_drops, 1U, &new_transport_drops)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    metadata_from_unit(unit, unit->producer_dropped_frames, &metadata);
    destination = !route->primary.occupied ? &route->primary : &route->latest;
    memmove(destination->bytes, unit->access_unit.data, unit->access_unit.size);
    destination->metadata = metadata;
    destination->occupied = true;
    if (destination == &route->primary) {
        route->primary_offset = 0U;
    }
    route->submitted_sequence_high_water = unit->frame_sequence;
    route->submitted_producer_drop_high_water = unit->producer_dropped_frames;
    route->transport_whole_frames_dropped = new_transport_drops;
    return replace_latest ? MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED
                          : MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}
