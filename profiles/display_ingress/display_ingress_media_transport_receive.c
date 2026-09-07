#include "display_ingress_media_transport_private.h"

/* Edge-side routing/production-scope admission and latest reassembly. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool rgba_fragment_opaque(MeisoByteView fragment, uint32_t offset)
{
    size_t index;

    for (index = 0U; index < fragment.size; ++index) {
        if ((((size_t)offset + index) & 3U) == 3U &&
            fragment.data[index] != UINT8_MAX) {
            return false;
        }
    }
    return true;
}

static bool precompute_observation(
    const MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameMetadata *metadata,
    uint64_t *out_sequence_gaps
)
{
    const uint64_t gap = transport->observed_sequence_high_water == 0U
        ? metadata->frame_sequence - 1U
        : metadata->frame_sequence -
              transport->observed_sequence_high_water - 1U;

    return metadata->frame_sequence >
            transport->observed_sequence_high_water &&
        metadata->producer_dropped_frames >=
            transport->producer_drop_high_water &&
        meiso_display_ingress_media_private_checked_add_u64(
            transport->sequence_gap_frames, gap, out_sequence_gaps
        );
}

static bool precompute_publication(
    const MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameMetadata *metadata,
    uint64_t *out_sequence_gaps,
    uint64_t *out_latest_drops
)
{
    return precompute_observation(
               transport, metadata, out_sequence_gaps
           ) &&
        meiso_display_ingress_media_private_checked_add_u64(
            transport->unread_latest_frames_dropped,
            transport->latest.occupied ? 1U : 0U,
            out_latest_drops
        );
}

static void publish_primary(
    MeisoDisplayIngressMediaTransport *transport,
    uint64_t sequence_gaps,
    uint64_t latest_drops
)
{
    MeisoDisplayIngressMediaFrameSlot temporary = transport->latest;
    const uint64_t sequence = transport->primary.metadata.frame_sequence;
    const uint64_t producer_drops =
        transport->primary.metadata.producer_dropped_frames;

    transport->latest = transport->primary;
    transport->primary = temporary;
    transport->primary.occupied = false;
    transport->primary_offset = 0U;
    transport->observed_sequence_high_water = sequence;
    transport->published_sequence_high_water = sequence;
    transport->producer_drop_high_water = producer_drops;
    transport->sequence_gap_frames = sequence_gaps;
    transport->unread_latest_frames_dropped = latest_drops;
}

static MeisoDisplayIngressMediaTransportStatus publish_validated_primary(
    MeisoDisplayIngressMediaTransport *transport,
    uint64_t sequence_gaps,
    uint64_t latest_drops
)
{
    if (transport->frame_format == MEISO_DISPLAY_INGRESS_FRAME_H264_IDR &&
        !meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){
            transport->primary.bytes, transport->primary.metadata.frame_bytes
        })) {
        transport->primary.occupied = false;
        transport->primary_offset = 0U;
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }
    publish_primary(transport, sequence_gaps, latest_drops);
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED;
}

static MeisoDisplayIngressMediaTransportStatus expire_frame(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameMetadata *metadata
)
{
    uint64_t sequence_gaps;
    uint64_t expired_drops;
    uint64_t reassembly_drops = transport->reassembly_frames_dropped;
    const bool replaces_other_partial = transport->primary.occupied &&
        transport->primary.metadata.frame_sequence !=
            metadata->frame_sequence;

    if (!precompute_observation(
            transport, metadata, &sequence_gaps
        ) ||
        !meiso_display_ingress_media_private_checked_add_u64(
            transport->expired_frames_dropped, 1U, &expired_drops
        ) ||
        (replaces_other_partial &&
         !meiso_display_ingress_media_private_checked_add_u64(
             reassembly_drops, 1U, &reassembly_drops
         ))) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }

    transport->primary.occupied = false;
    transport->primary_offset = 0U;
    transport->observed_sequence_high_water = metadata->frame_sequence;
    transport->producer_drop_high_water =
        metadata->producer_dropped_frames;
    transport->sequence_gap_frames = sequence_gaps;
    transport->expired_frames_dropped = expired_drops;
    transport->reassembly_frames_dropped = reassembly_drops;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DROPPED_EXPIRED;
}

static bool packet_sequence_admissible(
    const MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameMetadata *metadata
)
{
    if (metadata->frame_sequence <=
            transport->observed_sequence_high_water ||
        metadata->producer_dropped_frames <
            transport->producer_drop_high_water) {
        return false;
    }
    if (!transport->primary.occupied) {
        return true;
    }
    if (metadata->frame_sequence <
        transport->primary.metadata.frame_sequence) {
        return false;
    }
    return metadata->frame_sequence ==
            transport->primary.metadata.frame_sequence
        ? meiso_display_ingress_media_private_metadata_equal(
              &transport->primary.metadata, metadata
          )
        : metadata->producer_dropped_frames >=
              transport->primary.metadata.producer_dropped_frames;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_receive_packet(
    MeisoDisplayIngressMediaTransport *transport,
    MeisoByteView packet_bytes,
    const MeisoStreamAdmission *admission
)
{
    MeisoDisplayIngressMediaDecodedPacket packet;
    MeisoDisplayIngressMediaTransportStatus status;
    uint32_t end_offset;
    bool finish;
    uint64_t sequence_gaps = 0U;
    uint64_t latest_drops = 0U;
    uint64_t reassembly_drops = 0U;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        packet_bytes.data == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, packet_bytes.data, packet_bytes.size
        ) ||
        (admission != NULL &&
         meiso_display_ingress_media_private_overlaps(
             transport, admission, sizeof(*admission)
         ))) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }

    status = meiso_display_ingress_media_private_packet_decode(
        packet_bytes, &packet
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return status;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    status = meiso_display_ingress_media_private_binding_difference(
        &transport->binding, &packet.binding
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        meiso_display_ingress_media_private_enter_terminal(
            transport,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING
        );
        return status;
    }
    if (packet.delivery != MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA ||
        packet.metadata.frame_bytes > transport->frame_bytes ||
        packet.fragment.size > transport->max_fragment_payload_bytes ||
        (transport->frame_format == MEISO_DISPLAY_INGRESS_FRAME_RGBA8888 &&
         (packet.metadata.frame_bytes != transport->frame_bytes ||
          !rgba_fragment_opaque(packet.fragment, packet.fragment_offset)))) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }
    if (!packet_sequence_admissible(transport, &packet.metadata)) {
        if (transport->primary.occupied &&
            packet.metadata.frame_sequence ==
                transport->primary.metadata.frame_sequence) {
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT;
        }
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }

    status = meiso_display_ingress_media_private_deadline_status(
        transport->deadline_policy, packet.metadata.sender_deadline,
        admission
    );
    if (status == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DROPPED_EXPIRED) {
        return expire_frame(transport, &packet.metadata);
    }
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return status;
    }

    end_offset = packet.fragment_offset + (uint32_t)packet.fragment.size;
    finish = end_offset == packet.metadata.frame_bytes;
    if (!transport->primary.occupied) {
        if (packet.fragment_offset != 0U) {
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY;
        }
        if (finish &&
            !precompute_publication(
                transport, &packet.metadata, &sequence_gaps, &latest_drops
            )) {
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
        memmove(
            transport->primary.bytes, packet.fragment.data,
            packet.fragment.size
        );
        transport->primary.metadata = packet.metadata;
        transport->primary.occupied = true;
        transport->primary_offset = end_offset;
        if (finish) {
            return publish_validated_primary(transport, sequence_gaps, latest_drops);
        }
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED;
    }

    if (packet.metadata.frame_sequence ==
        transport->primary.metadata.frame_sequence) {
        if (packet.fragment_offset != transport->primary_offset) {
            if (!meiso_display_ingress_media_private_checked_add_u64(
                    transport->reassembly_frames_dropped, 1U,
                    &reassembly_drops
                )) {
                return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
            }
            transport->primary.occupied = false;
            transport->primary_offset = 0U;
            transport->reassembly_frames_dropped = reassembly_drops;
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY;
        }
        if (finish &&
            !precompute_publication(
                transport, &packet.metadata, &sequence_gaps, &latest_drops
            )) {
            return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
        memmove(
            transport->primary.bytes + packet.fragment_offset,
            packet.fragment.data, packet.fragment.size
        );
        transport->primary_offset = end_offset;
        if (finish) {
            return publish_validated_primary(transport, sequence_gaps, latest_drops);
        }
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED;
    }

    if (!meiso_display_ingress_media_private_checked_add_u64(
            transport->reassembly_frames_dropped, 1U,
            &reassembly_drops
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (packet.fragment_offset != 0U) {
        transport->primary.occupied = false;
        transport->primary_offset = 0U;
        transport->reassembly_frames_dropped = reassembly_drops;
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY;
    }
    if (finish &&
        !precompute_publication(
            transport, &packet.metadata, &sequence_gaps, &latest_drops
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    memmove(
        transport->primary.bytes, packet.fragment.data, packet.fragment.size
    );
    transport->primary.metadata = packet.metadata;
    transport->primary.occupied = true;
    transport->primary_offset = end_offset;
    transport->reassembly_frames_dropped = reassembly_drops;
    if (finish) {
        return publish_validated_primary(transport, sequence_gaps, latest_drops);
    }
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_REASSEMBLY_REPLACED;
}
