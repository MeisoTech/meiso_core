#include "display_ingress_media_transport_private.h"

/* Host-side bounded latest submission and packet progress. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool rgba_frame_opaque(MeisoByteView frame)
{
    size_t index;

    for (index = 3U; index < frame.size; index += 4U) {
        if (frame.data[index] != UINT8_MAX) {
            return false;
        }
    }
    return true;
}

static void metadata_from_frame(
    const MeisoDisplayIngressMediaFrame *frame,
    uint64_t effective_drops,
    MeisoDisplayIngressMediaFrameMetadata *out_metadata
)
{
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->frame_sequence = frame->frame_sequence;
    out_metadata->source_time = frame->source_time;
    out_metadata->sender_deadline = frame->sender_deadline;
    out_metadata->producer_dropped_frames = effective_drops;
    out_metadata->frame_bytes = (uint32_t)frame->rgba_frame.size;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_submit(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrame *frame,
    const MeisoStreamAdmission *admission
)
{
    MeisoDisplayIngressMediaFrameSlot *destination;
    MeisoDisplayIngressMediaFrameMetadata metadata;
    MeisoDisplayIngressMediaTransportStatus deadline_status;
    uint64_t new_transport_drops;
    uint64_t effective_drops;
    uint64_t reserved_transport_drops = 0U;
    bool replace_latest;
    bool reserve_preemption;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        frame == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, frame, sizeof(*frame)
        ) ||
        (admission != NULL &&
         meiso_display_ingress_media_private_overlaps(
             transport, admission, sizeof(*admission)
         )) ||
        frame->rgba_frame.data == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, frame->rgba_frame.data, frame->rgba_frame.size
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (frame->frame_sequence == 0U ||
        !meiso_time_point_valid(frame->source_time) ||
        !meiso_deadline_valid(frame->sender_deadline) ||
        (frame->sender_deadline.present &&
         frame->sender_deadline.value.clock_epoch !=
             frame->source_time.clock_epoch) ||
        frame->rgba_frame.size != transport->frame_bytes ||
        !rgba_frame_opaque(frame->rgba_frame)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }

    deadline_status = meiso_display_ingress_media_private_deadline_status(
        transport->deadline_policy, frame->sender_deadline, admission
    );
    if (deadline_status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return deadline_status;
    }
    if (frame->frame_sequence <= transport->submitted_sequence_high_water ||
        frame->producer_dropped_frames <
            transport->submitted_producer_drop_high_water) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }

    replace_latest = transport->primary.occupied &&
        transport->latest.occupied;
    reserve_preemption = transport->primary.occupied &&
        transport->outbound_packet_pending &&
        transport->outbound_packet_ref.packet_bytes >
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE &&
        transport->outbound_packet_ref.fragment_offset <
            transport->frame_bytes &&
        transport->outbound_packet_ref.packet_bytes -
                MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE <
            transport->frame_bytes -
                transport->outbound_packet_ref.fragment_offset;
    new_transport_drops = transport->transport_whole_frames_dropped;
    if (replace_latest &&
        !meiso_display_ingress_media_private_checked_add_u64(
            new_transport_drops, 1U, &new_transport_drops
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (reserve_preemption) {
        if (!meiso_display_ingress_media_private_checked_add_u64(
                new_transport_drops, 1U, &reserved_transport_drops
            ) ||
            !meiso_display_ingress_media_private_checked_add_u64(
                frame->producer_dropped_frames, reserved_transport_drops,
                &effective_drops
            )) {
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
    } else if (!meiso_display_ingress_media_private_checked_add_u64(
                   frame->producer_dropped_frames, new_transport_drops,
                   &effective_drops
               )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }

    metadata_from_frame(frame, effective_drops, &metadata);
    destination = !transport->primary.occupied
        ? &transport->primary
        : &transport->latest;
    memmove(
        destination->bytes, frame->rgba_frame.data, frame->rgba_frame.size
    );
    destination->metadata = metadata;
    destination->occupied = true;
    if (destination == &transport->primary) {
        transport->primary_offset = 0U;
    }
    transport->submitted_sequence_high_water = frame->frame_sequence;
    transport->submitted_producer_drop_high_water =
        frame->producer_dropped_frames;
    transport->transport_whole_frames_dropped = new_transport_drops;
    if (reserve_preemption) {
        transport->outbound_preempt_on_complete = true;
        transport->outbound_preempt_transport_drops =
            reserved_transport_drops;
        transport->outbound_preempt_latest_effective_drops =
            effective_drops;
    }
    return replace_latest
        ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_LATEST_REPLACED
        : MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

static void packet_from_slot(
    const MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameSlot *slot,
    const MeisoDisplayIngressMediaFrameMetadata *metadata,
    uint32_t offset,
    uint32_t fragment_bytes,
    MeisoDisplayIngressMediaDecodedPacket *out_packet
)
{
    memset(out_packet, 0, sizeof(*out_packet));
    out_packet->binding = transport->binding;
    out_packet->delivery = MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    out_packet->metadata = *metadata;
    out_packet->fragment_offset = offset;
    out_packet->fragment.data = slot->bytes + offset;
    out_packet->fragment.size = fragment_bytes;
    if (offset == 0U) {
        out_packet->flags |= MEISO_DISPLAY_INGRESS_MEDIA_PACKET_BEGIN |
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_RECOVERY;
    }
    if (fragment_bytes == transport->frame_bytes - offset) {
        out_packet->flags |= MEISO_DISPLAY_INGRESS_MEDIA_PACKET_END;
    }
}

static void promote_latest_to_primary(
    MeisoDisplayIngressMediaTransport *transport
);

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_next_outbound(
    MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaPacketView *out_packet
)
{
    MeisoDisplayIngressMediaDecodedPacket packet;
    MeisoDisplayIngressMediaPacketRef ref;
    MeisoDisplayIngressMediaPacketView view;
    MeisoDisplayIngressMediaTransportStatus status;
    const MeisoDisplayIngressMediaFrameSlot *packet_slot;
    MeisoDisplayIngressMediaFrameMetadata packet_metadata;
    uint32_t packet_offset;
    uint32_t remaining;
    uint32_t fragment_bytes;
    uint64_t serial;
    uint64_t preempt_transport_drops = 0U;
    uint64_t preempt_latest_drops = 0U;
    bool boundary_preempt;
    size_t packet_size = 0U;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        out_packet == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, out_packet, sizeof(*out_packet)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (transport->outbound_packet_pending) {
        view.ref = transport->outbound_packet_ref;
        view.bytes.data = transport->packet_scratch;
        view.bytes.size = transport->outbound_packet_size;
        *out_packet = view;
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
    }
    if (!transport->primary.occupied) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_PACKET;
    }
    if (transport->primary_offset >= transport->frame_bytes ||
        !meiso_display_ingress_media_private_checked_add_u64(
            transport->outbound_packet_serial_high_water, 1U, &serial
        )) {
        return transport->primary_offset >= transport->frame_bytes
            ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE
            : MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }

    boundary_preempt = transport->latest.occupied;
    packet_slot = boundary_preempt ? &transport->latest : &transport->primary;
    packet_metadata = packet_slot->metadata;
    packet_offset = boundary_preempt ? 0U : transport->primary_offset;
    if (boundary_preempt &&
        (!meiso_display_ingress_media_private_checked_add_u64(
             transport->transport_whole_frames_dropped, 1U,
             &preempt_transport_drops
         ) ||
         !meiso_display_ingress_media_private_checked_add_u64(
             packet_metadata.producer_dropped_frames, 1U,
             &preempt_latest_drops
         ))) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (boundary_preempt) {
        packet_metadata.producer_dropped_frames = preempt_latest_drops;
    }
    remaining = transport->frame_bytes - packet_offset;
    fragment_bytes = remaining < transport->max_fragment_payload_bytes
        ? remaining
        : transport->max_fragment_payload_bytes;
    packet_from_slot(
        transport, packet_slot, &packet_metadata, packet_offset,
        fragment_bytes, &packet
    );
    status = meiso_display_ingress_media_private_packet_encode(
        &packet,
        (MeisoMutableBuffer){
            transport->packet_scratch,
            transport->packet_scratch_capacity
        },
        &packet_size
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        packet_size > UINT32_MAX) {
        return status == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK
            ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CAPACITY
            : status;
    }

    memset(&ref, 0, sizeof(ref));
    ref.binding = transport->binding;
    ref.serial = serial;
    ref.frame_sequence = packet_metadata.frame_sequence;
    ref.fragment_offset = packet_offset;
    ref.packet_bytes = (uint32_t)packet_size;
    view.ref = ref;
    view.bytes.data = transport->packet_scratch;
    view.bytes.size = packet_size;
    if (boundary_preempt) {
        transport->latest.metadata = packet_metadata;
        transport->transport_whole_frames_dropped = preempt_transport_drops;
        promote_latest_to_primary(transport);
    }
    transport->outbound_packet_ref = ref;
    transport->outbound_packet_size = packet_size;
    transport->outbound_packet_serial_high_water = serial;
    transport->outbound_packet_pending = true;
    transport->outbound_preempt_on_complete = false;
    transport->outbound_preempt_transport_drops = 0U;
    transport->outbound_preempt_latest_effective_drops = 0U;
    *out_packet = view;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

static void promote_latest_to_primary(
    MeisoDisplayIngressMediaTransport *transport
)
{
    MeisoDisplayIngressMediaFrameSlot temporary = transport->primary;

    transport->primary.occupied = false;
    transport->primary_offset = 0U;
    if (transport->latest.occupied) {
        transport->primary = transport->latest;
        transport->latest = temporary;
        transport->latest.occupied = false;
    }
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_complete_outbound(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaPacketRef *expected_packet
)
{
    uint32_t fragment_bytes;
    uint32_t next_offset;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        expected_packet == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, expected_packet, sizeof(*expected_packet)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST ||
        !transport->outbound_packet_pending) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!meiso_display_ingress_media_private_packet_ref_equal(
            expected_packet, &transport->outbound_packet_ref
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT;
    }
    if (expected_packet->packet_bytes <=
        MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }

    fragment_bytes = expected_packet->packet_bytes -
        MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE;
    if (!transport->primary.occupied ||
        transport->primary.metadata.frame_sequence !=
            expected_packet->frame_sequence ||
        transport->primary_offset != expected_packet->fragment_offset ||
        fragment_bytes > transport->frame_bytes - transport->primary_offset) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    next_offset = transport->primary_offset + fragment_bytes;
    if (transport->outbound_preempt_on_complete) {
        if (!transport->latest.occupied ||
            transport->outbound_preempt_transport_drops == 0U) {
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
        }
        transport->latest.metadata.producer_dropped_frames =
            transport->outbound_preempt_latest_effective_drops;
        transport->transport_whole_frames_dropped =
            transport->outbound_preempt_transport_drops;
        promote_latest_to_primary(transport);
    } else {
        transport->primary_offset = next_offset;
        if (next_offset == transport->frame_bytes) {
            promote_latest_to_primary(transport);
        }
    }
    transport->outbound_packet_pending = false;
    transport->outbound_preempt_on_complete = false;
    transport->outbound_packet_size = 0U;
    transport->outbound_preempt_transport_drops = 0U;
    transport->outbound_preempt_latest_effective_drops = 0U;
    memset(
        &transport->outbound_packet_ref, 0,
        sizeof(transport->outbound_packet_ref)
    );
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}
