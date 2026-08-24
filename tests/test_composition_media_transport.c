#include "composition_media_transport.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEST_ARENA_CAPACITY 8192U
#define TEST_PACKET_CAPACITY MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES

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
    MeisoCompositionMediaTransportConfig host_config;
    MeisoCompositionMediaTransportConfig edge_config;
    TestArena host_arena;
    TestArena edge_arena;
    size_t host_bytes;
    size_t edge_bytes;
    MeisoCompositionMediaTransport *host;
    MeisoCompositionMediaTransport *edge;
} MediaPair;

typedef struct {
    MeisoCompositionMediaPacketRef ref;
    uint8_t bytes[TEST_PACKET_CAPACITY];
    size_t size;
} OwnedPacket;

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t i;
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

static bool session_id_equal(MeisoSessionId lhs, MeisoSessionId rhs)
{
    return meiso_session_id_equal(lhs, rhs);
}

static bool binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        lhs->endpoint_id.high == rhs->endpoint_id.high &&
        lhs->endpoint_id.low == rhs->endpoint_id.low &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        lhs->resource_id.high == rhs->resource_id.high &&
        lhs->resource_id.low == rhs->resource_id.low &&
        lhs->source == rhs->source &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

static bool packet_ref_equal(
    const MeisoCompositionMediaPacketRef *lhs,
    const MeisoCompositionMediaPacketRef *rhs
)
{
    return session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        lhs->serial == rhs->serial && lhs->kind == rhs->kind &&
        lhs->source == rhs->source &&
        lhs->frame_sequence == rhs->frame_sequence &&
        lhs->fragment_offset == rhs->fragment_offset &&
        lhs->packet_bytes == rhs->packet_bytes;
}

static MeisoCompositionMediaTransportConfig transport_config(
    MeisoSessionRole role
)
{
    MeisoCompositionMediaTransportConfig value;
    size_t render_count = 0U;
    size_t camera_count = 0U;
    const MeisoResourceDescriptor *render_resources =
        meiso_composition_render_media_resources(&render_count);
    const MeisoResourceDescriptor *camera_resources =
        meiso_composition_camera_media_resources(&camera_count);
    memset(&value, 0, sizeof(value));
    value.local_role = role;
    value.scope.session_id.host_nonce = (MeisoSessionNonce){11U, 12U};
    value.scope.session_id.edge_nonce = (MeisoSessionNonce){13U, 14U};
    value.scope.catalog_generation = 17U;

    value.routes[0].binding.session_id = value.scope.session_id;
    value.routes[0].binding.catalog_generation =
        value.scope.catalog_generation;
    value.routes[0].binding.endpoint_id =
        *meiso_composition_render_media_endpoint();
    value.routes[0].binding.endpoint_incarnation = 21U;
    if (render_resources != NULL && render_count == 3U) {
        value.routes[0].binding.resource_id = render_resources[2].id;
    }
    value.routes[0].binding.source =
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    value.routes[0].binding.production_epoch = 23U;
    value.routes[0].binding.media_stream_epoch = 29U;
    value.routes[0].binding.channel_nonce = (MeisoSessionNonce){31U, 37U};
    value.routes[0].capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        2U, 1U, 2U, 1U, 1000U, 8U, 3U, 0U
    };
    value.routes[0].effective = (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        2U, 1U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };

    value.routes[1].binding.session_id = value.scope.session_id;
    value.routes[1].binding.catalog_generation =
        value.scope.catalog_generation;
    value.routes[1].binding.endpoint_id =
        *meiso_composition_camera_media_endpoint();
    value.routes[1].binding.endpoint_incarnation = 22U;
    if (camera_resources != NULL && camera_count == 3U) {
        value.routes[1].binding.resource_id = camera_resources[2].id;
    }
    value.routes[1].binding.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    value.routes[1].binding.production_epoch = 24U;
    value.routes[1].binding.media_stream_epoch = 30U;
    value.routes[1].binding.channel_nonce = (MeisoSessionNonce){41U, 43U};
    value.routes[1].capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        2U, 2U, 2U, 2U, 1000U, 6U, 2U, 0U
    };
    value.routes[1].effective = (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        2U, 2U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    return value;
}

static bool snapshot(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaTransportSnapshot *out_snapshot
)
{
    return meiso_composition_media_transport_snapshot(
               transport, out_snapshot
           ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static bool capture_packet(
    MeisoCompositionMediaTransport *transport,
    OwnedPacket *out_packet
)
{
    MeisoCompositionMediaPacketView view;
    memset(&view, 0, sizeof(view));
    if (meiso_composition_media_transport_next_outbound(transport, &view) !=
            MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        view.bytes.data == NULL || view.bytes.size > sizeof(out_packet->bytes)) {
        return false;
    }
    out_packet->ref = view.ref;
    out_packet->size = view.bytes.size;
    memcpy(out_packet->bytes, view.bytes.data, view.bytes.size);
    return true;
}

static bool complete_and_receive(
    MeisoCompositionMediaTransport *sender,
    MeisoCompositionMediaTransport *receiver,
    OwnedPacket *packet,
    MeisoCompositionMediaTransportStatus expected_receive
)
{
    CHECK(meiso_composition_media_transport_complete_outbound(
              sender, &packet->ref
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_receive_packet(
              receiver, (MeisoByteView){packet->bytes, packet->size}
          ) == expected_receive);
    return true;
}

static bool initialize_pair(MediaPair *pair, bool handshake)
{
    size_t alignment = 0U;
    memset(pair, 0, sizeof(*pair));
    pair->host_config = transport_config(MEISO_SESSION_ROLE_HOST);
    pair->edge_config = transport_config(MEISO_SESSION_ROLE_EDGE);
    CHECK(meiso_composition_media_transport_memory_required(
              &pair->host_config, &pair->host_bytes, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(alignment == alignof(max_align_t));
    CHECK(pair->host_bytes <= sizeof(pair->host_arena.bytes));
    CHECK(meiso_composition_media_transport_memory_required(
              &pair->edge_config, &pair->edge_bytes, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(pair->edge_bytes == pair->host_bytes);
    CHECK(meiso_composition_media_transport_init(
              &pair->host_config,
              (MeisoMutableBuffer){pair->host_arena.bytes, pair->host_bytes},
              &pair->host
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_init(
              &pair->edge_config,
              (MeisoMutableBuffer){pair->edge_arena.bytes, pair->edge_bytes},
              &pair->edge
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    if (handshake) {
        OwnedPacket packet;
        size_t i;
        CHECK(capture_packet(pair->host, &packet));
        CHECK(packet.ref.kind == MEISO_COMPOSITION_MEDIA_PACKET_OPEN);
        CHECK(complete_and_receive(
            pair->host, pair->edge, &packet,
            MEISO_COMPOSITION_MEDIA_TRANSPORT_OK
        ));
        for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
            CHECK(capture_packet(pair->edge, &packet));
            CHECK(packet.ref.kind == MEISO_COMPOSITION_MEDIA_PACKET_BIND);
            CHECK(packet.ref.source == pair->edge_config.routes[i].binding.source);
            CHECK(complete_and_receive(
                pair->edge, pair->host, &packet,
                MEISO_COMPOSITION_MEDIA_TRANSPORT_OK
            ));
        }
    }
    return true;
}

static bool initialize_single_fragment_pair(MediaPair *pair)
{
    size_t alignment = 0U;
    OwnedPacket packet;
    size_t i;
    memset(pair, 0, sizeof(*pair));
    pair->host_config = transport_config(MEISO_SESSION_ROLE_HOST);
    pair->edge_config = transport_config(MEISO_SESSION_ROLE_EDGE);
    pair->host_config.routes[0].capability.max_fragment_payload_bytes = 8U;
    pair->host_config.routes[1].capability.max_fragment_payload_bytes = 6U;
    pair->edge_config.routes[0].capability.max_fragment_payload_bytes = 8U;
    pair->edge_config.routes[1].capability.max_fragment_payload_bytes = 6U;
    CHECK(meiso_composition_media_transport_memory_required(
              &pair->host_config, &pair->host_bytes, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_memory_required(
              &pair->edge_config, &pair->edge_bytes, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(pair->host_bytes == pair->edge_bytes &&
          pair->host_bytes <= TEST_ARENA_CAPACITY);
    CHECK(meiso_composition_media_transport_init(
              &pair->host_config,
              (MeisoMutableBuffer){pair->host_arena.bytes, pair->host_bytes},
              &pair->host
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_init(
              &pair->edge_config,
              (MeisoMutableBuffer){pair->edge_arena.bytes, pair->edge_bytes},
              &pair->edge
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(capture_packet(pair->host, &packet));
    CHECK(complete_and_receive(
        pair->host, pair->edge, &packet,
        MEISO_COMPOSITION_MEDIA_TRANSPORT_OK
    ));
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        CHECK(capture_packet(pair->edge, &packet));
        CHECK(complete_and_receive(
            pair->edge, pair->host, &packet,
            MEISO_COMPOSITION_MEDIA_TRANSPORT_OK
        ));
    }
    return true;
}

static MeisoCompositionMediaUnit media_unit(
    const MediaPair *pair,
    size_t route_index,
    uint64_t sequence,
    uint64_t producer_drops,
    const uint8_t *bytes
)
{
    MeisoCompositionMediaUnit value;
    const MeisoCompositionMediaRouteConfig *route =
        &pair->edge_config.routes[route_index];
    memset(&value, 0, sizeof(value));
    value.binding = route->binding;
    value.frame_sequence = sequence;
    value.source_time = (MeisoTimePoint){7U, sequence * 100U};
    if (route_index == 0U) {
        value.applied_scene_owner =
            (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 47U};
        value.applied_scene_state_version = 53U;
        value.active_plan_generation = 59U;
        value.world_generation = 61U;
    }
    value.producer_dropped_frames = producer_drops;
    value.pixel_format = route->effective.pixel_format;
    value.codec = route->effective.codec;
    value.width = route->effective.width;
    value.height = route->effective.height;
    value.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    value.access_unit.data = bytes;
    value.access_unit.size = route->capability.max_access_unit_bytes;
    return value;
}

static MeisoCompositionMediaFrame media_frame(
    const MediaPair *pair,
    size_t route_index,
    uint64_t sequence,
    uint64_t producer_drops,
    uint32_t offset,
    const uint8_t *fragment,
    size_t fragment_size
)
{
    const MeisoCompositionMediaUnit unit = media_unit(
        pair, route_index, sequence, producer_drops, fragment
    );
    MeisoCompositionMediaFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.binding = unit.binding;
    frame.frame_sequence = sequence;
    frame.source_time = unit.source_time;
    frame.applied_scene_owner = unit.applied_scene_owner;
    frame.applied_scene_state_version = unit.applied_scene_state_version;
    frame.active_plan_generation = unit.active_plan_generation;
    frame.world_generation = unit.world_generation;
    frame.producer_dropped_frames = producer_drops;
    frame.pixel_format = unit.pixel_format;
    frame.codec = unit.codec;
    frame.width = unit.width;
    frame.height = unit.height;
    frame.access_unit_bytes =
        pair->edge_config.routes[route_index].capability.max_access_unit_bytes;
    frame.fragment_offset = offset;
    frame.fragment = (MeisoByteView){fragment, fragment_size};
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    if (offset == 0U) {
        frame.flags |= MEISO_COMPOSITION_MEDIA_FRAME_BEGIN;
    }
    if ((size_t)offset + fragment_size == frame.access_unit_bytes) {
        frame.flags |= MEISO_COMPOSITION_MEDIA_FRAME_END;
    }
    return frame;
}

static bool encode_frame(
    const MeisoCompositionMediaFrame *frame,
    OwnedPacket *out_packet
)
{
    size_t size = 0U;
    memset(out_packet, 0, sizeof(*out_packet));
    CHECK(meiso_composition_media_frame_encode(
              frame,
              (MeisoMutableBuffer){out_packet->bytes,
                                   sizeof(out_packet->bytes)},
              &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    out_packet->size = size;
    return true;
}

static bool receive_frame(
    MeisoCompositionMediaTransport *host,
    const MeisoCompositionMediaFrame *frame,
    MeisoCompositionMediaTransportStatus expected
)
{
    OwnedPacket packet;
    CHECK(encode_frame(frame, &packet));
    CHECK(meiso_composition_media_transport_receive_packet(
              host, (MeisoByteView){packet.bytes, packet.size}
          ) == expected);
    return true;
}

static bool test_memory_init_and_alias_contract(void)
{
    MeisoCompositionMediaTransportConfig config =
        transport_config(MEISO_SESSION_ROLE_HOST);
    MeisoCompositionMediaTransportConfig invalid = config;
    TestArena arena;
    TestArena before;
    size_t required = 0U;
    size_t alignment = 0U;
    size_t size_sentinel = 91U;
    size_t alignment_sentinel = 92U;
    MeisoCompositionMediaTransport *transport =
        (MeisoCompositionMediaTransport *)(uintptr_t)0x1234U;
    MeisoCompositionMediaTransport *transport_before = transport;
    union {
        max_align_t alignment;
        MeisoCompositionMediaTransportConfig config;
        size_t size;
    } config_alias;

    CHECK(meiso_composition_media_transport_memory_required(
              &config, &required, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(required == MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES +
              2U * (8U + 6U) +
              MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + 3U);
    CHECK(alignment == alignof(max_align_t));
    CHECK(meiso_composition_media_transport_memory_required(
              &config, &required, &required
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    invalid.local_role = 0U;
    CHECK(meiso_composition_media_transport_memory_required(
              &invalid, &size_sentinel, &alignment_sentinel
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG);
    CHECK(size_sentinel == 91U && alignment_sentinel == 92U);
    config_alias.config = config;
    alignment_sentinel = 92U;
    CHECK(meiso_composition_media_transport_memory_required(
              &config_alias.config, &config_alias.size, &alignment_sentinel
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(alignment_sentinel == 92U);

    memset(&arena, 0xa5, sizeof(arena));
    before = arena;
    CHECK(meiso_composition_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes, required - 1U}, &transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY);
    CHECK(transport == transport_before &&
          memcmp(&arena, &before, sizeof(arena)) == 0);
    CHECK(meiso_composition_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes + 1U, required}, &transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY);
    CHECK(transport == transport_before &&
          memcmp(&arena, &before, sizeof(arena)) == 0);
    CHECK(meiso_composition_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes, required},
              (MeisoCompositionMediaTransport **)(void *)arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);
    memcpy(arena.bytes, &config, sizeof(config));
    before = arena;
    CHECK(meiso_composition_media_transport_init(
              (const MeisoCompositionMediaTransportConfig *)(const void *)
                  arena.bytes,
              (MeisoMutableBuffer){arena.bytes, required}, &transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(transport == transport_before &&
          memcmp(&arena, &before, sizeof(arena)) == 0);

    memset(&arena, 0xa5, sizeof(arena));
    before = arena;
    CHECK(meiso_composition_media_transport_init(
              &config,
              (MeisoMutableBuffer){arena.bytes, required}, &transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(transport == (MeisoCompositionMediaTransport *)(void *)arena.bytes);
    CHECK(memcmp(arena.bytes + required, before.bytes + required,
                 sizeof(arena.bytes) - required) == 0);
    return true;
}

static bool test_handshake_would_block_and_aliases(void)
{
    MediaPair pair;
    OwnedPacket open;
    OwnedPacket repeated;
    uint8_t host_before[TEST_ARENA_CAPACITY];
    uint8_t edge_before[TEST_ARENA_CAPACITY];
    MeisoCompositionMediaTransportSnapshot host_snapshot;
    MeisoCompositionMediaTransportSnapshot edge_snapshot;
    MeisoCompositionMediaPacketRef wrong;
    MeisoCompositionMediaPacketView sentinel;
    size_t i;
    CHECK(initialize_pair(&pair, false));
    CHECK(snapshot(pair.host, &host_snapshot));
    CHECK(snapshot(pair.edge, &edge_snapshot));
    CHECK(host_snapshot.phase ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING);
    CHECK(edge_snapshot.phase ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN);

    CHECK(capture_packet(pair.host, &open));
    memcpy(host_before, pair.host_arena.bytes, pair.host_bytes);
    CHECK(capture_packet(pair.host, &repeated));
    CHECK(packet_ref_equal(&open.ref, &repeated.ref));
    CHECK(open.size == repeated.size &&
          memcmp(open.bytes, repeated.bytes, open.size) == 0);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    wrong = open.ref;
    ++wrong.serial;
    CHECK(meiso_composition_media_transport_complete_outbound(
              pair.host, &wrong
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(complete_and_receive(
        pair.host, pair.edge, &open, MEISO_COMPOSITION_MEDIA_TRANSPORT_OK
    ));

    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        OwnedPacket bind;
        MeisoCompositionMediaBinding decoded;
        CHECK(capture_packet(pair.edge, &bind));
        CHECK(bind.ref.kind == MEISO_COMPOSITION_MEDIA_PACKET_BIND);
        CHECK(meiso_composition_media_binding_decode(
                  (MeisoByteView){bind.bytes, bind.size}, &decoded
              ) == MEISO_COMPOSITION_SESSION_V3_OK);
        CHECK(binding_equal(&decoded, &pair.edge_config.routes[i].binding));
        CHECK(complete_and_receive(
            pair.edge, pair.host, &bind,
            MEISO_COMPOSITION_MEDIA_TRANSPORT_OK
        ));
    }
    CHECK(snapshot(pair.host, &host_snapshot));
    CHECK(snapshot(pair.edge, &edge_snapshot));
    CHECK(host_snapshot.phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE);
    CHECK(edge_snapshot.phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE);
    CHECK(host_snapshot.bound_route_mask == UINT8_C(0x03));
    CHECK(edge_snapshot.bound_route_mask == UINT8_C(0x03));

    memcpy(host_before, pair.host_arena.bytes, pair.host_bytes);
    memset(&sentinel, 0xa5, sizeof(sentinel));
    CHECK(meiso_composition_media_transport_next_outbound(
              pair.host,
              (MeisoCompositionMediaPacketView *)(void *)pair.host_arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(meiso_composition_media_transport_snapshot(
              pair.host,
              (MeisoCompositionMediaTransportSnapshot *)(void *)
                  pair.host_arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              (MeisoCompositionMediaLatestView *)(void *)pair.host_arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(meiso_composition_media_transport_receive_packet(
              pair.host,
              (MeisoByteView){pair.host_arena.bytes,
                              MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE}
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);

    memcpy(edge_before, pair.edge_arena.bytes, pair.edge_bytes);
    CHECK(meiso_composition_media_transport_complete_outbound(
              pair.edge,
              (MeisoCompositionMediaPacketRef *)(void *)pair.edge_arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(edge_before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    return true;
}

static bool test_edge_queue_replacement_and_fragment_rr(void)
{
    static const uint8_t render1[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    static const uint8_t render2[8] = {11U,12U,13U,14U,15U,16U,17U,18U};
    static const uint8_t render3[8] = {21U,22U,23U,24U,25U,26U,27U,28U};
    static const uint8_t camera1[6] = {31U,32U,33U,34U,35U,36U};
    MediaPair pair;
    MeisoCompositionMediaUnit unit;
    MeisoCompositionMediaTransportSnapshot snapshot_value;
    MeisoCompositionMediaLatestView latest;
    MeisoCompositionMediaSource sources[9];
    uint64_t sequences[9];
    size_t packet_count = 0U;
    CHECK(initialize_pair(&pair, true));

    unit = media_unit(&pair, 0U, 1U, 0U, render1);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    unit = media_unit(&pair, 1U, 1U, 0U, camera1);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    unit = media_unit(&pair, 0U, 2U, 0U, render2);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    unit = media_unit(&pair, 0U, 3U, 1U, render3);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED);
    CHECK(snapshot(pair.edge, &snapshot_value));
    CHECK(snapshot_value.routes[0].send_in_flight);
    CHECK(snapshot_value.routes[0].send_latest);
    CHECK(snapshot_value.routes[0].send_frame_sequence == 1U);
    CHECK(snapshot_value.routes[0].submitted_sequence_high_water == 3U);
    CHECK(snapshot_value.routes[0].transport_whole_frames_dropped == 1U);

    while (true) {
        OwnedPacket packet;
        const MeisoCompositionMediaTransportStatus status =
            meiso_composition_media_transport_next_outbound(
                pair.edge,
                &(MeisoCompositionMediaPacketView){0}
            );
        if (status == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET) {
            break;
        }
        CHECK(status == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
        CHECK(packet_count < sizeof(sources) / sizeof(sources[0]));
        CHECK(capture_packet(pair.edge, &packet));
        sources[packet_count] = packet.ref.source;
        sequences[packet_count] = packet.ref.frame_sequence;
        CHECK(complete_and_receive(
            pair.edge, pair.host, &packet,
            packet.ref.fragment_offset +
                    (packet.ref.packet_bytes -
                     MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) ==
                pair.edge_config.routes[packet.ref.source - 1U]
                    .capability.max_access_unit_bytes ?
                MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED :
                MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
        ));
        ++packet_count;
    }
    CHECK(packet_count == 9U);
    CHECK(sources[0] == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW);
    CHECK(sources[1] == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA);
    CHECK(sources[2] == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW);
    CHECK(sources[3] == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA);
    CHECK(sources[4] == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW);
    CHECK(sources[5] == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA);
    CHECK(sequences[0] == 1U && sequences[2] == 1U && sequences[4] == 1U);
    CHECK(sequences[6] == 3U && sequences[7] == 3U && sequences[8] == 3U);

    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &latest
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(latest.ref.frame_sequence == 3U);
    CHECK(latest.unit.producer_dropped_frames == 1U);
    CHECK(latest.unit.access_unit.size == sizeof(render3));
    CHECK(memcmp(latest.unit.access_unit.data, render3, sizeof(render3)) == 0);
    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA, &latest
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(latest.ref.frame_sequence == 1U);
    CHECK(memcmp(latest.unit.access_unit.data, camera1, sizeof(camera1)) == 0);
    CHECK(snapshot(pair.host, &snapshot_value));
    CHECK(snapshot_value.routes[0].published_sequence_high_water == 3U);
    CHECK(snapshot_value.routes[0].sequence_gap_frames == 1U);
    CHECK(snapshot_value.routes[0].producer_drop_high_water == 1U);
    CHECK(snapshot_value.routes[0].unread_latest_frames_dropped == 1U);
    return true;
}

static bool assert_host_arena_unchanged(
    MediaPair *pair,
    const MeisoCompositionMediaFrame *frame,
    MeisoCompositionMediaTransportStatus expected
)
{
    uint8_t before[TEST_ARENA_CAPACITY];
    OwnedPacket packet;
    memcpy(before, pair->host_arena.bytes, pair->host_bytes);
    CHECK(encode_frame(frame, &packet));
    CHECK(meiso_composition_media_transport_receive_packet(
              pair->host, (MeisoByteView){packet.bytes, packet.size}
          ) == expected);
    CHECK(memcmp(before, pair->host_arena.bytes, pair->host_bytes) == 0);
    return true;
}

static bool test_host_reassembly_failures_and_latest(void)
{
    static const uint8_t bytes1[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    static const uint8_t bytes2[8] = {9U,10U,11U,12U,13U,14U,15U,16U};
    MediaPair pair;
    MeisoCompositionMediaFrame frame;
    MeisoCompositionMediaTransportSnapshot before_snapshot;
    MeisoCompositionMediaTransportSnapshot after_snapshot;
    MeisoCompositionMediaLatestView latest;
    MeisoCompositionMediaFrameRef wrong_ref;
    OwnedPacket packet;
    uint8_t host_before[TEST_ARENA_CAPACITY];
    CHECK(initialize_pair(&pair, true));

    frame = media_frame(&pair, 0U, 10U, 2U, 0U, bytes1, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    CHECK(snapshot(pair.host, &before_snapshot));
    CHECK(before_snapshot.routes[0].receive_reassembly);
    CHECK(before_snapshot.routes[0].receive_next_offset == 3U);

    frame = media_frame(&pair, 0U, 10U, 2U, 3U, bytes1 + 3U, 3U);
    ++frame.binding.endpoint_incarnation;
    CHECK(assert_host_arena_unchanged(
        &pair, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH
    ));
    frame.binding = pair.host_config.routes[0].binding;
    ++frame.binding.production_epoch;
    CHECK(assert_host_arena_unchanged(
        &pair, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_EPOCH_MISMATCH
    ));
    frame.binding = pair.host_config.routes[0].binding;
    ++frame.binding.channel_nonce.low;
    CHECK(assert_host_arena_unchanged(
        &pair, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_NONCE_MISMATCH
    ));
    frame.binding = pair.host_config.routes[0].binding;
    ++frame.source_time.monotonic_ns;
    CHECK(assert_host_arena_unchanged(
        &pair, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT
    ));

    frame = media_frame(&pair, 0U, 10U, 2U, 4U, bytes1 + 4U, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY
    ));
    CHECK(snapshot(pair.host, &after_snapshot));
    CHECK(!after_snapshot.routes[0].receive_reassembly);
    CHECK(after_snapshot.routes[0].reassembly_frames_dropped ==
          before_snapshot.routes[0].reassembly_frames_dropped + 1U);

    frame = media_frame(&pair, 0U, 11U, 2U, 0U, bytes1, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    frame = media_frame(&pair, 0U, 11U, 2U, 2U, bytes1 + 2U, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY
    ));

    frame = media_frame(&pair, 0U, 12U, 3U, 0U, bytes1, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    frame = media_frame(&pair, 0U, 13U, 2U, 0U, bytes2, 3U);
    CHECK(assert_host_arena_unchanged(
        &pair, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY
    ));
    frame = media_frame(&pair, 0U, 13U, 3U, 0U, bytes2, 3U);
    CHECK(receive_frame(
        pair.host, &frame,
        MEISO_COMPOSITION_MEDIA_TRANSPORT_REASSEMBLY_REPLACED
    ));
    frame = media_frame(&pair, 0U, 13U, 3U, 3U, bytes2 + 3U, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    frame = media_frame(&pair, 0U, 13U, 3U, 6U, bytes2 + 6U, 2U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED
    ));
    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &latest
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(latest.ref.frame_sequence == 13U);
    CHECK(memcmp(latest.unit.access_unit.data, bytes2, sizeof(bytes2)) == 0);

    frame = media_frame(&pair, 0U, 15U, 4U, 0U, bytes1, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    frame = media_frame(&pair, 0U, 15U, 4U, 3U, bytes1 + 3U, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    frame = media_frame(&pair, 0U, 15U, 4U, 6U, bytes1 + 6U, 2U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED
    ));
    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &latest
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(latest.ref.frame_sequence == 15U);
    CHECK(memcmp(latest.unit.access_unit.data, bytes1, sizeof(bytes1)) == 0);
    CHECK(snapshot(pair.host, &after_snapshot));
    CHECK(after_snapshot.routes[0].sequence_gap_frames == 13U);
    CHECK(after_snapshot.routes[0].producer_drop_high_water == 4U);
    CHECK(after_snapshot.routes[0].unread_latest_frames_dropped == 1U);

    wrong_ref = latest.ref;
    --wrong_ref.frame_sequence;
    memcpy(host_before, pair.host_arena.bytes, pair.host_bytes);
    CHECK(meiso_composition_media_transport_release_latest(
              pair.host, &wrong_ref
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(meiso_composition_media_transport_release_latest(
              pair.host, &latest.ref
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    memset(&latest, 0xa5, sizeof(latest));
    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &latest
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME);
    CHECK(bytes_are((const uint8_t *)&latest, 0xa5, sizeof(latest)));

    frame = media_frame(&pair, 0U, 16U, 4U, 0U, bytes1, 3U);
    CHECK(encode_frame(&frame, &packet));
    packet.bytes[224] = 1U;
    memcpy(host_before, pair.host_arena.bytes, pair.host_bytes);
    CHECK(meiso_composition_media_transport_receive_packet(
              pair.host, (MeisoByteView){packet.bytes, packet.size}
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    CHECK(meiso_composition_media_transport_receive_packet(
              pair.host, (MeisoByteView){packet.bytes, 3U}
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED);
    CHECK(memcmp(host_before, pair.host_arena.bytes, pair.host_bytes) == 0);
    return true;
}

static bool test_complete_replacement_and_u64_sequence_boundary(void)
{
    static const uint8_t old_bytes[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    static const uint8_t new_bytes[8] = {9U,10U,11U,12U,13U,14U,15U,16U};
    MediaPair pair;
    MeisoCompositionMediaFrame frame;
    MeisoCompositionMediaLatestView latest;
    MeisoCompositionMediaTransportSnapshot snapshot_value;
    uint8_t before[TEST_ARENA_CAPACITY];
    CHECK(initialize_single_fragment_pair(&pair));

    frame = media_frame(&pair, 0U, 1U, 0U, 0U, old_bytes, 3U);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED
    ));
    frame = media_frame(&pair, 0U, 2U, 0U, 0U, new_bytes, sizeof(new_bytes));
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED
    ));
    CHECK(meiso_composition_media_transport_latest(
              pair.host, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &latest
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(latest.ref.frame_sequence == 2U &&
          memcmp(latest.unit.access_unit.data, new_bytes,
                 sizeof(new_bytes)) == 0);
    CHECK(snapshot(pair.host, &snapshot_value));
    CHECK(snapshot_value.routes[0].reassembly_frames_dropped == 1U);

    CHECK(meiso_composition_media_transport_release_latest(
              pair.host, &latest.ref
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    frame = media_frame(
        &pair, 0U, UINT64_MAX, UINT64_MAX, 0U, old_bytes,
        sizeof(old_bytes)
    );
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED
    ));
    CHECK(snapshot(pair.host, &snapshot_value));
    CHECK(snapshot_value.routes[0].published_sequence_high_water == UINT64_MAX);
    CHECK(snapshot_value.routes[0].producer_drop_high_water == UINT64_MAX);
    CHECK(snapshot_value.routes[0].sequence_gap_frames == UINT64_MAX - 2U);
    memcpy(before, pair.host_arena.bytes, pair.host_bytes);
    CHECK(receive_frame(
        pair.host, &frame, MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY
    ));
    CHECK(memcmp(before, pair.host_arena.bytes, pair.host_bytes) == 0);
    return true;
}

static bool test_submit_identity_and_atomic_failures(void)
{
    static const uint8_t render[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    MediaPair pair;
    MeisoCompositionMediaUnit unit;
    uint8_t before[TEST_ARENA_CAPACITY];
    CHECK(initialize_pair(&pair, true));
    unit = media_unit(&pair, 0U, 1U, 0U, render);

    ++unit.binding.endpoint_incarnation;
    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit.binding = pair.edge_config.routes[0].binding;
    ++unit.binding.production_epoch;
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_EPOCH_MISMATCH);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit.binding = pair.edge_config.routes[0].binding;
    ++unit.binding.channel_nonce.high;
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_NONCE_MISMATCH);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit.binding = pair.edge_config.routes[0].binding;
    unit.flags |= MEISO_COMPOSITION_MEDIA_FRAME_BEGIN;
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    unit.access_unit.size = 7U;
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit.access_unit.size = 8U;
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit = media_unit(&pair, 0U, 2U, 0U, render);
    unit.producer_dropped_frames = 0U;
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    unit = media_unit(&pair, 0U, 3U, 2U, render);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED);
    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    unit = media_unit(&pair, 0U, 4U, 1U, render);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);

    CHECK(meiso_composition_media_transport_submit(
              pair.edge,
              (MeisoCompositionMediaUnit *)(void *)pair.edge_arena.bytes
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    unit = media_unit(&pair, 0U, 2U, 0U, pair.edge_arena.bytes);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    return true;
}

static bool test_terminal_scope_and_no_resurrection(void)
{
    static const uint8_t render[8] = {1U,2U,3U,4U,5U,6U,7U,8U};
    MediaPair pair;
    MeisoCompositionMediaUnit unit;
    MeisoCompositionMediaOpen wrong_scope;
    MeisoCompositionMediaTransportSnapshot terminal_snapshot;
    MeisoCompositionMediaPacketView packet;
    uint8_t before[TEST_ARENA_CAPACITY];
    CHECK(initialize_pair(&pair, true));
    unit = media_unit(&pair, 0U, 1U, 0U, render);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(meiso_composition_media_transport_next_outbound(pair.edge, &packet) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);

    wrong_scope = pair.edge_config.scope;
    ++wrong_scope.catalog_generation;
    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    CHECK(meiso_composition_media_transport_terminate(
              pair.edge,
              (MeisoCompositionMediaOpen *)(void *)pair.edge_arena.bytes,
              MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    CHECK(meiso_composition_media_transport_terminate(
              pair.edge, &wrong_scope,
              MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    CHECK(meiso_composition_media_transport_terminate(
              pair.edge, &pair.edge_config.scope, 0U
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    CHECK(meiso_composition_media_transport_terminate(
              pair.edge, &pair.edge_config.scope,
              MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    CHECK(snapshot(pair.edge, &terminal_snapshot));
    CHECK(terminal_snapshot.phase ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL);
    CHECK(terminal_snapshot.terminal_reason ==
          MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION);
    CHECK(terminal_snapshot.bound_route_mask == 0U);
    CHECK(!terminal_snapshot.outbound_packet_pending);
    CHECK(!terminal_snapshot.routes[0].send_in_flight);
    CHECK(!terminal_snapshot.routes[0].send_latest);

    memcpy(before, pair.edge_arena.bytes, pair.edge_bytes);
    CHECK(meiso_composition_media_transport_next_outbound(pair.edge, &packet) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL);
    CHECK(meiso_composition_media_transport_complete_outbound(
              pair.edge, &packet.ref
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL);
    CHECK(meiso_composition_media_transport_submit(pair.edge, &unit) ==
          MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL);
    CHECK(meiso_composition_media_transport_receive_packet(
              pair.edge, (MeisoByteView){render, sizeof(render)}
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL);
    CHECK(meiso_composition_media_transport_terminate(
              pair.edge, &pair.edge_config.scope,
              MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL);
    CHECK(memcmp(before, pair.edge_arena.bytes, pair.edge_bytes) == 0);
    return true;
}

int main(void)
{
    if (!test_memory_init_and_alias_contract() ||
        !test_handshake_would_block_and_aliases() ||
        !test_edge_queue_replacement_and_fragment_rr() ||
        !test_host_reassembly_failures_and_latest() ||
        !test_complete_replacement_and_u64_sequence_boundary() ||
        !test_submit_identity_and_atomic_failures() ||
        !test_terminal_scope_and_no_resurrection()) {
        return 1;
    }
    puts("composition media transport tests passed");
    return 0;
}
