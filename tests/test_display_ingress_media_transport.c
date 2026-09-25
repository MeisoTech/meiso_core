#include <meiso/profiles/display_ingress/media_transport.h>

#include <limits.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_ARENA_CAPACITY 4096U
#define TEST_PACKET_CAPACITY 512U

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_ARENA_CAPACITY];
} TestArena;

typedef struct {
    MeisoDisplayIngressMediaTransportConfig host_config;
    MeisoDisplayIngressMediaTransportConfig edge_config;
    TestArena host_arena;
    TestArena edge_arena;
    size_t host_bytes;
    size_t edge_bytes;
    MeisoDisplayIngressMediaTransport *host;
    MeisoDisplayIngressMediaTransport *edge;
} MediaPair;

typedef struct {
    MeisoDisplayIngressMediaPacketRef ref;
    uint8_t bytes[TEST_PACKET_CAPACITY];
    size_t size;
} OwnedPacket;

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    bytes[0] = (uint8_t)(value >> 56U);
    bytes[1] = (uint8_t)(value >> 48U);
    bytes[2] = (uint8_t)(value >> 40U);
    bytes[3] = (uint8_t)(value >> 32U);
    bytes[4] = (uint8_t)(value >> 24U);
    bytes[5] = (uint8_t)(value >> 16U);
    bytes[6] = (uint8_t)(value >> 8U);
    bytes[7] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint64_t get_u64(const uint8_t *bytes)
{
    return ((uint64_t)bytes[0] << 56U) |
        ((uint64_t)bytes[1] << 48U) |
        ((uint64_t)bytes[2] << 40U) |
        ((uint64_t)bytes[3] << 32U) |
        ((uint64_t)bytes[4] << 24U) |
        ((uint64_t)bytes[5] << 16U) |
        ((uint64_t)bytes[6] << 8U) |
        (uint64_t)bytes[7];
}

static bool binding_equal(
    const MeisoDisplayIngressMediaBinding *lhs,
    const MeisoDisplayIngressMediaBinding *rhs
)
{
    return meiso_session_id_equal(
               lhs->runtime.session_id, rhs->runtime.session_id
           ) &&
        lhs->runtime.route.catalog_generation ==
            rhs->runtime.route.catalog_generation &&
        lhs->runtime.route.endpoint_id.high ==
            rhs->runtime.route.endpoint_id.high &&
        lhs->runtime.route.endpoint_id.low ==
            rhs->runtime.route.endpoint_id.low &&
        lhs->runtime.route.endpoint_incarnation ==
            rhs->runtime.route.endpoint_incarnation &&
        lhs->runtime.route.resource_id.high ==
            rhs->runtime.route.resource_id.high &&
        lhs->runtime.route.resource_id.low ==
            rhs->runtime.route.resource_id.low &&
        lhs->runtime.route.facet == rhs->runtime.route.facet &&
        meiso_resource_descriptor_equal(
            &lhs->runtime.resource, &rhs->runtime.resource
        ) &&
        lhs->runtime.production_epoch == rhs->runtime.production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low &&
        lhs->producer.role == rhs->producer.role &&
        lhs->producer.incarnation == rhs->producer.incarnation;
}

static bool packet_ref_equal(
    const MeisoDisplayIngressMediaPacketRef *lhs,
    const MeisoDisplayIngressMediaPacketRef *rhs
)
{
    return binding_equal(&lhs->binding, &rhs->binding) &&
        lhs->serial == rhs->serial &&
        lhs->frame_sequence == rhs->frame_sequence &&
        lhs->fragment_offset == rhs->fragment_offset &&
        lhs->packet_bytes == rhs->packet_bytes;
}

static MeisoDisplayIngressMediaTransportConfig transport_config(
    MeisoSessionRole role,
    uint32_t width,
    uint32_t height,
    uint32_t fragment_bytes,
    MeisoStreamDeadlinePolicy deadline_policy
)
{
    MeisoDisplayIngressMediaTransportConfig value;
    const uint32_t frame_bytes = width * height * 4U;

    memset(&value, 0, sizeof(value));
    value.local_role = role;
    value.input_width = width;
    value.input_height = height;
    value.max_fragment_payload_bytes = fragment_bytes;
    value.deadline_policy = deadline_policy;
    value.binding.runtime.session_id.host_nonce =
        (MeisoSessionNonce){11U, 12U};
    value.binding.runtime.session_id.edge_nonce =
        (MeisoSessionNonce){13U, 14U};
    value.binding.runtime.route.catalog_generation = 17U;
    value.binding.runtime.route.endpoint_id = (MeisoEndpointId){
        UINT64_C(0x444953504c41595f),
        UINT64_C(0x494e47525f454e44)
    };
    value.binding.runtime.route.endpoint_incarnation = 19U;
    value.binding.runtime.route.resource_id = (MeisoResourceId){
        UINT64_C(0x444953504c41595f),
        UINT64_C(0x444154415f4d4544)
    };
    value.binding.runtime.route.facet = MEISO_RUNTIME_FACET_DATA;
    value.binding.runtime.resource.id =
        value.binding.runtime.route.resource_id;
    value.binding.runtime.resource.kind = MEISO_RESOURCE_KIND_STREAM;
    value.binding.runtime.resource.schema = (MeisoSchemaRef){
        {
            UINT64_C(0x444953505f534348),
            UINT64_C(0x4652414d455f5f5f)
        },
        1U
    };
    value.binding.runtime.resource.owner = MEISO_OWNER_ROLE_HOST;
    value.binding.runtime.resource.max_payload_size = frame_bytes;
    value.binding.runtime.production_epoch = 23U;
    value.binding.media_stream_epoch = 23U;
    value.binding.channel_nonce = (MeisoSessionNonce){31U, 37U};
    value.binding.producer = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 41U};
    return value;
}

static bool initialize_pair_policies(
    MediaPair *pair,
    uint32_t width,
    uint32_t height,
    uint32_t fragment_bytes,
    MeisoStreamDeadlinePolicy host_policy,
    MeisoStreamDeadlinePolicy edge_policy
)
{
    size_t alignment = 0U;

    memset(pair, 0, sizeof(*pair));
    pair->host_config = transport_config(
        MEISO_SESSION_ROLE_HOST, width, height, fragment_bytes, host_policy
    );
    pair->edge_config = transport_config(
        MEISO_SESSION_ROLE_EDGE, width, height, fragment_bytes, edge_policy
    );
    CHECK(meiso_display_ingress_media_transport_memory_required(
              &pair->host_config, &pair->host_bytes, &alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(alignment == alignof(max_align_t));
    CHECK(pair->host_bytes <= sizeof(pair->host_arena.bytes));
    CHECK(meiso_display_ingress_media_transport_memory_required(
              &pair->edge_config, &pair->edge_bytes, &alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(pair->edge_bytes == pair->host_bytes);
    CHECK(meiso_display_ingress_media_transport_init(
              &pair->host_config,
              (MeisoMutableBuffer){pair->host_arena.bytes, pair->host_bytes},
              &pair->host
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_init(
              &pair->edge_config,
              (MeisoMutableBuffer){pair->edge_arena.bytes, pair->edge_bytes},
              &pair->edge
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    return true;
}

static bool initialize_pair(
    MediaPair *pair,
    uint32_t width,
    uint32_t height,
    uint32_t fragment_bytes
)
{
    return initialize_pair_policies(
        pair, width, height, fragment_bytes,
        MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE,
        MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE
    );
}

static void fill_frame(uint8_t *bytes, size_t size, uint8_t seed)
{
    size_t offset;

    for (offset = 0U; offset < size; offset += 4U) {
        bytes[offset] = seed++;
        bytes[offset + 1U] = seed++;
        bytes[offset + 2U] = seed++;
        bytes[offset + 3U] = UINT8_MAX;
    }
}

static MeisoDisplayIngressMediaFrame frame_value(
    const uint8_t *bytes,
    size_t size,
    uint64_t sequence,
    uint64_t drops
)
{
    MeisoDisplayIngressMediaFrame frame;

    memset(&frame, 0, sizeof(frame));
    frame.frame_sequence = sequence;
    frame.source_time = (MeisoTimePoint){43U, 47U + sequence};
    frame.producer_dropped_frames = drops;
    frame.rgba_frame = (MeisoByteView){bytes, size};
    return frame;
}

static bool snapshot(
    const MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaTransportSnapshot *out_snapshot
)
{
    return meiso_display_ingress_media_transport_snapshot(
               transport, out_snapshot
           ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK;
}

static bool capture_packet(
    MeisoDisplayIngressMediaTransport *transport,
    OwnedPacket *out_packet
)
{
    MeisoDisplayIngressMediaPacketView view;

    memset(&view, 0, sizeof(view));
    if (meiso_display_ingress_media_transport_next_outbound(
            transport, &view
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        view.bytes.data == NULL || view.bytes.size > sizeof(out_packet->bytes)) {
        return false;
    }
    out_packet->ref = view.ref;
    out_packet->size = view.bytes.size;
    memcpy(out_packet->bytes, view.bytes.data, view.bytes.size);
    return true;
}

static bool complete_and_receive(
    MeisoDisplayIngressMediaTransport *sender,
    MeisoDisplayIngressMediaTransport *receiver,
    const OwnedPacket *packet,
    const MeisoStreamAdmission *admission,
    MeisoDisplayIngressMediaTransportStatus expected_receive
)
{
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              sender, &packet->ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              receiver, (MeisoByteView){packet->bytes, packet->size},
              admission
          ) == expected_receive);
    return true;
}

static bool pump_current_frame(
    MeisoDisplayIngressMediaTransport *sender,
    MeisoDisplayIngressMediaTransport *receiver
)
{
    MeisoDisplayIngressMediaTransportSnapshot state;

    for (;;) {
        OwnedPacket packet;
        const bool end = snapshot(sender, &state) &&
            state.send_in_flight &&
            state.send_fragment_offset +
                    state.max_fragment_payload_bytes >=
                state.frame_bytes;
        CHECK(state.send_in_flight);
        CHECK(capture_packet(sender, &packet));
        CHECK(complete_and_receive(
            sender, receiver, &packet, NULL,
            end ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED
                : MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
        ));
        CHECK(snapshot(sender, &state));
        if (!state.send_in_flight) {
            return true;
        }
    }
}

static bool memory_rejects(
    const MeisoDisplayIngressMediaTransportConfig *config
)
{
    size_t size = 123U;
    size_t alignment = 456U;

    CHECK(meiso_display_ingress_media_transport_memory_required(
              config, &size, &alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_CONFIG);
    CHECK(size == 123U && alignment == 456U);
    return true;
}

static bool test_memory_init_exact_profile_and_alias_contract(void)
{
    MeisoDisplayIngressMediaTransportConfig config = transport_config(
        MEISO_SESSION_ROLE_HOST, 4U, 2U, 8U,
        MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE
    );
    MeisoDisplayIngressMediaTransportConfig invalid;
    MeisoDisplayIngressMediaTransport *transport =
        (MeisoDisplayIngressMediaTransport *)(uintptr_t)1U;
    MeisoDisplayIngressMediaPacketView output;
    MeisoDisplayIngressMediaTransportSnapshot state;
    TestArena arena;
    uint8_t before[TEST_ARENA_CAPACITY];
    size_t required = 0U;
    size_t alignment = 0U;
    size_t aliased = 999U;

    CHECK(meiso_display_ingress_media_transport_memory_required(
              &config, &required, &alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(required == 2048U + 2U * 32U + 224U + 8U);
    CHECK(alignment == alignof(max_align_t));
    CHECK(meiso_display_ingress_media_transport_memory_required(
              &config, &aliased, &aliased
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(aliased == 999U);

    invalid = config;
    invalid.binding.runtime.route.endpoint_id.low ^= 1U;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.runtime.route.resource_id.low ^= 1U;
    invalid.binding.runtime.resource.id = invalid.binding.runtime.route.resource_id;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.runtime.route.facet = MEISO_RUNTIME_FACET_ACTUAL;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.runtime.resource.kind = MEISO_RESOURCE_KIND_BLOB;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.runtime.resource.owner = MEISO_OWNER_ROLE_EDGE;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    ++invalid.binding.runtime.resource.schema.version;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.runtime.resource.max_payload_size = 31U;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.runtime.production_epoch = 0U;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.media_stream_epoch = 0U;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    ++invalid.binding.media_stream_epoch;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.channel_nonce = (MeisoSessionNonce){0U, 0U};
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.binding.producer.role = MEISO_OWNER_ROLE_EDGE;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.max_fragment_payload_bytes = UINT32_MAX;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.input_width = UINT32_MAX;
    invalid.input_height = UINT32_MAX;
    CHECK(memory_rejects(&invalid));
    invalid = config;
    invalid.deadline_policy = 0U;
    CHECK(memory_rejects(&invalid));

    memset(arena.bytes, 0xa5, sizeof(arena.bytes));
    memcpy(before, arena.bytes, sizeof(before));
    CHECK(meiso_display_ingress_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes, required - 1U},
              &transport
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CAPACITY);
    CHECK(transport == (MeisoDisplayIngressMediaTransport *)(uintptr_t)1U);
    CHECK(memcmp(before, arena.bytes, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes + 1U, required},
              &transport
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CAPACITY);
    CHECK(memcmp(before, arena.bytes, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes, required},
              (MeisoDisplayIngressMediaTransport **)(void *)arena.bytes
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, arena.bytes, sizeof(before)) == 0);

    CHECK(meiso_display_ingress_media_transport_init(
              &config, (MeisoMutableBuffer){arena.bytes, required},
              &transport
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(transport, &state));
    CHECK(state.phase == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE &&
          state.frame_bytes == 32U && state.arena_bytes == required);
    memcpy(before, arena.bytes, required);
    CHECK(meiso_display_ingress_media_transport_snapshot(
              transport,
              (MeisoDisplayIngressMediaTransportSnapshot *)(void *)(
                  arena.bytes + 512U
              )
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, arena.bytes, required) == 0);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_display_ingress_media_transport_next_outbound(
              transport, &output
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_PACKET);
    CHECK(bytes_are((const uint8_t *)&output, 0xa5, sizeof(output)));
    return true;
}

static bool test_golden_packet_idempotence_and_exact_refs(void)
{
    const uint8_t rgba[8] = {1U, 2U, 3U, 255U, 4U, 5U, 6U, 255U};
    MediaPair pair;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaPacketRef wrong_ref;
    MeisoDisplayIngressMediaPacketView repeated;
    MeisoDisplayIngressMediaTransportSnapshot before;
    MeisoDisplayIngressMediaTransportSnapshot after;
    MeisoDisplayIngressMediaTransportConfig new_config;
    MeisoDisplayIngressMediaTransport *new_transport = NULL;
    TestArena new_arena;
    OwnedPacket packet;
    OwnedPacket new_packet;
    uint8_t expected[232];
    size_t new_required = 0U;
    size_t new_alignment = 0U;

    CHECK(initialize_pair(&pair, 2U, 1U, 8U));
    frame = frame_value(rgba, sizeof(rgba), 5U, 7U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &packet));
    CHECK(packet.size == sizeof(expected));
    CHECK(packet.ref.serial == 1U && packet.ref.frame_sequence == 5U &&
          packet.ref.fragment_offset == 0U &&
          packet.ref.packet_bytes == sizeof(expected));
    CHECK(binding_equal(&packet.ref.binding, &pair.host_config.binding));

    memset(expected, 0, sizeof(expected));
    put_u32(expected, UINT32_C(0x44494652));
    put_u16(expected + 4U, 1U);
    put_u16(expected + 6U, 7U);
    put_u32(expected + 8U, 224U);
    put_u32(expected + 12U, 232U);
    put_u64(expected + 16U, 11U);
    put_u64(expected + 24U, 12U);
    put_u64(expected + 32U, 13U);
    put_u64(expected + 40U, 14U);
    put_u64(expected + 48U, 17U);
    put_u64(expected + 56U, UINT64_C(0x444953504c41595f));
    put_u64(expected + 64U, UINT64_C(0x494e47525f454e44));
    put_u64(expected + 72U, 19U);
    put_u64(expected + 80U, UINT64_C(0x444953504c41595f));
    put_u64(expected + 88U, UINT64_C(0x444154415f4d4544));
    put_u64(expected + 96U, 23U);
    put_u64(expected + 104U, 23U);
    put_u64(expected + 112U, 31U);
    put_u64(expected + 120U, 37U);
    expected[128U] = MEISO_OWNER_ROLE_HOST;
    expected[129U] = MEISO_RUNTIME_FACET_DATA;
    expected[130U] = MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    put_u64(expected + 136U, 41U);
    put_u64(expected + 144U, 5U);
    put_u64(expected + 152U, 43U);
    put_u64(expected + 160U, 52U);
    put_u64(expected + 184U, 7U);
    put_u32(expected + 192U, 8U);
    put_u32(expected + 196U, 0U);
    put_u32(expected + 200U, 8U);
    memcpy(expected + 224U, rgba, sizeof(rgba));
    CHECK(memcmp(expected, packet.bytes, sizeof(expected)) == 0);

    memset(&repeated, 0, sizeof(repeated));
    CHECK(meiso_display_ingress_media_transport_next_outbound(
              pair.host, &repeated
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(packet_ref_equal(&repeated.ref, &packet.ref));
    CHECK(repeated.bytes.size == packet.size &&
          memcmp(repeated.bytes.data, packet.bytes, packet.size) == 0);

    CHECK(snapshot(pair.host, &before));
    wrong_ref = packet.ref;
    ++wrong_ref.binding.runtime.route.endpoint_incarnation;
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &wrong_ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    wrong_ref = packet.ref;
    ++wrong_ref.binding.runtime.production_epoch;
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &wrong_ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    wrong_ref = packet.ref;
    ++wrong_ref.binding.media_stream_epoch;
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &wrong_ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    wrong_ref = packet.ref;
    ++wrong_ref.binding.channel_nonce.low;
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &wrong_ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    wrong_ref = packet.ref;
    ++wrong_ref.binding.runtime.resource.schema.version;
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &wrong_ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    CHECK(snapshot(pair.host, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);

    new_config = transport_config(
        MEISO_SESSION_ROLE_HOST, 2U, 1U, 8U,
        MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE
    );
    ++new_config.binding.runtime.production_epoch;
    ++new_config.binding.media_stream_epoch;
    CHECK(meiso_display_ingress_media_transport_memory_required(
              &new_config, &new_required, &new_alignment
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_init(
              &new_config,
              (MeisoMutableBuffer){new_arena.bytes, new_required},
              &new_transport
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_submit(
              new_transport, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(new_transport, &new_packet));
    CHECK(new_packet.ref.serial == packet.ref.serial &&
          new_packet.ref.frame_sequence == packet.ref.frame_sequence &&
          new_packet.ref.fragment_offset == packet.ref.fragment_offset &&
          new_packet.ref.packet_bytes == packet.ref.packet_bytes);
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              new_transport, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    CHECK(snapshot(new_transport, &after));
    CHECK(after.outbound_packet_pending && after.send_fragment_offset == 0U);
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              new_transport, &new_packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    return true;
}

static bool test_fragment_boundary_preemption_latest_and_release(void)
{
    uint8_t rgba1[32];
    uint8_t rgba2[32];
    uint8_t rgba3[32];
    MediaPair pair;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaFrameRef wrong;
    MeisoDisplayIngressMediaTransportSnapshot host_state;
    MeisoDisplayIngressMediaTransportSnapshot edge_state;
    OwnedPacket old_packet;
    OwnedPacket new_packet;

    fill_frame(rgba1, sizeof(rgba1), 1U);
    fill_frame(rgba2, sizeof(rgba2), 31U);
    fill_frame(rgba3, sizeof(rgba3), 61U);
    CHECK(initialize_pair(&pair, 4U, 2U, 8U));
    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &old_packet));
    frame = frame_value(rgba2, sizeof(rgba2), 2U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(pair.host, &host_state));
    CHECK(host_state.outbound_packet_pending &&
          host_state.outbound_preemption_pending);
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &old_packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(pair.host, &host_state));
    CHECK(host_state.send_in_flight && !host_state.send_latest &&
          host_state.send_frame_sequence == 2U &&
          host_state.send_fragment_offset == 0U &&
          host_state.transport_whole_frames_dropped == 1U);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge,
              (MeisoByteView){old_packet.bytes, old_packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED);

    CHECK(capture_packet(pair.host, &new_packet));
    CHECK(new_packet.ref.frame_sequence == 2U &&
          get_u16(new_packet.bytes + 6U) ==
              (MEISO_DISPLAY_INGRESS_MEDIA_PACKET_BEGIN |
               MEISO_DISPLAY_INGRESS_MEDIA_PACKET_RECOVERY) &&
          get_u64(new_packet.bytes + 184U) == 1U);
    CHECK(complete_and_receive(
        pair.host, pair.edge, &new_packet, NULL,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_REASSEMBLY_REPLACED
    ));
    CHECK(pump_current_frame(pair.host, pair.edge));
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.frame_sequence == 2U &&
          latest.frame.producer_dropped_frames == 1U &&
          latest.frame.rgba_frame.size == sizeof(rgba2) &&
          memcmp(latest.frame.rgba_frame.data, rgba2, sizeof(rgba2)) == 0);

    frame = frame_value(rgba3, sizeof(rgba3), 3U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(pump_current_frame(pair.host, pair.edge));
    CHECK(snapshot(pair.edge, &edge_state));
    CHECK(edge_state.published_sequence_high_water == 3U &&
          edge_state.reassembly_frames_dropped == 1U &&
          edge_state.unread_latest_frames_dropped == 1U &&
          edge_state.producer_drop_high_water == 1U);
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    wrong = latest.ref;
    --wrong.frame_sequence;
    CHECK(meiso_display_ingress_media_transport_release_latest(
              pair.edge, &wrong
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT);
    CHECK(meiso_display_ingress_media_transport_release_latest(
              pair.edge, &latest.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME);
    return true;
}

/* planes-v12.md §6 (meiso_core #36): a frame arriving after a newer one was published is never
 * published, so the Edge only ever moves its video forward. */
static bool test_late_older_frame_is_not_published(void)
{
    uint8_t rgba1[32];
    uint8_t rgba2[32];
    MediaPair pair;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaLatestView latest;
    MeisoDisplayIngressMediaTransportSnapshot edge_state;
    OwnedPacket old_packet;
    OwnedPacket new_packet;

    fill_frame(rgba1, sizeof(rgba1), 3U);
    fill_frame(rgba2, sizeof(rgba2), 33U);
    CHECK(initialize_pair(&pair, 4U, 2U, 64U));
    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &old_packet));
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &old_packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    frame = frame_value(rgba2, sizeof(rgba2), 2U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &new_packet));
    CHECK(complete_and_receive(
        pair.host, pair.edge, &new_packet, NULL,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED
    ));
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(latest.frame.frame_sequence == 2U);
    CHECK(meiso_display_ingress_media_transport_release_latest(
              pair.edge, &latest.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);

    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){old_packet.bytes, old_packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_SEQUENCE_REPLAY);
    CHECK(meiso_display_ingress_media_transport_latest(
              pair.edge, &latest
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME);
    CHECK(snapshot(pair.edge, &edge_state));
    CHECK(edge_state.published_sequence_high_water == 2U);
    return true;
}

static bool test_discontinuity_recovery_and_separate_counters(void)
{
    uint8_t rgba[32];
    MediaPair pair;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaTransportSnapshot state;
    OwnedPacket first;
    OwnedPacket packet;

    memset(rgba, 0xff, sizeof(rgba));
    CHECK(initialize_pair(&pair, 4U, 2U, 8U));
    frame = frame_value(rgba, sizeof(rgba), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &first));
    CHECK(complete_and_receive(
        pair.host, pair.edge, &first, NULL,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){first.bytes, first.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY);
    CHECK(capture_packet(pair.host, &packet));
    CHECK(complete_and_receive(
        pair.host, pair.edge, &packet, NULL,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY
    ));
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY);
    do {
        CHECK(capture_packet(pair.host, &packet));
        CHECK(meiso_display_ingress_media_transport_complete_outbound(
                  pair.host, &packet.ref
              ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        CHECK(snapshot(pair.host, &state));
    } while (state.send_in_flight);

    frame = frame_value(rgba, sizeof(rgba), 3U, 2U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &packet));
    CHECK(complete_and_receive(
        pair.host, pair.edge, &packet, NULL,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){first.bytes, first.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_SEQUENCE_REPLAY);
    CHECK(snapshot(pair.edge, &state));
    CHECK(state.receive_reassembly && state.receive_frame_sequence == 3U &&
          state.receive_next_offset == 8U &&
          state.reassembly_frames_dropped == 1U);
    CHECK(pump_current_frame(pair.host, pair.edge));
    CHECK(snapshot(pair.edge, &state));
    CHECK(state.observed_sequence_high_water == 3U &&
          state.published_sequence_high_water == 3U &&
          state.sequence_gap_frames == 2U &&
          state.producer_drop_high_water == 2U &&
          state.reassembly_frames_dropped == 1U &&
          state.expired_frames_dropped == 0U);
    return true;
}

static bool test_idle_fragment_boundary_preemption(void)
{
    const size_t completed_fragments[] = {0U, 1U, 3U};
    uint8_t old_rgba[32];
    uint8_t new_rgba[32];
    uint8_t newest_rgba[32];
    size_t case_index;

    fill_frame(old_rgba, sizeof(old_rgba), 1U);
    fill_frame(new_rgba, sizeof(new_rgba), 31U);
    fill_frame(newest_rgba, sizeof(newest_rgba), 61U);
    for (case_index = 0U;
         case_index < sizeof(completed_fragments) /
                          sizeof(completed_fragments[0]);
         ++case_index) {
        MediaPair pair;
        MeisoDisplayIngressMediaFrame frame;
        MeisoDisplayIngressMediaTransportSnapshot state;
        OwnedPacket packet;
        size_t index;
        uint64_t expected_sequence = 2U;
        uint64_t expected_drops = 1U;

        CHECK(initialize_pair(&pair, 4U, 2U, 8U));
        frame = frame_value(old_rgba, sizeof(old_rgba), 1U, 0U);
        CHECK(meiso_display_ingress_media_transport_submit(
                  pair.host, &frame, NULL
              ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        for (index = 0U; index < completed_fragments[case_index]; ++index) {
            CHECK(capture_packet(pair.host, &packet));
            CHECK(packet.ref.frame_sequence == 1U);
            CHECK(meiso_display_ingress_media_transport_complete_outbound(
                      pair.host, &packet.ref
                  ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        }
        frame = frame_value(new_rgba, sizeof(new_rgba), 2U, 0U);
        CHECK(meiso_display_ingress_media_transport_submit(
                  pair.host, &frame, NULL
              ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
        if (completed_fragments[case_index] == 1U) {
            frame = frame_value(newest_rgba, sizeof(newest_rgba), 3U, 0U);
            CHECK(meiso_display_ingress_media_transport_submit(
                      pair.host, &frame, NULL
                  ) ==
                  MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_LATEST_REPLACED);
            expected_sequence = 3U;
            expected_drops = 2U;
        }
        CHECK(capture_packet(pair.host, &packet));
        CHECK(packet.ref.frame_sequence == expected_sequence &&
              packet.ref.fragment_offset == 0U &&
              get_u16(packet.bytes + 6U) ==
                  (MEISO_DISPLAY_INGRESS_MEDIA_PACKET_BEGIN |
                   MEISO_DISPLAY_INGRESS_MEDIA_PACKET_RECOVERY) &&
              get_u64(packet.bytes + 184U) == expected_drops);
        CHECK(snapshot(pair.host, &state));
        CHECK(state.send_frame_sequence == expected_sequence &&
              state.send_fragment_offset == 0U &&
              state.transport_whole_frames_dropped == expected_drops);
    }
    return true;
}

static bool test_borrowed_packet_stable_across_latest_replacement(void)
{
    uint8_t rgba1[32];
    uint8_t rgba2[32];
    uint8_t rgba3[32];
    MediaPair pair;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaPacketView repeated;
    MeisoDisplayIngressMediaTransportSnapshot state;
    OwnedPacket borrowed;

    fill_frame(rgba1, sizeof(rgba1), 1U);
    fill_frame(rgba2, sizeof(rgba2), 31U);
    fill_frame(rgba3, sizeof(rgba3), 61U);
    CHECK(initialize_pair(&pair, 4U, 2U, 8U));
    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &borrowed));

    frame = frame_value(rgba2, sizeof(rgba2), 2U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    memset(&repeated, 0, sizeof(repeated));
    CHECK(meiso_display_ingress_media_transport_next_outbound(
              pair.host, &repeated
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(packet_ref_equal(&repeated.ref, &borrowed.ref) &&
          repeated.bytes.size == borrowed.size &&
          memcmp(repeated.bytes.data, borrowed.bytes, borrowed.size) == 0);

    frame = frame_value(rgba3, sizeof(rgba3), 3U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_LATEST_REPLACED);
    memset(&repeated, 0, sizeof(repeated));
    CHECK(meiso_display_ingress_media_transport_next_outbound(
              pair.host, &repeated
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(packet_ref_equal(&repeated.ref, &borrowed.ref) &&
          repeated.bytes.size == borrowed.size &&
          memcmp(repeated.bytes.data, borrowed.bytes, borrowed.size) == 0);
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &borrowed.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(pair.host, &state));
    CHECK(state.send_in_flight && !state.send_latest &&
          state.send_frame_sequence == 3U &&
          state.send_fragment_offset == 0U &&
          state.transport_whole_frames_dropped == 2U);
    return true;
}

static MeisoStreamAdmission admission(
    uint8_t comparable,
    uint8_t uncertainty,
    uint64_t local_deadline,
    uint64_t local_now
)
{
    MeisoStreamAdmission value;

    memset(&value, 0, sizeof(value));
    value.comparable = comparable;
    value.uncertainty_acceptable = uncertainty;
    value.local_deadline = (MeisoTimePoint){77U, local_deadline};
    value.local_now = (MeisoTimePoint){77U, local_now};
    return value;
}

static bool test_deadline_inheritance(void)
{
    uint8_t rgba[8];
    MediaPair pair;
    MediaPair unsupported;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaTransportSnapshot before;
    MeisoDisplayIngressMediaTransportSnapshot after;
    MeisoStreamAdmission equal = admission(2U, 7U, 100U, 100U);
    MeisoStreamAdmission live = admission(1U, 1U, 200U, 199U);
    MeisoStreamAdmission expired = admission(1U, 1U, 200U, 201U);
    OwnedPacket packet;

    fill_frame(rgba, sizeof(rgba), 1U);
    CHECK(initialize_pair(&pair, 2U, 1U, 8U));
    frame = frame_value(rgba, sizeof(rgba), 1U, 0U);
    frame.sender_deadline = (MeisoDeadline){true, {43U, 1000U}};
    CHECK(snapshot(pair.host, &before));
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNCOMPARABLE);
    CHECK(snapshot(pair.host, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    frame.sender_deadline.value.clock_epoch = 44U;
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, &equal
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    frame.sender_deadline.value.clock_epoch = 43U;
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, &equal
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &packet));
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNCOMPARABLE);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, &equal
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED);

    frame = frame_value(rgba, sizeof(rgba), 2U, 0U);
    frame.sender_deadline = (MeisoDeadline){true, {43U, 2000U}};
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, &expired
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DROPPED_EXPIRED);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, &live
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &packet));
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, &expired
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DROPPED_EXPIRED);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, &live
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_SEQUENCE_REPLAY);
    CHECK(snapshot(pair.edge, &after));
    CHECK(after.observed_sequence_high_water == 2U &&
          after.published_sequence_high_water == 1U &&
          after.expired_frames_dropped == 1U &&
          after.sequence_gap_frames == 0U);

    CHECK(initialize_pair_policies(
        &unsupported, 2U, 1U, 8U,
        MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE,
        MEISO_STREAM_DEADLINE_UNSUPPORTED
    ));
    frame = frame_value(rgba, sizeof(rgba), 1U, 0U);
    frame.sender_deadline = (MeisoDeadline){true, {43U, 1000U}};
    CHECK(meiso_display_ingress_media_transport_submit(
              unsupported.host, &frame, &equal
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(unsupported.host, &packet));
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              unsupported.host, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              unsupported.edge,
              (MeisoByteView){packet.bytes, packet.size}, &equal
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNSUPPORTED);
    return true;
}

static bool prepare_single_packet(MediaPair *pair, OwnedPacket *packet)
{
    uint8_t rgba[8];
    MeisoDisplayIngressMediaFrame frame;

    fill_frame(rgba, sizeof(rgba), 1U);
    CHECK(initialize_pair(pair, 2U, 1U, 8U));
    frame = frame_value(rgba, sizeof(rgba), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair->host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair->host, packet));
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair->host, &packet->ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    return true;
}

static bool expect_terminal_mutation(
    size_t offset,
    uint8_t replacement,
    bool replace,
    MeisoDisplayIngressMediaTransportStatus expected
)
{
    MediaPair pair;
    MeisoDisplayIngressMediaTransportSnapshot state;
    OwnedPacket packet;

    CHECK(prepare_single_packet(&pair, &packet));
    if (replace) {
        packet.bytes[offset] = replacement;
    } else {
        packet.bytes[offset] ^= 1U;
    }
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == expected);
    CHECK(snapshot(pair.edge, &state));
    CHECK(state.phase ==
              MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL &&
          state.terminal_reason ==
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING &&
          !state.receive_reassembly && !state.receive_latest);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL);
    return true;
}

static bool test_wire_failures_binding_terminal_and_alpha(void)
{
    MediaPair pair;
    MeisoDisplayIngressMediaTransportSnapshot state;
    OwnedPacket packet;
    uint8_t before[TEST_ARENA_CAPACITY];

    CHECK(expect_terminal_mutation(
        23U, 0U, false,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        79U, 0U, false,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        95U, 0U, false,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        103U, 0U, false,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_EPOCH_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        111U, 0U, false,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_EPOCH_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        127U, 0U, false,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NONCE_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        128U, MEISO_OWNER_ROLE_EDGE, true,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OWNER_MISMATCH
    ));
    CHECK(expect_terminal_mutation(
        129U, MEISO_RUNTIME_FACET_ACTUAL, true,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH
    ));

    CHECK(prepare_single_packet(&pair, &packet));
    packet.bytes[130U] = MEISO_RUNTIME_DELIVERY_INLINE;
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    CHECK(snapshot(pair.edge, &state));
    CHECK(state.phase == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE);
    packet.bytes[130U] = MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    packet.bytes[132U] = 1U;
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    packet.bytes[132U] = 0U;
    packet.bytes[204U] = 1U;
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    packet.bytes[204U] = 0U;
    packet.bytes[227U] = 0U;
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge, (MeisoByteView){packet.bytes, 223U}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TRUNCATED);
    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    CHECK(meiso_display_ingress_media_transport_receive_packet(
              pair.edge,
              (MeisoByteView){pair.edge_arena.bytes, packet.size}, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    return true;
}

static bool test_submit_atomic_alpha_alias_and_preemption_overflow(void)
{
    uint8_t rgba1[32];
    uint8_t rgba2[32];
    MediaPair pair;
    MediaPair pending_pair;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaPacketView output;
    MeisoDisplayIngressMediaTransportSnapshot before;
    MeisoDisplayIngressMediaTransportSnapshot after;
    OwnedPacket packet;

    fill_frame(rgba1, sizeof(rgba1), 1U);
    fill_frame(rgba2, sizeof(rgba2), 31U);
    CHECK(initialize_pair(&pair, 4U, 2U, 8U));
    rgba1[3U] = 0U;
    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(snapshot(pair.host, &before));
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    CHECK(snapshot(pair.host, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    rgba1[3U] = UINT8_MAX;
    frame.rgba_frame.data = pair.host_arena.bytes + 2048U;
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(snapshot(pair.host, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    frame = frame_value(rgba2, sizeof(rgba2), 2U, UINT64_MAX);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_display_ingress_media_transport_next_outbound(
              pair.host, &output
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED);
    CHECK(bytes_are((const uint8_t *)&output, 0xa5, sizeof(output)));
    CHECK(snapshot(pair.host, &after));
    CHECK(!after.outbound_packet_pending &&
          after.transport_whole_frames_dropped == 0U &&
          after.send_frame_sequence == 1U && after.send_latest);

    CHECK(initialize_pair(&pending_pair, 4U, 2U, 8U));
    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pending_pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pending_pair.host, &packet));
    frame = frame_value(rgba2, sizeof(rgba2), 2U, UINT64_MAX);
    CHECK(meiso_display_ingress_media_transport_submit(
              pending_pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED);
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pending_pair.host, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(pending_pair.host, &after));
    CHECK(after.send_in_flight && !after.send_latest &&
          after.send_fragment_offset == 8U &&
          after.transport_whole_frames_dropped == 0U);
    return true;
}

static bool test_explicit_terminal_no_resurrection(void)
{
    uint8_t rgba1[8];
    uint8_t rgba2[8];
    MediaPair pair;
    MeisoDisplayIngressMediaBinding wrong;
    MeisoDisplayIngressMediaFrame frame;
    MeisoDisplayIngressMediaPacketView output;
    MeisoDisplayIngressMediaTransportSnapshot state;
    OwnedPacket packet;
    uint8_t before[TEST_ARENA_CAPACITY];
    const size_t primary_offset = 2048U;
    const size_t latest_offset = primary_offset + sizeof(rgba1);
    const size_t scratch_offset = latest_offset + sizeof(rgba2);

    fill_frame(rgba1, sizeof(rgba1), 1U);
    fill_frame(rgba2, sizeof(rgba2), 31U);
    CHECK(initialize_pair(&pair, 2U, 1U, 8U));
    frame = frame_value(rgba1, sizeof(rgba1), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair.host, &packet));
    frame = frame_value(rgba2, sizeof(rgba2), 2U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(pair.host_arena.bytes[primary_offset] != 0U &&
          pair.host_arena.bytes[latest_offset] != 0U &&
          pair.host_arena.bytes[scratch_offset] != 0U);

    wrong = pair.host_config.binding;
    ++wrong.media_stream_epoch;
    memcpy(before, pair.host_arena.bytes, pair.host_bytes);
    CHECK(meiso_display_ingress_media_transport_terminate(
              pair.host, &wrong,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PRODUCTION_CHANGED
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_EPOCH_MISMATCH);
    CHECK(memcmp(before, pair.host_arena.bytes, pair.host_bytes) == 0);
    wrong = pair.host_config.binding;
    ++wrong.runtime.resource.schema.version;
    CHECK(meiso_display_ingress_media_transport_terminate(
              pair.host, &wrong,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CATALOG_CHANGED
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH);
    CHECK(memcmp(before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(meiso_display_ingress_media_transport_terminate(
              pair.host, &pair.host_config.binding,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CONTROL_SESSION
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(pair.host, &state));
    CHECK(state.phase ==
              MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL &&
          state.terminal_reason ==
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CONTROL_SESSION &&
          !state.send_in_flight && !state.send_latest);
    CHECK(bytes_are(
        pair.host_arena.bytes + MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES,
        0U,
        pair.host_bytes - MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES
    ));
    memcpy(before, pair.host_arena.bytes, pair.host_bytes);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_display_ingress_media_transport_next_outbound(
              pair.host, &output
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL);
    CHECK(bytes_are((const uint8_t *)&output, 0xa5, sizeof(output)));
    CHECK(meiso_display_ingress_media_transport_complete_outbound(
              pair.host, &packet.ref
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL);
    CHECK(meiso_display_ingress_media_transport_submit(
              pair.host, &frame, NULL
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL);
    CHECK(meiso_display_ingress_media_transport_terminate(
              pair.host, &pair.host_config.binding,
              MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE
          ) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL);
    CHECK(memcmp(before, pair.host_arena.bytes, pair.host_bytes) == 0);
    return true;
}

static bool pump_h264_frame(MediaPair *pair, bool corrupt_idr, bool replacing)
{
    for (;;) {
        OwnedPacket packet;
        CHECK(capture_packet(pair->host, &packet));
        const bool end = (get_u16(packet.bytes + 6U) & MEISO_DISPLAY_INGRESS_MEDIA_PACKET_END) != 0U;
        const uint32_t offset = packet.ref.fragment_offset;
        if (corrupt_idr && offset <= 19U && 19U < offset + packet.size - 224U)
            packet.bytes[224U + 19U - offset] = 0x61U;
        CHECK(complete_and_receive(pair->host, pair->edge, &packet, NULL,
            end ? (corrupt_idr ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED :
                MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED) :
                (replacing && offset == 0U ? MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_REASSEMBLY_REPLACED :
                 MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED)));
        if (end) return true;
    }
}

static bool test_h264_variable_units_and_recovery(void)
{
    MediaPair pair;
    size_t required, alignment;
    uint8_t a[64], b[37];
    const uint8_t prefix[] = {0,0,0,1,0x67,66,0xc0,31,0xaa,
        0,0,0,1,0x68,0x80,0,0,0,1,0x65,0x88,0x84};
    MeisoDisplayIngressMediaLatestView latest;
    memset(&pair, 0, sizeof(pair));
    memset(a, 0x55, sizeof(a)); memcpy(a, prefix, sizeof(prefix));
    memcpy(b, a, sizeof(b));
    CHECK(meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){a,sizeof(a)}));
    b[19] = 0x61;
    CHECK(!meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){b,sizeof(b)}));
    b[19] = 0x65;
    CHECK(!meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){NULL, sizeof(a)}));
    CHECK(!meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){a, 19U}));
    CHECK(!meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){a + 9U, sizeof(a) - 9U}));
    b[13] = 0x6c; /* Filler cannot replace PPS. */
    CHECK(!meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){b, sizeof(b)}));
    b[13] = 0x68;
    uint8_t two_pictures[128];
    memcpy(two_pictures, a, sizeof(a)); memcpy(two_pictures + sizeof(a), a, sizeof(a));
    CHECK(!meiso_display_ingress_h264_idr_access_unit_valid((MeisoByteView){two_pictures, sizeof(two_pictures)}));
    pair.host_config = transport_config(MEISO_SESSION_ROLE_HOST, 1280U, 720U, 8U,
        MEISO_STREAM_DEADLINE_UNSUPPORTED);
    pair.host_config.frame_format = MEISO_DISPLAY_INGRESS_FRAME_H264_IDR;
    pair.host_config.binding.runtime.resource.max_payload_size = 96U;
    CHECK(meiso_display_ingress_media_transport_memory_required(&pair.host_config,
        &required, &alignment) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_CONFIG);
    pair.host_config.binding.runtime.resource.schema.version = 2U;
    pair.edge_config = pair.host_config;
    pair.edge_config.local_role = MEISO_SESSION_ROLE_EDGE;
    CHECK(meiso_display_ingress_media_transport_memory_required(&pair.host_config,
        &required, &alignment) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK);
    CHECK(required <= TEST_ARENA_CAPACITY);
    CHECK(meiso_display_ingress_media_transport_init(&pair.host_config,
        (MeisoMutableBuffer){pair.host_arena.bytes, required}, &pair.host) == 0);
    CHECK(meiso_display_ingress_media_transport_init(&pair.edge_config,
        (MeisoMutableBuffer){pair.edge_arena.bytes, required}, &pair.edge) == 0);
    uint8_t oversized[97];
    memset(oversized, 0x55, sizeof(oversized)); memcpy(oversized, prefix, sizeof(prefix));
    MeisoDisplayIngressMediaFrame frame = frame_value(oversized, sizeof(oversized), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(pair.host, &frame, NULL) == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED);
    frame = frame_value(a, sizeof(a), 1U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(pair.host, &frame, NULL) == 0);
    OwnedPacket borrowed;
    CHECK(capture_packet(pair.host, &borrowed));
    frame = frame_value(b, sizeof(b), 2U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(pair.host, &frame, NULL) == 0);
    CHECK(complete_and_receive(pair.host, pair.edge, &borrowed, NULL,
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED));
    CHECK(pump_h264_frame(&pair, false, true));
    CHECK(meiso_display_ingress_media_transport_latest(pair.edge, &latest) == 0);
    CHECK(latest.frame.frame_sequence == 2U && latest.frame.access_unit.size == sizeof(b));
    CHECK(!memcmp(latest.frame.access_unit.data, b, sizeof(b)));
    CHECK(meiso_display_ingress_media_transport_release_latest(pair.edge, &latest.ref) == 0);
    frame = frame_value(a, sizeof(a), 3U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(pair.host, &frame, NULL) == 0);
    CHECK(pump_h264_frame(&pair, false, false));
    CHECK(meiso_display_ingress_media_transport_latest(pair.edge, &latest) == 0);
    CHECK(latest.frame.access_unit.size == sizeof(a));
    CHECK(meiso_display_ingress_media_transport_release_latest(pair.edge, &latest.ref) == 0);
    frame = frame_value(a, sizeof(a), 4U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(pair.host, &frame, NULL) == 0);
    CHECK(pump_h264_frame(&pair, true, false));
    CHECK(meiso_display_ingress_media_transport_latest(pair.edge, &latest) ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME);
    frame = frame_value(b, sizeof(b), 5U, 0U);
    CHECK(meiso_display_ingress_media_transport_submit(pair.host, &frame, NULL) == 0);
    CHECK(pump_h264_frame(&pair, false, false));
    CHECK(meiso_display_ingress_media_transport_latest(pair.edge, &latest) == 0);
    CHECK(latest.frame.frame_sequence == 5U);
    return true;
}

int main(void)
{
    if (!test_h264_variable_units_and_recovery() ||
        !test_memory_init_exact_profile_and_alias_contract() ||
        !test_golden_packet_idempotence_and_exact_refs() ||
        !test_fragment_boundary_preemption_latest_and_release() ||
        !test_late_older_frame_is_not_published() ||
        !test_discontinuity_recovery_and_separate_counters() ||
        !test_idle_fragment_boundary_preemption() ||
        !test_borrowed_packet_stable_across_latest_replacement() ||
        !test_deadline_inheritance() ||
        !test_wire_failures_binding_terminal_and_alpha() ||
        !test_submit_atomic_alpha_alias_and_preemption_overflow() ||
        !test_explicit_terminal_no_resurrection()) {
        return 1;
    }
    puts("display ingress media transport tests passed");
    return 0;
}
