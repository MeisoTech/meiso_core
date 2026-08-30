#include "meiso_composition_media_transport_private.h"

/* Receive processing owns OPEN/BIND admission and bounded frame reassembly. */

#include <stdint.h>
#include <string.h>

static uint32_t read_be_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static void
metadata_from_frame(const MeisoCompositionMediaFrame *frame,
                    MeisoCompositionMediaTransportFrameMetadata *out_metadata)
{
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->binding = frame->binding;
    out_metadata->frame_sequence = frame->frame_sequence;
    out_metadata->source_time = frame->source_time;
    out_metadata->applied_scene_owner = frame->applied_scene_owner;
    out_metadata->applied_scene_state_version =
        frame->applied_scene_state_version;
    out_metadata->active_plan_generation = frame->active_plan_generation;
    out_metadata->world_generation = frame->world_generation;
    out_metadata->dependency_sequence = frame->dependency_sequence;
    out_metadata->producer_dropped_frames = frame->producer_dropped_frames;
    out_metadata->pixel_format = frame->pixel_format;
    out_metadata->codec = frame->codec;
    out_metadata->width = frame->width;
    out_metadata->height = frame->height;
    out_metadata->flags =
        frame->flags & (MEISO_COMPOSITION_MEDIA_FRAME_KEY |
                        MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY);
    out_metadata->access_unit_bytes = frame->access_unit_bytes;
}
static MeisoCompositionMediaTransportStatus
receive_open(MeisoCompositionMediaTransport *transport, MeisoByteView packet)
{
    MeisoCompositionMediaOpen open;
    MeisoCompositionProfileStatus status =
        meiso_composition_media_open_decode(packet, &open);
    if (status == MEISO_COMPOSITION_PROFILE_TRUNCATED) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!meiso_composition_media_transport_private_open_equal(&transport->scope,
                                                              &open)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    if (!meiso_composition_media_transport_private_next_selected_route(
            transport->active_route_mask, 0U, &transport->bind_send_index)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static MeisoCompositionMediaTransportStatus
receive_bind(MeisoCompositionMediaTransport *transport, MeisoByteView packet)
{
    MeisoCompositionMediaBinding binding;
    MeisoCompositionProfileStatus codec_status =
        meiso_composition_media_binding_decode(packet, &binding);
    MeisoCompositionMediaTransportStatus binding_status;
    const int route_index =
        codec_status == MEISO_COMPOSITION_PROFILE_OK
            ? meiso_composition_media_transport_private_route_index(
                  binding.source)
            : -1;
    uint8_t route_mask_bit;
    if (codec_status == MEISO_COMPOSITION_PROFILE_TRUNCATED) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (codec_status != MEISO_COMPOSITION_PROFILE_OK) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST || route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route_mask_bit = meiso_composition_media_transport_private_route_bit(
        (size_t)route_index);
    if ((transport->active_route_mask & route_mask_bit) == 0U) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    if (transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_WAIT_BIND) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    binding_status =
        meiso_composition_media_transport_private_binding_difference(
            &transport->routes[(size_t)route_index].config.binding, &binding);
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if ((transport->bound_route_mask & route_mask_bit) != 0U) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
    }
    transport->bound_route_mask |= route_mask_bit;
    if (transport->bound_route_mask == transport->active_route_mask) {
        transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE;
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static bool precompute_publication(
    const MeisoCompositionMediaTransportRouteState *route,
    const MeisoCompositionMediaTransportFrameMetadata *metadata,
    uint64_t *out_sequence_gaps, uint64_t *out_latest_drops)
{
    uint64_t gap;
    if (metadata->frame_sequence <= route->published_sequence_high_water ||
        metadata->producer_dropped_frames < route->producer_drop_high_water) {
        return false;
    }
    gap = route->published_sequence_high_water == 0U
              ? metadata->frame_sequence - 1U
              : metadata->frame_sequence -
                    route->published_sequence_high_water - 1U;
    return meiso_composition_media_transport_private_checked_add_u64(
               route->sequence_gap_frames, gap, out_sequence_gaps) &&
           meiso_composition_media_transport_private_checked_add_u64(
               route->unread_latest_frames_dropped,
               route->latest.occupied ? 1U : 0U, out_latest_drops);
}

static void publish_primary(MeisoCompositionMediaTransportRouteState *route,
                            uint64_t sequence_gaps, uint64_t latest_drops)
{
    MeisoCompositionMediaTransportFrameSlot temporary = route->latest;
    const uint64_t sequence = route->primary.metadata.frame_sequence;
    const uint64_t producer_drops =
        route->primary.metadata.producer_dropped_frames;
    route->latest = route->primary;
    route->primary = temporary;
    route->primary.occupied = false;
    route->primary_offset = 0U;
    route->published_sequence_high_water = sequence;
    route->producer_drop_high_water = producer_drops;
    route->sequence_gap_frames = sequence_gaps;
    route->unread_latest_frames_dropped = latest_drops;
}

static MeisoCompositionMediaTransportStatus
receive_frame(MeisoCompositionMediaTransport *transport, MeisoByteView packet)
{
    MeisoCompositionMediaFrame frame;
    MeisoCompositionMediaTransportFrameMetadata metadata;
    MeisoCompositionProfileStatus codec_status =
        meiso_composition_media_frame_decode(packet, &frame);
    MeisoCompositionMediaTransportStatus binding_status;
    MeisoCompositionMediaTransportRouteState *route;
    int route_index;
    uint32_t end_offset;
    bool finish;
    uint64_t sequence_gaps = 0U;
    uint64_t latest_drops = 0U;
    uint64_t reassembly_drops = 0U;
    if (codec_status == MEISO_COMPOSITION_PROFILE_TRUNCATED) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (codec_status != MEISO_COMPOSITION_PROFILE_OK) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route_index = meiso_composition_media_transport_private_route_index(
        frame.binding.source);
    if (route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    if (!meiso_composition_media_transport_private_route_selected(
            transport->active_route_mask, (size_t)route_index)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    binding_status =
        meiso_composition_media_transport_private_binding_difference(
            &route->config.binding, &frame.binding);
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if (frame.pixel_format != route->config.effective.pixel_format ||
        frame.codec != route->config.effective.codec ||
        frame.width != route->config.effective.width ||
        frame.height != route->config.effective.height ||
        frame.access_unit_bytes !=
            meiso_composition_media_transport_private_access_unit_bytes(
                &route->config.effective) ||
        frame.access_unit_bytes > route->primary.capacity ||
        frame.fragment.size >
            route->config.capability.max_fragment_payload_bytes) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    metadata_from_frame(&frame, &metadata);
    end_offset = frame.fragment_offset + (uint32_t)frame.fragment.size;
    finish = end_offset == frame.access_unit_bytes;

    if (!route->primary.occupied) {
        if (frame.fragment_offset != 0U) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY;
        }
        if (frame.frame_sequence <= route->published_sequence_high_water ||
            frame.producer_dropped_frames < route->producer_drop_high_water) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
        }
        if (finish && !precompute_publication(route, &metadata, &sequence_gaps,
                                              &latest_drops)) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
        memmove(route->primary.bytes, frame.fragment.data, frame.fragment.size);
        route->primary.metadata = metadata;
        route->primary.occupied = true;
        route->primary_offset = end_offset;
        if (finish) {
            publish_primary(route, sequence_gaps, latest_drops);
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED;
        }
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED;
    }

    if (frame.frame_sequence < route->primary.metadata.frame_sequence ||
        frame.frame_sequence <= route->published_sequence_high_water) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }
    if (frame.frame_sequence == route->primary.metadata.frame_sequence) {
        if (!meiso_composition_media_transport_private_metadata_equal(
                &route->primary.metadata, &metadata)) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
        }
        if (frame.fragment_offset != route->primary_offset) {
            if (!meiso_composition_media_transport_private_checked_add_u64(
                    route->reassembly_frames_dropped, 1U, &reassembly_drops)) {
                return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
            }
            route->primary.occupied = false;
            route->primary_offset = 0U;
            route->reassembly_frames_dropped = reassembly_drops;
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY;
        }
        if (finish && !precompute_publication(route, &metadata, &sequence_gaps,
                                              &latest_drops)) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
        memmove(route->primary.bytes + frame.fragment_offset,
                frame.fragment.data, frame.fragment.size);
        route->primary_offset = end_offset;
        if (finish) {
            publish_primary(route, sequence_gaps, latest_drops);
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED;
        }
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED;
    }

    if (frame.fragment_offset == 0U &&
        (frame.producer_dropped_frames <
             route->primary.metadata.producer_dropped_frames ||
         frame.producer_dropped_frames < route->producer_drop_high_water)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }
    if (!meiso_composition_media_transport_private_checked_add_u64(
            route->reassembly_frames_dropped, 1U, &reassembly_drops)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (frame.fragment_offset != 0U) {
        route->primary.occupied = false;
        route->primary_offset = 0U;
        route->reassembly_frames_dropped = reassembly_drops;
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY;
    }
    if (finish && !precompute_publication(route, &metadata, &sequence_gaps,
                                          &latest_drops)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    memmove(route->primary.bytes, frame.fragment.data, frame.fragment.size);
    route->primary.metadata = metadata;
    route->primary.occupied = true;
    route->primary_offset = end_offset;
    route->reassembly_frames_dropped = reassembly_drops;
    if (finish) {
        publish_primary(route, sequence_gaps, latest_drops);
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED;
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_REASSEMBLY_REPLACED;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_receive_packet(
    MeisoCompositionMediaTransport *transport, MeisoByteView packet)
{
    uint32_t magic;
    if (!meiso_composition_media_transport_private_handle_valid(transport) ||
        packet.data == NULL ||
        meiso_composition_media_transport_private_overlaps(
            transport, packet.data, packet.size)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (packet.size < 4U) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    magic = read_be_u32(packet.data);
    if (magic == MEISO_COMPOSITION_MEDIA_TRANSPORT_OPEN_MAGIC) {
        return receive_open(transport, packet);
    }
    if (magic == MEISO_COMPOSITION_MEDIA_TRANSPORT_BIND_MAGIC) {
        return receive_bind(transport, packet);
    }
    if (magic == MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_MAGIC) {
        return receive_frame(transport, packet);
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
}
