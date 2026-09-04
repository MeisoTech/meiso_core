#include "display_ingress_media_transport_private.h"

/* Shared identity, alias, deadline and terminal-state policy. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool meiso_display_ingress_media_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
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
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

bool meiso_display_ingress_media_private_checked_add_size(
    size_t lhs,
    size_t rhs,
    size_t *out_value
)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

bool meiso_display_ingress_media_private_checked_add_u64(
    uint64_t lhs,
    uint64_t rhs,
    uint64_t *out_value
)
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

bool meiso_display_ingress_media_private_binding_equal(
    const MeisoDisplayIngressMediaBinding *lhs,
    const MeisoDisplayIngressMediaBinding *rhs
)
{
    return meiso_session_id_equal(
               lhs->runtime.session_id, rhs->runtime.session_id
           ) &&
        lhs->runtime.route.catalog_generation ==
            rhs->runtime.route.catalog_generation &&
        endpoint_equal(
            lhs->runtime.route.endpoint_id, rhs->runtime.route.endpoint_id
        ) &&
        lhs->runtime.route.endpoint_incarnation ==
            rhs->runtime.route.endpoint_incarnation &&
        resource_equal(
            lhs->runtime.route.resource_id, rhs->runtime.route.resource_id
        ) &&
        lhs->runtime.route.facet == rhs->runtime.route.facet &&
        meiso_resource_descriptor_equal(
            &lhs->runtime.resource, &rhs->runtime.resource
        ) &&
        lhs->runtime.production_epoch == rhs->runtime.production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        nonce_equal(lhs->channel_nonce, rhs->channel_nonce) &&
        owner_equal(lhs->producer, rhs->producer);
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_binding_difference(
    const MeisoDisplayIngressMediaBinding *expected,
    const MeisoDisplayIngressMediaBinding *actual
)
{
    if (!meiso_session_id_equal(
            expected->runtime.session_id, actual->runtime.session_id
        ) ||
        expected->runtime.route.catalog_generation !=
            actual->runtime.route.catalog_generation ||
        !endpoint_equal(
            expected->runtime.route.endpoint_id,
            actual->runtime.route.endpoint_id
        ) ||
        expected->runtime.route.endpoint_incarnation !=
            actual->runtime.route.endpoint_incarnation ||
        !resource_equal(
            expected->runtime.route.resource_id,
            actual->runtime.route.resource_id
        ) ||
        expected->runtime.route.facet != actual->runtime.route.facet) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    if (expected->runtime.production_epoch !=
            actual->runtime.production_epoch ||
        expected->media_stream_epoch != actual->media_stream_epoch) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_EPOCH_MISMATCH;
    }
    if (!nonce_equal(expected->channel_nonce, actual->channel_nonce)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NONCE_MISMATCH;
    }
    if (!owner_equal(expected->producer, actual->producer)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OWNER_MISMATCH;
    }
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

bool meiso_display_ingress_media_private_metadata_equal(
    const MeisoDisplayIngressMediaFrameMetadata *lhs,
    const MeisoDisplayIngressMediaFrameMetadata *rhs
)
{
    return lhs->frame_sequence == rhs->frame_sequence &&
        time_equal(lhs->source_time, rhs->source_time) &&
        lhs->sender_deadline.present == rhs->sender_deadline.present &&
        (!lhs->sender_deadline.present || time_equal(
            lhs->sender_deadline.value, rhs->sender_deadline.value
        )) &&
        lhs->producer_dropped_frames == rhs->producer_dropped_frames &&
        lhs->frame_bytes == rhs->frame_bytes;
}

bool meiso_display_ingress_media_private_packet_ref_equal(
    const MeisoDisplayIngressMediaPacketRef *lhs,
    const MeisoDisplayIngressMediaPacketRef *rhs
)
{
    return meiso_display_ingress_media_private_binding_equal(
               &lhs->binding, &rhs->binding
           ) &&
        lhs->serial == rhs->serial &&
        lhs->frame_sequence == rhs->frame_sequence &&
        lhs->fragment_offset == rhs->fragment_offset &&
        lhs->packet_bytes == rhs->packet_bytes;
}

bool meiso_display_ingress_media_private_frame_ref_equal(
    const MeisoDisplayIngressMediaFrameRef *lhs,
    const MeisoDisplayIngressMediaFrameRef *rhs
)
{
    return lhs->frame_sequence == rhs->frame_sequence &&
        meiso_display_ingress_media_private_binding_equal(
            &lhs->binding, &rhs->binding
        );
}

bool meiso_display_ingress_media_private_terminal_reason_valid(
    MeisoDisplayIngressMediaTerminalReason reason
)
{
    return reason >= MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE &&
        reason <= MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING;
}

bool meiso_display_ingress_media_private_handle_valid(
    const MeisoDisplayIngressMediaTransport *transport
)
{
    if (transport == NULL ||
        transport->magic != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MAGIC ||
        (transport->local_role != MEISO_SESSION_ROLE_HOST &&
         transport->local_role != MEISO_SESSION_ROLE_EDGE) ||
        (transport->phase !=
             MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE &&
         transport->phase !=
             MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) ||
        transport->arena_begin != (const uint8_t *)transport ||
        transport->required_bytes <
            MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES ||
        transport->arena_extent < transport->required_bytes ||
        transport->frame_bytes == 0U ||
        transport->primary.capacity != transport->frame_bytes ||
        transport->latest.capacity != transport->frame_bytes ||
        transport->max_fragment_payload_bytes == 0U ||
        transport->packet_scratch_capacity !=
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE +
                transport->max_fragment_payload_bytes) {
        return false;
    }
    return transport->phase ==
               MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE
        ? transport->terminal_reason == 0U
        : meiso_display_ingress_media_private_terminal_reason_valid(
              transport->terminal_reason
          );
}

bool meiso_display_ingress_media_private_overlaps(
    const MeisoDisplayIngressMediaTransport *transport,
    const void *data,
    size_t size
)
{
    return meiso_display_ingress_media_private_handle_valid(transport) &&
        meiso_display_ingress_media_private_ranges_overlap(
            transport->arena_begin, transport->arena_extent, data, size
        );
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_deadline_status(
    MeisoStreamDeadlinePolicy policy,
    MeisoDeadline deadline,
    const MeisoStreamAdmission *admission
)
{
    if (!meiso_deadline_valid(deadline)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }
    if (!deadline.present) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
    }
    if (policy == MEISO_STREAM_DEADLINE_UNSUPPORTED) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNSUPPORTED;
    }
    if (policy != MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE ||
        admission == NULL || admission->comparable == 0U ||
        admission->uncertainty_acceptable == 0U ||
        !meiso_time_point_valid(admission->local_deadline) ||
        !meiso_time_point_valid(admission->local_now) ||
        admission->local_deadline.clock_epoch !=
            admission->local_now.clock_epoch) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNCOMPARABLE;
    }
    return admission->local_now.monotonic_ns >
            admission->local_deadline.monotonic_ns
        ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DROPPED_EXPIRED
        : MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

void meiso_display_ingress_media_private_enter_terminal(
    MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaTerminalReason reason
)
{
    memset(transport->primary.bytes, 0, transport->primary.capacity);
    memset(transport->latest.bytes, 0, transport->latest.capacity);
    memset(
        transport->packet_scratch, 0, transport->packet_scratch_capacity
    );
    transport->primary.occupied = false;
    transport->latest.occupied = false;
    transport->primary_offset = 0U;
    transport->outbound_packet_pending = false;
    transport->outbound_preempt_on_complete = false;
    transport->outbound_packet_size = 0U;
    transport->outbound_preempt_transport_drops = 0U;
    transport->outbound_preempt_latest_effective_drops = 0U;
    memset(
        &transport->outbound_packet_ref, 0,
        sizeof(transport->outbound_packet_ref)
    );
    transport->phase =
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL;
    transport->terminal_reason = reason;
}
