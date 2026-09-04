#include "display_ingress_media_transport_private.h"

/* Canonical carrier-neutral DIFR v1 packet codec. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    output[0] = (uint8_t)(value >> 56U);
    output[1] = (uint8_t)(value >> 48U);
    output[2] = (uint8_t)(value >> 40U);
    output[3] = (uint8_t)(value >> 32U);
    output[4] = (uint8_t)(value >> 24U);
    output[5] = (uint8_t)(value >> 16U);
    output[6] = (uint8_t)(value >> 8U);
    output[7] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t get_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
        ((uint32_t)input[1] << 16U) |
        ((uint32_t)input[2] << 8U) |
        (uint32_t)input[3];
}

static uint64_t get_u64(const uint8_t *input)
{
    return ((uint64_t)input[0] << 56U) |
        ((uint64_t)input[1] << 48U) |
        ((uint64_t)input[2] << 40U) |
        ((uint64_t)input[3] << 32U) |
        ((uint64_t)input[4] << 24U) |
        ((uint64_t)input[5] << 16U) |
        ((uint64_t)input[6] << 8U) |
        (uint64_t)input[7];
}

static bool bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool session_id_valid(MeisoSessionId value)
{
    return meiso_session_nonce_valid(value.host_nonce) &&
        meiso_session_nonce_valid(value.edge_nonce);
}

static bool packet_shape_valid(
    const MeisoDisplayIngressMediaDecodedPacket *packet
)
{
    const uint32_t fragment_bytes = (uint32_t)packet->fragment.size;
    const uint32_t frame_bytes = packet->metadata.frame_bytes;
    const uint32_t offset = packet->fragment_offset;
    const bool begin =
        (packet->flags & MEISO_DISPLAY_INGRESS_MEDIA_PACKET_BEGIN) != 0U;
    const bool end =
        (packet->flags & MEISO_DISPLAY_INGRESS_MEDIA_PACKET_END) != 0U;
    const bool recovery =
        (packet->flags & MEISO_DISPLAY_INGRESS_MEDIA_PACKET_RECOVERY) != 0U;

    if (packet->fragment.size == 0U || packet->fragment.size > UINT32_MAX ||
        frame_bytes == 0U || offset >= frame_bytes ||
        fragment_bytes > frame_bytes - offset ||
        (packet->flags & (MeisoDisplayIngressMediaPacketFlags)~(
             MEISO_DISPLAY_INGRESS_MEDIA_PACKET_BEGIN |
             MEISO_DISPLAY_INGRESS_MEDIA_PACKET_END |
             MEISO_DISPLAY_INGRESS_MEDIA_PACKET_RECOVERY
         )) != 0U) {
        return false;
    }
    return begin == (offset == 0U) &&
        end == (fragment_bytes == frame_bytes - offset) &&
        recovery == begin;
}

static bool packet_value_valid(
    const MeisoDisplayIngressMediaDecodedPacket *packet
)
{
    const MeisoDisplayIngressMediaBinding *binding;

    if (packet == NULL) {
        return false;
    }
    binding = &packet->binding;

    return session_id_valid(binding->runtime.session_id) &&
        binding->runtime.route.catalog_generation != 0U &&
        meiso_endpoint_id_valid(binding->runtime.route.endpoint_id) &&
        binding->runtime.route.endpoint_incarnation != 0U &&
        meiso_resource_id_valid(binding->runtime.route.resource_id) &&
        meiso_runtime_facet_valid(binding->runtime.route.facet) &&
        binding->runtime.production_epoch != 0U &&
        binding->media_stream_epoch != 0U &&
        meiso_session_nonce_valid(binding->channel_nonce) &&
        meiso_owner_scope_valid(binding->producer) &&
        packet->delivery == MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA &&
        packet->metadata.frame_sequence != 0U &&
        meiso_time_point_valid(packet->metadata.source_time) &&
        meiso_deadline_valid(packet->metadata.sender_deadline) &&
        (!packet->metadata.sender_deadline.present ||
         packet->metadata.sender_deadline.value.clock_epoch ==
             packet->metadata.source_time.clock_epoch) &&
        packet_shape_valid(packet);
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_packet_encode(
    const MeisoDisplayIngressMediaDecodedPacket *packet,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    const MeisoDisplayIngressMediaBinding *binding;
    size_t packet_bytes;
    uint8_t *bytes;

    if (packet == NULL || output.data == NULL || out_size == NULL ||
        !packet_value_valid(packet) ||
        !meiso_display_ingress_media_private_checked_add_size(
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE,
            packet->fragment.size, &packet_bytes
        ) ||
        packet_bytes > UINT32_MAX) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (output.capacity < packet_bytes) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CAPACITY;
    }

    binding = &packet->binding;
    bytes = output.data;
    memset(
        bytes, 0, MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE
    );
    put_u32(bytes, MEISO_DISPLAY_INGRESS_MEDIA_PACKET_MAGIC);
    put_u16(
        bytes + 4U, MEISO_DISPLAY_INGRESS_MEDIA_PACKET_SCHEMA_VERSION
    );
    put_u16(bytes + 6U, packet->flags);
    put_u32(
        bytes + 8U, MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE
    );
    put_u32(bytes + 12U, (uint32_t)packet_bytes);
    put_u64(bytes + 16U, binding->runtime.session_id.host_nonce.high);
    put_u64(bytes + 24U, binding->runtime.session_id.host_nonce.low);
    put_u64(bytes + 32U, binding->runtime.session_id.edge_nonce.high);
    put_u64(bytes + 40U, binding->runtime.session_id.edge_nonce.low);
    put_u64(bytes + 48U, binding->runtime.route.catalog_generation);
    put_u64(bytes + 56U, binding->runtime.route.endpoint_id.high);
    put_u64(bytes + 64U, binding->runtime.route.endpoint_id.low);
    put_u64(bytes + 72U, binding->runtime.route.endpoint_incarnation);
    put_u64(bytes + 80U, binding->runtime.route.resource_id.high);
    put_u64(bytes + 88U, binding->runtime.route.resource_id.low);
    put_u64(bytes + 96U, binding->runtime.production_epoch);
    put_u64(bytes + 104U, binding->media_stream_epoch);
    put_u64(bytes + 112U, binding->channel_nonce.high);
    put_u64(bytes + 120U, binding->channel_nonce.low);
    bytes[128U] = binding->producer.role;
    bytes[129U] = binding->runtime.route.facet;
    bytes[130U] = packet->delivery;
    bytes[131U] = packet->metadata.sender_deadline.present ? 1U : 0U;
    put_u64(bytes + 136U, binding->producer.incarnation);
    put_u64(bytes + 144U, packet->metadata.frame_sequence);
    put_u64(bytes + 152U, packet->metadata.source_time.clock_epoch);
    put_u64(bytes + 160U, packet->metadata.source_time.monotonic_ns);
    if (packet->metadata.sender_deadline.present) {
        put_u64(
            bytes + 168U,
            packet->metadata.sender_deadline.value.clock_epoch
        );
        put_u64(
            bytes + 176U,
            packet->metadata.sender_deadline.value.monotonic_ns
        );
    }
    put_u64(bytes + 184U, packet->metadata.producer_dropped_frames);
    put_u32(bytes + 192U, packet->metadata.frame_bytes);
    put_u32(bytes + 196U, packet->fragment_offset);
    put_u32(bytes + 200U, (uint32_t)packet->fragment.size);
    memmove(
        bytes + MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE,
        packet->fragment.data, packet->fragment.size
    );
    *out_size = packet_bytes;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_private_packet_decode(
    MeisoByteView input,
    MeisoDisplayIngressMediaDecodedPacket *out_packet
)
{
    MeisoDisplayIngressMediaDecodedPacket packet;
    const uint8_t *bytes;
    uint32_t packet_bytes;
    uint32_t fragment_bytes;

    if (input.data == NULL || out_packet == NULL) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (input.size > UINT32_MAX) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }

    bytes = input.data;
    packet_bytes = get_u32(bytes + 12U);
    if (packet_bytes > input.size) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (get_u32(bytes) != MEISO_DISPLAY_INGRESS_MEDIA_PACKET_MAGIC ||
        get_u16(bytes + 4U) !=
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_SCHEMA_VERSION ||
        get_u32(bytes + 8U) !=
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE ||
        packet_bytes != input.size ||
        !bytes_zero(bytes + 132U, 4U) ||
        !bytes_zero(bytes + 204U, 20U) ||
        bytes[131U] > 1U) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }
    fragment_bytes = get_u32(bytes + 200U);
    if (fragment_bytes != packet_bytes -
            MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }

    memset(&packet, 0, sizeof(packet));
    packet.flags = get_u16(bytes + 6U);
    packet.binding.runtime.session_id.host_nonce.high = get_u64(bytes + 16U);
    packet.binding.runtime.session_id.host_nonce.low = get_u64(bytes + 24U);
    packet.binding.runtime.session_id.edge_nonce.high = get_u64(bytes + 32U);
    packet.binding.runtime.session_id.edge_nonce.low = get_u64(bytes + 40U);
    packet.binding.runtime.route.catalog_generation = get_u64(bytes + 48U);
    packet.binding.runtime.route.endpoint_id.high = get_u64(bytes + 56U);
    packet.binding.runtime.route.endpoint_id.low = get_u64(bytes + 64U);
    packet.binding.runtime.route.endpoint_incarnation = get_u64(bytes + 72U);
    packet.binding.runtime.route.resource_id.high = get_u64(bytes + 80U);
    packet.binding.runtime.route.resource_id.low = get_u64(bytes + 88U);
    packet.binding.runtime.production_epoch = get_u64(bytes + 96U);
    packet.binding.media_stream_epoch = get_u64(bytes + 104U);
    packet.binding.channel_nonce.high = get_u64(bytes + 112U);
    packet.binding.channel_nonce.low = get_u64(bytes + 120U);
    packet.binding.producer.role = bytes[128U];
    packet.binding.runtime.route.facet = bytes[129U];
    packet.delivery = bytes[130U];
    packet.binding.producer.incarnation = get_u64(bytes + 136U);
    packet.metadata.frame_sequence = get_u64(bytes + 144U);
    packet.metadata.source_time.clock_epoch = get_u64(bytes + 152U);
    packet.metadata.source_time.monotonic_ns = get_u64(bytes + 160U);
    packet.metadata.sender_deadline.present = bytes[131U] != 0U;
    packet.metadata.sender_deadline.value.clock_epoch = get_u64(bytes + 168U);
    packet.metadata.sender_deadline.value.monotonic_ns = get_u64(bytes + 176U);
    packet.metadata.producer_dropped_frames = get_u64(bytes + 184U);
    packet.metadata.frame_bytes = get_u32(bytes + 192U);
    packet.fragment_offset = get_u32(bytes + 196U);
    packet.fragment.data = bytes +
        MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE;
    packet.fragment.size = fragment_bytes;
    packet.binding.runtime.resource.id =
        packet.binding.runtime.route.resource_id;
    packet.binding.runtime.resource.kind = MEISO_RESOURCE_KIND_STREAM;
    packet.binding.runtime.resource.owner = packet.binding.producer.role;
    packet.binding.runtime.resource.max_payload_size =
        packet.metadata.frame_bytes;

    if ((!packet.metadata.sender_deadline.present &&
         (packet.metadata.sender_deadline.value.clock_epoch != 0U ||
          packet.metadata.sender_deadline.value.monotonic_ns != 0U)) ||
        !packet_value_valid(&packet)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED;
    }
    *out_packet = packet;
    return MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}
