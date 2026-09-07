#include "display_ingress_media_transport_private.h"

/* Arena lifecycle, latest-frame borrow, termination and snapshots. */

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

static bool session_id_valid(MeisoSessionId value)
{
    return meiso_session_nonce_valid(value.host_nonce) &&
        meiso_session_nonce_valid(value.edge_nonce);
}

static bool pointer_aligned(const void *pointer, size_t alignment)
{
    return pointer != NULL && alignment != 0U &&
        ((uintptr_t)pointer % alignment) == 0U;
}

static bool calculate_frame_bytes(
    uint32_t width,
    uint32_t height,
    uint32_t *out_frame_bytes
)
{
    const uint64_t pixels = (uint64_t)width * height;

    if (out_frame_bytes == NULL || width == 0U || height == 0U ||
        pixels > UINT32_MAX / 4U) {
        return false;
    }
    *out_frame_bytes = (uint32_t)(pixels * 4U);
    return true;
}

static bool config_valid(
    const MeisoDisplayIngressMediaTransportConfig *config,
    uint32_t *out_frame_bytes
)
{
    const MeisoDisplayIngressMediaBinding *binding;
    const MeisoEndpointId *expected_endpoint;
    const MeisoResourceId *expected_resource;
    const MeisoSchemaRef *expected_schema;
    uint32_t frame_bytes = 0U;

    if (config == NULL || out_frame_bytes == NULL ||
        (config->local_role != MEISO_SESSION_ROLE_HOST &&
         config->local_role != MEISO_SESSION_ROLE_EDGE) ||
        (config->deadline_policy != MEISO_STREAM_DEADLINE_UNSUPPORTED &&
         config->deadline_policy !=
             MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE) ||
        !calculate_frame_bytes(
            config->input_width, config->input_height, &frame_bytes
        ) ||
        config->max_fragment_payload_bytes == 0U ||
        config->max_fragment_payload_bytes >
            UINT32_MAX -
                MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE) {
        return false;
    }

    binding = &config->binding;
    expected_endpoint = meiso_display_ingress_endpoint();
    expected_resource = meiso_display_ingress_resource_id(
        MEISO_DISPLAY_INGRESS_ROUTE_MEDIA
    );
    expected_schema = meiso_display_ingress_resource_schema_for_format(
        MEISO_DISPLAY_INGRESS_ROUTE_MEDIA, config->frame_format
    );
    if (config->frame_format == MEISO_DISPLAY_INGRESS_FRAME_H264_IDR) {
        if (config->input_width & 1U || config->input_height & 1U ||
            binding->runtime.resource.max_payload_size == 0U ||
            binding->runtime.resource.max_payload_size > MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES)
            return false;
        frame_bytes = (uint32_t)binding->runtime.resource.max_payload_size;
    }
    if (!session_id_valid(binding->runtime.session_id) ||
        expected_endpoint == NULL || expected_resource == NULL ||
        expected_schema == NULL ||
        binding->runtime.route.catalog_generation == 0U ||
        !endpoint_equal(
            binding->runtime.route.endpoint_id, *expected_endpoint
        ) ||
        binding->runtime.route.endpoint_incarnation == 0U ||
        !resource_equal(
            binding->runtime.route.resource_id,
            *expected_resource
        ) ||
        binding->runtime.route.facet != MEISO_RUNTIME_FACET_DATA ||
        binding->runtime.production_epoch == 0U ||
        meiso_resource_descriptor_validate(&binding->runtime.resource) !=
            MEISO_DESCRIPTOR_VALID ||
        !resource_equal(
            binding->runtime.resource.id,
            binding->runtime.route.resource_id
        ) ||
        binding->runtime.resource.kind != MEISO_RESOURCE_KIND_STREAM ||
        binding->runtime.resource.owner != MEISO_OWNER_ROLE_HOST ||
        !meiso_schema_ref_equal(
            binding->runtime.resource.schema,
            *expected_schema
        ) ||
        binding->runtime.resource.max_payload_size < frame_bytes ||
        binding->media_stream_epoch == 0U ||
        binding->media_stream_epoch != binding->runtime.production_epoch ||
        !meiso_session_nonce_valid(binding->channel_nonce) ||
        !meiso_owner_scope_valid(binding->producer) ||
        binding->producer.role != MEISO_OWNER_ROLE_HOST) {
        return false;
    }
    *out_frame_bytes = frame_bytes;
    return true;
}

static bool calculate_required(
    const MeisoDisplayIngressMediaTransportConfig *config,
    size_t *out_required,
    uint32_t *out_frame_bytes,
    uint32_t *out_packet_capacity
)
{
    size_t required =
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES;
    uint32_t frame_bytes = 0U;
    size_t packet_capacity;

    if (!config_valid(config, &frame_bytes) ||
        !meiso_display_ingress_media_private_checked_add_size(
            required, frame_bytes, &required
        ) ||
        !meiso_display_ingress_media_private_checked_add_size(
            required, frame_bytes, &required
        ) ||
        !meiso_display_ingress_media_private_checked_add_size(
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE,
            config->max_fragment_payload_bytes, &packet_capacity
        ) ||
        packet_capacity > UINT32_MAX ||
        !meiso_display_ingress_media_private_checked_add_size(
            required, packet_capacity, &required
        )) {
        return false;
    }
    *out_required = required;
    *out_frame_bytes = frame_bytes;
    *out_packet_capacity = (uint32_t)packet_capacity;
    return true;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_memory_required(
    const MeisoDisplayIngressMediaTransportConfig *config,
    size_t *out_size,
    size_t *out_alignment
)
{
    size_t required = 0U;
    uint32_t frame_bytes = 0U;
    uint32_t packet_capacity = 0U;

    if (config == NULL || out_size == NULL || out_alignment == NULL ||
        meiso_display_ingress_media_private_ranges_overlap(
            config, sizeof(*config), out_size, sizeof(*out_size)
        ) ||
        meiso_display_ingress_media_private_ranges_overlap(
            config, sizeof(*config), out_alignment, sizeof(*out_alignment)
        ) ||
        meiso_display_ingress_media_private_ranges_overlap(
            out_size, sizeof(*out_size), out_alignment,
            sizeof(*out_alignment)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!calculate_required(
            config, &required, &frame_bytes, &packet_capacity
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_CONFIG;
    }
    (void)frame_bytes;
    (void)packet_capacity;
    *out_size = required;
    *out_alignment = alignof(max_align_t);
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_init(
    const MeisoDisplayIngressMediaTransportConfig *config,
    MeisoMutableBuffer arena,
    MeisoDisplayIngressMediaTransport **out_transport
)
{
    MeisoDisplayIngressMediaTransport value;
    MeisoDisplayIngressMediaTransport *transport;
    size_t required = 0U;
    size_t offset;
    uint32_t frame_bytes = 0U;
    uint32_t packet_capacity = 0U;

    if (config == NULL || arena.data == NULL || out_transport == NULL ||
        meiso_display_ingress_media_private_ranges_overlap(
            config, sizeof(*config), arena.data, arena.capacity
        ) ||
        meiso_display_ingress_media_private_ranges_overlap(
            out_transport, sizeof(*out_transport), arena.data, arena.capacity
        ) ||
        meiso_display_ingress_media_private_ranges_overlap(
            config, sizeof(*config), out_transport, sizeof(*out_transport)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!calculate_required(
            config, &required, &frame_bytes, &packet_capacity
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_CONFIG;
    }
    if (!pointer_aligned(arena.data, alignof(max_align_t)) ||
        arena.capacity < required) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CAPACITY;
    }

    memset(&value, 0, sizeof(value));
    value.magic = MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MAGIC;
    value.local_role = config->local_role;
    value.phase = MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE;
    value.deadline_policy = config->deadline_policy;
    value.binding = config->binding;
    value.input_width = config->input_width;
    value.input_height = config->input_height;
    value.frame_bytes = frame_bytes;
    value.frame_format = config->frame_format;
    value.max_fragment_payload_bytes =
        config->max_fragment_payload_bytes;
    value.arena_begin = arena.data;
    value.arena_extent = arena.capacity;
    value.required_bytes = required;

    offset = MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES;
    value.primary.bytes = arena.data + offset;
    value.primary.capacity = frame_bytes;
    offset += frame_bytes;
    value.latest.bytes = arena.data + offset;
    value.latest.capacity = frame_bytes;
    offset += frame_bytes;
    value.packet_scratch = arena.data + offset;
    value.packet_scratch_capacity = packet_capacity;
    offset += packet_capacity;
    if (offset != required) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_CONFIG;
    }

    memset(arena.data, 0, required);
    transport = (MeisoDisplayIngressMediaTransport *)arena.data;
    *transport = value;
    *out_transport = transport;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

static void frame_from_slot(
    const MeisoDisplayIngressMediaFrameSlot *slot,
    MeisoDisplayIngressMediaFrame *out_frame
)
{
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->frame_sequence = slot->metadata.frame_sequence;
    out_frame->source_time = slot->metadata.source_time;
    out_frame->sender_deadline = slot->metadata.sender_deadline;
    out_frame->producer_dropped_frames =
        slot->metadata.producer_dropped_frames;
    out_frame->rgba_frame.data = slot->bytes;
    out_frame->rgba_frame.size = slot->metadata.frame_bytes;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_latest(
    const MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaLatestView *out_latest
)
{
    MeisoDisplayIngressMediaLatestView view;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        out_latest == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, out_latest, sizeof(*out_latest)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!transport->latest.occupied) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME;
    }

    memset(&view, 0, sizeof(view));
    view.ref.binding = transport->binding;
    view.ref.frame_sequence = transport->latest.metadata.frame_sequence;
    frame_from_slot(&transport->latest, &view.frame);
    *out_latest = view;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_release_latest(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameRef *expected_frame
)
{
    MeisoDisplayIngressMediaFrameRef current;
    MeisoDisplayIngressMediaTransportStatus status;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        expected_frame == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, expected_frame, sizeof(*expected_frame)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE;
    }
    status = meiso_display_ingress_media_private_binding_difference(
        &transport->binding, &expected_frame->binding
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return status;
    }
    if (!transport->latest.occupied) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME;
    }
    current.binding = transport->binding;
    current.frame_sequence = transport->latest.metadata.frame_sequence;
    if (!meiso_display_ingress_media_private_frame_ref_equal(
            expected_frame, &current
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT;
    }
    transport->latest.occupied = false;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_terminate(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaBinding *expected_binding,
    MeisoDisplayIngressMediaTerminalReason reason
)
{
    MeisoDisplayIngressMediaTransportStatus status;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        expected_binding == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, expected_binding, sizeof(*expected_binding)
        ) ||
        !meiso_display_ingress_media_private_terminal_reason_valid(reason)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL;
    }
    status = meiso_display_ingress_media_private_binding_difference(
        &transport->binding, expected_binding
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return status;
    }
    if (!meiso_display_ingress_media_private_binding_equal(
            &transport->binding, expected_binding
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    meiso_display_ingress_media_private_enter_terminal(transport, reason);
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_snapshot(
    const MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaTransportSnapshot *out_snapshot
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    if (!meiso_display_ingress_media_private_handle_valid(transport) ||
        out_snapshot == NULL ||
        meiso_display_ingress_media_private_overlaps(
            transport, out_snapshot, sizeof(*out_snapshot)
        )) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.local_role = transport->local_role;
    snapshot.binding = transport->binding;
    snapshot.phase = transport->phase;
    snapshot.terminal_reason = transport->terminal_reason;
    snapshot.deadline_policy = transport->deadline_policy;
    snapshot.input_width = transport->input_width;
    snapshot.input_height = transport->input_height;
    snapshot.frame_bytes = transport->frame_bytes;
    snapshot.max_fragment_payload_bytes =
        transport->max_fragment_payload_bytes;
    snapshot.arena_bytes = transport->required_bytes;
    snapshot.outbound_packet_pending = transport->outbound_packet_pending;
    snapshot.outbound_preemption_pending =
        transport->outbound_preempt_on_complete;
    snapshot.outbound_packet_serial_high_water =
        transport->outbound_packet_serial_high_water;
    snapshot.send_in_flight =
        transport->local_role == MEISO_SESSION_ROLE_HOST &&
        transport->primary.occupied;
    snapshot.send_latest =
        transport->local_role == MEISO_SESSION_ROLE_HOST &&
        transport->latest.occupied;
    snapshot.send_frame_sequence = snapshot.send_in_flight
        ? transport->primary.metadata.frame_sequence
        : 0U;
    snapshot.send_fragment_offset = snapshot.send_in_flight
        ? transport->primary_offset
        : 0U;
    snapshot.submitted_sequence_high_water =
        transport->submitted_sequence_high_water;
    snapshot.submitted_producer_drop_high_water =
        transport->submitted_producer_drop_high_water;
    snapshot.transport_whole_frames_dropped =
        transport->transport_whole_frames_dropped;
    snapshot.receive_reassembly =
        transport->local_role == MEISO_SESSION_ROLE_EDGE &&
        transport->primary.occupied;
    snapshot.receive_latest =
        transport->local_role == MEISO_SESSION_ROLE_EDGE &&
        transport->latest.occupied;
    snapshot.receive_frame_sequence = snapshot.receive_reassembly
        ? transport->primary.metadata.frame_sequence
        : 0U;
    snapshot.receive_next_offset = snapshot.receive_reassembly
        ? transport->primary_offset
        : 0U;
    snapshot.observed_sequence_high_water =
        transport->observed_sequence_high_water;
    snapshot.published_sequence_high_water =
        transport->published_sequence_high_water;
    snapshot.sequence_gap_frames = transport->sequence_gap_frames;
    snapshot.producer_drop_high_water = transport->producer_drop_high_water;
    snapshot.reassembly_frames_dropped =
        transport->reassembly_frames_dropped;
    snapshot.unread_latest_frames_dropped =
        transport->unread_latest_frames_dropped;
    snapshot.expired_frames_dropped = transport->expired_frames_dropped;
    *out_snapshot = snapshot;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}
