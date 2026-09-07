#ifndef MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PRIVATE_H
#define MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PRIVATE_H

/* Build-private, single-threaded implementation contract. */

#include <meiso/profiles/display_ingress/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MAGIC = 0x44495452U, /* DITR */
    MEISO_DISPLAY_INGRESS_MEDIA_PACKET_MAGIC = 0x44494652U    /* DIFR */
};

typedef struct {
    uint64_t frame_sequence;
    MeisoTimePoint source_time;
    MeisoDeadline sender_deadline;
    uint64_t producer_dropped_frames;
    uint32_t frame_bytes;
} MeisoDisplayIngressMediaFrameMetadata;

typedef struct {
    bool occupied;
    MeisoDisplayIngressMediaFrameMetadata metadata;
    uint8_t *bytes;
    uint32_t capacity;
} MeisoDisplayIngressMediaFrameSlot;

typedef struct {
    MeisoDisplayIngressMediaBinding binding;
    MeisoRuntimeDeliveryClass delivery;
    MeisoDisplayIngressMediaPacketFlags flags;
    MeisoDisplayIngressMediaFrameMetadata metadata;
    uint32_t fragment_offset;
    MeisoByteView fragment;
} MeisoDisplayIngressMediaDecodedPacket;

struct MeisoDisplayIngressMediaTransport {
    uint32_t magic;
    MeisoSessionRole local_role;
    MeisoDisplayIngressMediaTransportPhase phase;
    MeisoDisplayIngressMediaTerminalReason terminal_reason;
    MeisoStreamDeadlinePolicy deadline_policy;
    MeisoDisplayIngressMediaBinding binding;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t frame_bytes; /* slot capacity; actual bytes live in slot metadata */
    MeisoDisplayIngressFrameFormat frame_format;
    uint32_t max_fragment_payload_bytes;
    uint8_t *arena_begin;
    size_t arena_extent;
    size_t required_bytes;
    MeisoDisplayIngressMediaFrameSlot primary;
    MeisoDisplayIngressMediaFrameSlot latest;
    uint32_t primary_offset;
    uint8_t *packet_scratch;
    uint32_t packet_scratch_capacity;
    bool outbound_packet_pending;
    bool outbound_preempt_on_complete;
    MeisoDisplayIngressMediaPacketRef outbound_packet_ref;
    size_t outbound_packet_size;
    uint64_t outbound_preempt_transport_drops;
    uint64_t outbound_preempt_latest_effective_drops;
    uint64_t outbound_packet_serial_high_water;
    uint64_t submitted_sequence_high_water;
    uint64_t submitted_producer_drop_high_water;
    uint64_t transport_whole_frames_dropped;
    uint64_t observed_sequence_high_water;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
    uint64_t expired_frames_dropped;
};

_Static_assert(sizeof(struct MeisoDisplayIngressMediaTransport) <=
                   MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES,
               "display ingress media metadata arena");

bool meiso_display_ingress_media_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
bool meiso_display_ingress_media_private_checked_add_size(
    size_t lhs,
    size_t rhs,
    size_t *out_value
);
bool meiso_display_ingress_media_private_checked_add_u64(
    uint64_t lhs,
    uint64_t rhs,
    uint64_t *out_value
);
bool meiso_display_ingress_media_private_handle_valid(
    const MeisoDisplayIngressMediaTransport *transport
);
bool meiso_display_ingress_media_private_overlaps(
    const MeisoDisplayIngressMediaTransport *transport,
    const void *data,
    size_t size
);
bool meiso_display_ingress_media_private_binding_equal(
    const MeisoDisplayIngressMediaBinding *lhs,
    const MeisoDisplayIngressMediaBinding *rhs
);
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_binding_difference(
    const MeisoDisplayIngressMediaBinding *expected,
    const MeisoDisplayIngressMediaBinding *actual
);
bool meiso_display_ingress_media_private_metadata_equal(
    const MeisoDisplayIngressMediaFrameMetadata *lhs,
    const MeisoDisplayIngressMediaFrameMetadata *rhs
);
bool meiso_display_ingress_media_private_packet_ref_equal(
    const MeisoDisplayIngressMediaPacketRef *lhs,
    const MeisoDisplayIngressMediaPacketRef *rhs
);
bool meiso_display_ingress_media_private_frame_ref_equal(
    const MeisoDisplayIngressMediaFrameRef *lhs,
    const MeisoDisplayIngressMediaFrameRef *rhs
);
bool meiso_display_ingress_media_private_terminal_reason_valid(
    MeisoDisplayIngressMediaTerminalReason reason
);
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_deadline_status(
    MeisoStreamDeadlinePolicy policy,
    MeisoDeadline deadline,
    const MeisoStreamAdmission *admission
);
void meiso_display_ingress_media_private_enter_terminal(
    MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaTerminalReason reason
);

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_packet_encode(
    const MeisoDisplayIngressMediaDecodedPacket *packet,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_packet_decode(
    MeisoByteView input,
    MeisoDisplayIngressMediaDecodedPacket *out_packet
);

#endif
