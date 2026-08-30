#ifndef MEISO_COMPOSITION_MEDIA_TRANSPORT_PRIVATE_H
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_PRIVATE_H

/*
 * Build-private and non-installed. The caller owns the arena containing the
 * transport, route slots and packet scratch; configuration is copied at init.
 * Packet/latest views borrow arena bytes until documented progress or release.
 * Every entry point and cross-TU helper is single-threaded and non-reentrant.
 */

#include <meiso/profiles/composition/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_COMPOSITION_MEDIA_TRANSPORT_MAGIC = 0x4d545250U,      /* MTRP */
    MEISO_COMPOSITION_MEDIA_TRANSPORT_OPEN_MAGIC = 0x4d4f504eU, /* MOPN */
    MEISO_COMPOSITION_MEDIA_TRANSPORT_BIND_MAGIC = 0x4d424e44U, /* MBND */
    MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_MAGIC = 0x4d46524dU /* MFRM */
};

typedef struct {
    MeisoCompositionMediaBinding binding;
    uint64_t frame_sequence;
    MeisoTimePoint source_time;
    MeisoOwnerScope applied_scene_owner;
    uint64_t applied_scene_state_version;
    uint64_t active_plan_generation;
    uint64_t world_generation;
    uint64_t dependency_sequence;
    uint64_t producer_dropped_frames;
    MeisoCompositionMediaPixelFormat pixel_format;
    MeisoCompositionMediaCodec codec;
    uint16_t width;
    uint16_t height;
    MeisoCompositionMediaFrameFlags flags;
    uint32_t access_unit_bytes;
} MeisoCompositionMediaTransportFrameMetadata;

typedef struct {
    bool occupied;
    MeisoCompositionMediaTransportFrameMetadata metadata;
    uint8_t *bytes;
    uint32_t capacity;
} MeisoCompositionMediaTransportFrameSlot;

typedef struct {
    MeisoCompositionMediaRouteConfig config;
    MeisoCompositionMediaTransportFrameSlot primary;
    MeisoCompositionMediaTransportFrameSlot latest;
    uint32_t primary_offset;
    uint64_t submitted_sequence_high_water;
    uint64_t submitted_producer_drop_high_water;
    uint64_t transport_whole_frames_dropped;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
} MeisoCompositionMediaTransportRouteState;

struct MeisoCompositionMediaTransport {
    uint32_t magic;
    MeisoSessionRole local_role;
    MeisoCompositionMediaTransportPhase phase;
    MeisoCompositionMediaTerminalReason terminal_reason;
    MeisoCompositionMediaOpen scope;
    uint8_t active_route_mask;
    uint8_t bound_route_mask;
    uint8_t bind_send_index;
    uint8_t fragment_rr_cursor;
    bool outbound_packet_pending;
    MeisoCompositionMediaPacketRef outbound_packet_ref;
    size_t outbound_packet_size;
    uint64_t outbound_packet_serial_high_water;
    uint8_t *arena_begin;
    size_t arena_extent;
    size_t required_bytes;
    uint8_t *packet_scratch;
    uint32_t packet_scratch_capacity;
    MeisoCompositionMediaTransportRouteState
        routes[MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT];
};

_Static_assert(sizeof(struct MeisoCompositionMediaTransport) <=
                   MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES,
               "media transport metadata arena");

bool meiso_composition_media_transport_private_ranges_overlap(const void *lhs,
                                                              size_t lhs_size,
                                                              const void *rhs,
                                                              size_t rhs_size);
bool meiso_composition_media_transport_private_checked_add_u64(
    uint64_t lhs, uint64_t rhs, uint64_t *out_value);
bool meiso_composition_media_transport_private_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs);
bool meiso_composition_media_transport_private_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs);
bool meiso_composition_media_transport_private_open_equal(
    const MeisoCompositionMediaOpen *lhs, const MeisoCompositionMediaOpen *rhs);
bool meiso_composition_media_transport_private_packet_ref_equal(
    const MeisoCompositionMediaPacketRef *lhs,
    const MeisoCompositionMediaPacketRef *rhs);
bool meiso_composition_media_transport_private_frame_ref_equal(
    const MeisoCompositionMediaFrameRef *lhs,
    const MeisoCompositionMediaFrameRef *rhs);
bool meiso_composition_media_transport_private_metadata_equal(
    const MeisoCompositionMediaTransportFrameMetadata *lhs,
    const MeisoCompositionMediaTransportFrameMetadata *rhs);
bool meiso_composition_media_transport_private_terminal_reason_valid(
    MeisoCompositionMediaTerminalReason reason);
bool meiso_composition_media_transport_private_handle_valid(
    const MeisoCompositionMediaTransport *transport);
bool meiso_composition_media_transport_private_overlaps(
    const MeisoCompositionMediaTransport *transport, const void *data,
    size_t size);
uint32_t meiso_composition_media_transport_private_access_unit_bytes(
    const MeisoCompositionMediaDesired *desired);
bool meiso_composition_media_transport_private_scene_fields_valid(
    const MeisoCompositionMediaUnit *unit);
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_private_binding_difference(
    const MeisoCompositionMediaBinding *expected,
    const MeisoCompositionMediaBinding *actual);
int meiso_composition_media_transport_private_route_index(
    MeisoCompositionMediaSource source);
uint8_t meiso_composition_media_transport_private_route_bit(size_t route_index);
bool meiso_composition_media_transport_private_route_selected(
    uint8_t active_route_mask, size_t route_index);
bool meiso_composition_media_transport_private_next_selected_route(
    uint8_t active_route_mask, size_t start, uint8_t *out_route_index);

#endif
