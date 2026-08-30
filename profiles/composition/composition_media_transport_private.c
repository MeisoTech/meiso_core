#include "meiso_composition_media_transport_private.h"

/* Shared policy primitives own identity checks, bounds and route selection. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

bool meiso_composition_media_transport_private_ranges_overlap(const void *lhs,
                                                              size_t lhs_size,
                                                              const void *rhs,
                                                              size_t rhs_size)
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
    return lhs_begin < rhs_begin + rhs_size && rhs_begin < lhs_begin + lhs_size;
}

bool meiso_composition_media_transport_private_checked_add_u64(
    uint64_t lhs, uint64_t rhs, uint64_t *out_value)
{
    if (out_value == NULL || lhs > UINT64_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool resource_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool nonce_equal(MeisoSessionNonce lhs, MeisoSessionNonce rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

static bool time_equal(MeisoTimePoint lhs, MeisoTimePoint rhs)
{
    return lhs.clock_epoch == rhs.clock_epoch &&
           lhs.monotonic_ns == rhs.monotonic_ns;
}

bool meiso_composition_media_transport_private_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs)
{
    return lhs->pixel_format == rhs->pixel_format && lhs->codec == rhs->codec &&
           lhs->width == rhs->width && lhs->height == rhs->height &&
           lhs->fps_millihz == rhs->fps_millihz &&
           lhs->bitrate_bps == rhs->bitrate_bps &&
           lhs->gop_frames == rhs->gop_frames &&
           lhs->recovery_flags == rhs->recovery_flags;
}

bool meiso_composition_media_transport_private_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
           lhs->catalog_generation == rhs->catalog_generation &&
           endpoint_equal(lhs->endpoint_id, rhs->endpoint_id) &&
           lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
           resource_equal(lhs->resource_id, rhs->resource_id) &&
           lhs->source == rhs->source &&
           lhs->production_epoch == rhs->production_epoch &&
           lhs->media_stream_epoch == rhs->media_stream_epoch &&
           nonce_equal(lhs->channel_nonce, rhs->channel_nonce);
}

bool meiso_composition_media_transport_private_open_equal(
    const MeisoCompositionMediaOpen *lhs, const MeisoCompositionMediaOpen *rhs)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
           lhs->catalog_generation == rhs->catalog_generation;
}

bool meiso_composition_media_transport_private_packet_ref_equal(
    const MeisoCompositionMediaPacketRef *lhs,
    const MeisoCompositionMediaPacketRef *rhs)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
           lhs->catalog_generation == rhs->catalog_generation &&
           lhs->serial == rhs->serial && lhs->kind == rhs->kind &&
           lhs->source == rhs->source &&
           lhs->frame_sequence == rhs->frame_sequence &&
           lhs->fragment_offset == rhs->fragment_offset &&
           lhs->packet_bytes == rhs->packet_bytes;
}

bool meiso_composition_media_transport_private_frame_ref_equal(
    const MeisoCompositionMediaFrameRef *lhs,
    const MeisoCompositionMediaFrameRef *rhs)
{
    return lhs->frame_sequence == rhs->frame_sequence &&
           meiso_composition_media_transport_private_binding_equal(
               &lhs->binding, &rhs->binding);
}

bool meiso_composition_media_transport_private_metadata_equal(
    const MeisoCompositionMediaTransportFrameMetadata *lhs,
    const MeisoCompositionMediaTransportFrameMetadata *rhs)
{
    return meiso_composition_media_transport_private_binding_equal(
               &lhs->binding, &rhs->binding) &&
           lhs->frame_sequence == rhs->frame_sequence &&
           time_equal(lhs->source_time, rhs->source_time) &&
           owner_equal(lhs->applied_scene_owner, rhs->applied_scene_owner) &&
           lhs->applied_scene_state_version ==
               rhs->applied_scene_state_version &&
           lhs->active_plan_generation == rhs->active_plan_generation &&
           lhs->world_generation == rhs->world_generation &&
           lhs->dependency_sequence == rhs->dependency_sequence &&
           lhs->producer_dropped_frames == rhs->producer_dropped_frames &&
           lhs->pixel_format == rhs->pixel_format && lhs->codec == rhs->codec &&
           lhs->width == rhs->width && lhs->height == rhs->height &&
           lhs->flags == rhs->flags &&
           lhs->access_unit_bytes == rhs->access_unit_bytes;
}

bool meiso_composition_media_transport_private_terminal_reason_valid(
    MeisoCompositionMediaTerminalReason reason)
{
    return reason >= MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE &&
           reason <= MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED;
}

bool meiso_composition_media_transport_private_handle_valid(
    const MeisoCompositionMediaTransport *transport)
{
    return transport != NULL &&
           transport->magic == MEISO_COMPOSITION_MEDIA_TRANSPORT_MAGIC &&
           transport->active_route_mask != 0U &&
           (transport->active_route_mask &
            (uint8_t)~MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK) == 0U &&
           transport->arena_begin != NULL &&
           transport->required_bytes >=
               MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES &&
           transport->arena_extent >= transport->required_bytes;
}

bool meiso_composition_media_transport_private_overlaps(
    const MeisoCompositionMediaTransport *transport, const void *data,
    size_t size)
{
    return meiso_composition_media_transport_private_handle_valid(transport) &&
           meiso_composition_media_transport_private_ranges_overlap(
               transport->arena_begin, transport->arena_extent, data, size);
}

uint32_t meiso_composition_media_transport_private_access_unit_bytes(
    const MeisoCompositionMediaDesired *desired)
{
    const uint64_t pixels = (uint64_t)desired->width * desired->height;
    uint64_t bytes = 0U;
    switch (desired->pixel_format) {
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888:
        bytes = pixels * 4U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565:
    case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV:
        bytes = pixels * 2U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_NV12:
        bytes = pixels + pixels / 2U;
        break;
    default:
        return 0U;
    }
    return bytes <= UINT32_MAX ? (uint32_t)bytes : 0U;
}

bool meiso_composition_media_transport_private_scene_fields_valid(
    const MeisoCompositionMediaUnit *unit)
{
    if (unit->binding.source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return meiso_owner_scope_valid(unit->applied_scene_owner) &&
               unit->applied_scene_owner.role == MEISO_OWNER_ROLE_HOST &&
               unit->applied_scene_state_version != 0U &&
               unit->active_plan_generation != 0U &&
               unit->world_generation != 0U;
    }
    return unit->applied_scene_owner.role == 0U &&
           unit->applied_scene_owner.incarnation == 0U &&
           unit->applied_scene_state_version == 0U &&
           unit->active_plan_generation == 0U && unit->world_generation == 0U;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_private_binding_difference(
    const MeisoCompositionMediaBinding *expected,
    const MeisoCompositionMediaBinding *actual)
{
    if (!meiso_session_id_equal(expected->session_id, actual->session_id) ||
        expected->catalog_generation != actual->catalog_generation ||
        !endpoint_equal(expected->endpoint_id, actual->endpoint_id) ||
        expected->endpoint_incarnation != actual->endpoint_incarnation ||
        !resource_equal(expected->resource_id, actual->resource_id) ||
        expected->source != actual->source) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    if (expected->production_epoch != actual->production_epoch ||
        expected->media_stream_epoch != actual->media_stream_epoch) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_EPOCH_MISMATCH;
    }
    if (!nonce_equal(expected->channel_nonce, actual->channel_nonce)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NONCE_MISMATCH;
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

int meiso_composition_media_transport_private_route_index(
    MeisoCompositionMediaSource source)
{
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return 0;
    }
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return 1;
    }
    return -1;
}

uint8_t meiso_composition_media_transport_private_route_bit(size_t route_index)
{
    return (uint8_t)(UINT8_C(1) << (uint8_t)route_index);
}

bool meiso_composition_media_transport_private_route_selected(
    uint8_t active_route_mask, size_t route_index)
{
    return (active_route_mask &
            meiso_composition_media_transport_private_route_bit(route_index)) !=
           0U;
}

bool meiso_composition_media_transport_private_next_selected_route(
    uint8_t active_route_mask, size_t start, uint8_t *out_route_index)
{
    size_t route_index;
    if (out_route_index == NULL) {
        return false;
    }
    for (route_index = start;
         route_index < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT;
         ++route_index) {
        if (meiso_composition_media_transport_private_route_selected(
                active_route_mask, route_index)) {
            *out_route_index = (uint8_t)route_index;
            return true;
        }
    }
    return false;
}
