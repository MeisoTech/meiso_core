#include "composition_media_transport.h"

#include <limits.h>
#include <stdalign.h>
#include <string.h>

enum {
    MEDIA_TRANSPORT_MAGIC = 0x4d545250U, /* MTRP */
    MEDIA_OPEN_MAGIC = 0x4d4f504eU,      /* MOPN */
    MEDIA_BIND_MAGIC = 0x4d424e44U,      /* MBND */
    MEDIA_FRAME_MAGIC = 0x4d46524dU      /* MFRM */
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
} MediaFrameMetadata;

typedef struct {
    bool occupied;
    MediaFrameMetadata metadata;
    uint8_t *bytes;
    uint32_t capacity;
} MediaFrameSlot;

typedef struct {
    MeisoCompositionMediaRouteConfig config;
    MediaFrameSlot primary;
    MediaFrameSlot latest;
    uint32_t primary_offset;
    uint64_t submitted_sequence_high_water;
    uint64_t submitted_producer_drop_high_water;
    uint64_t transport_whole_frames_dropped;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
} MediaRouteState;

struct MeisoCompositionMediaTransport {
    uint32_t magic;
    MeisoSessionRole local_role;
    MeisoCompositionMediaTransportPhase phase;
    MeisoCompositionMediaTerminalReason terminal_reason;
    MeisoCompositionMediaOpen scope;
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
    MediaRouteState routes[MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT];
};

_Static_assert(
    sizeof(struct MeisoCompositionMediaTransport) <=
        MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES,
    "media transport metadata arena"
);

static bool ranges_overlap(
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

static bool pointer_aligned(const void *pointer, size_t alignment)
{
    return pointer != NULL && alignment != 0U &&
        ((uintptr_t)pointer % alignment) == 0U;
}

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t *out_value)
{
    if (out_value == NULL || lhs > UINT64_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static uint32_t read_be_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
        ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
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

static bool desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
)
{
    return lhs->pixel_format == rhs->pixel_format &&
        lhs->codec == rhs->codec && lhs->width == rhs->width &&
        lhs->height == rhs->height &&
        lhs->fps_millihz == rhs->fps_millihz &&
        lhs->bitrate_bps == rhs->bitrate_bps &&
        lhs->gop_frames == rhs->gop_frames &&
        lhs->recovery_flags == rhs->recovery_flags;
}

static bool binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
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

static bool open_equal(
    const MeisoCompositionMediaOpen *lhs,
    const MeisoCompositionMediaOpen *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation;
}

static bool packet_ref_equal(
    const MeisoCompositionMediaPacketRef *lhs,
    const MeisoCompositionMediaPacketRef *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        lhs->serial == rhs->serial && lhs->kind == rhs->kind &&
        lhs->source == rhs->source &&
        lhs->frame_sequence == rhs->frame_sequence &&
        lhs->fragment_offset == rhs->fragment_offset &&
        lhs->packet_bytes == rhs->packet_bytes;
}

static bool frame_ref_equal(
    const MeisoCompositionMediaFrameRef *lhs,
    const MeisoCompositionMediaFrameRef *rhs
)
{
    return lhs->frame_sequence == rhs->frame_sequence &&
        binding_equal(&lhs->binding, &rhs->binding);
}

static bool metadata_equal(
    const MediaFrameMetadata *lhs,
    const MediaFrameMetadata *rhs
)
{
    return binding_equal(&lhs->binding, &rhs->binding) &&
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

static bool terminal_reason_valid(MeisoCompositionMediaTerminalReason reason)
{
    return reason >= MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE &&
        reason <= MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED;
}

static bool handle_valid(const MeisoCompositionMediaTransport *transport)
{
    return transport != NULL && transport->magic == MEDIA_TRANSPORT_MAGIC &&
        transport->arena_begin != NULL &&
        transport->required_bytes >=
            MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES &&
        transport->arena_extent >= transport->required_bytes;
}

static bool overlaps_transport(
    const MeisoCompositionMediaTransport *transport,
    const void *data,
    size_t size
)
{
    return handle_valid(transport) &&
        ranges_overlap(
            transport->arena_begin, transport->arena_extent, data, size
        );
}

static uint32_t selected_access_unit_bytes(
    const MeisoCompositionMediaDesired *desired
)
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

static bool scene_fields_valid(const MeisoCompositionMediaUnit *unit)
{
    if (unit->binding.source ==
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
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

static MeisoCompositionMediaTransportStatus binding_difference(
    const MeisoCompositionMediaBinding *expected,
    const MeisoCompositionMediaBinding *actual
)
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

static int route_index_for_source(MeisoCompositionMediaSource source)
{
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return 0;
    }
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return 1;
    }
    return -1;
}

static bool route_config_valid(
    const MeisoCompositionMediaOpen *scope,
    const MeisoCompositionMediaRouteConfig *route,
    size_t route_index
)
{
    uint8_t capability_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    uint8_t binding_wire[MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE];
    const MeisoEndpointId *expected_endpoint = route_index == 0U ?
        meiso_composition_render_media_endpoint() :
        meiso_composition_camera_media_endpoint();
    const MeisoResourceDescriptor *resources;
    const MeisoCompositionMediaSource expected_source = route_index == 0U ?
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    size_t resource_count = 0U;
    size_t wire_size = 0U;

    resources = route_index == 0U ?
        meiso_composition_render_media_resources(&resource_count) :
        meiso_composition_camera_media_resources(&resource_count);
    if (resources == NULL || resource_count != 3U ||
        expected_endpoint == NULL || route->binding.source != expected_source ||
        route->capability.source != expected_source ||
        !open_equal(
            scope,
            &(MeisoCompositionMediaOpen){
                route->binding.session_id,
                route->binding.catalog_generation
            }
        ) ||
        !endpoint_equal(route->binding.endpoint_id, *expected_endpoint) ||
        !resource_equal(route->binding.resource_id, resources[2].id) ||
        route->binding.endpoint_incarnation == 0U ||
        route->binding.production_epoch == 0U ||
        route->binding.media_stream_epoch == 0U ||
        !meiso_session_nonce_valid(route->binding.channel_nonce) ||
        !meiso_composition_media_desired_supported(
            &route->capability, &route->effective
        )) {
        return false;
    }
    if (meiso_composition_media_capability_encode(
            &route->capability,
            (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)},
            &wire_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        wire_size != sizeof(capability_wire) ||
        meiso_composition_media_binding_encode(
            &route->binding,
            (MeisoMutableBuffer){binding_wire, sizeof(binding_wire)},
            &wire_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        wire_size != sizeof(binding_wire)) {
        return false;
    }
    return selected_access_unit_bytes(&route->effective) != 0U &&
        selected_access_unit_bytes(&route->effective) <=
            route->capability.max_access_unit_bytes;
}

static bool config_valid(const MeisoCompositionMediaTransportConfig *config)
{
    uint8_t open_wire[MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE];
    size_t wire_size = 0U;
    if (config == NULL ||
        (config->local_role != MEISO_SESSION_ROLE_HOST &&
         config->local_role != MEISO_SESSION_ROLE_EDGE) ||
        meiso_composition_media_open_encode(
            &config->scope,
            (MeisoMutableBuffer){open_wire, sizeof(open_wire)}, &wire_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        wire_size != sizeof(open_wire)) {
        return false;
    }
    return route_config_valid(&config->scope, &config->routes[0], 0U) &&
        route_config_valid(&config->scope, &config->routes[1], 1U);
}

static bool calculate_required(
    const MeisoCompositionMediaTransportConfig *config,
    size_t *out_required,
    uint32_t *out_packet_capacity
)
{
    size_t required = MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES;
    size_t payload_bytes = 0U;
    uint32_t max_fragment = 0U;
    size_t i;
    if (!config_valid(config)) {
        return false;
    }
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        const uint32_t capacity =
            config->routes[i].capability.max_access_unit_bytes;
        if (!checked_add_size(payload_bytes, capacity, &payload_bytes) ||
            !checked_add_size(payload_bytes, capacity, &payload_bytes)) {
            return false;
        }
        if (config->routes[i].capability.max_fragment_payload_bytes >
            max_fragment) {
            max_fragment =
                config->routes[i].capability.max_fragment_payload_bytes;
        }
    }
    if (!checked_add_size(required, payload_bytes, &required) ||
        !checked_add_size(
            required, MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE,
            &required
        ) ||
        !checked_add_size(required, max_fragment, &required)) {
        return false;
    }
    *out_required = required;
    *out_packet_capacity =
        MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + max_fragment;
    return true;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_memory_required(
    const MeisoCompositionMediaTransportConfig *config,
    size_t *out_size,
    size_t *out_alignment
)
{
    size_t required = 0U;
    uint32_t packet_capacity = 0U;
    if (config == NULL || out_size == NULL || out_alignment == NULL ||
        ranges_overlap(config, sizeof(*config), out_size, sizeof(*out_size)) ||
        ranges_overlap(
            config, sizeof(*config), out_alignment, sizeof(*out_alignment)
        ) ||
        ranges_overlap(
            out_size, sizeof(*out_size), out_alignment,
            sizeof(*out_alignment)
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!calculate_required(config, &required, &packet_capacity)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG;
    }
    (void)packet_capacity;
    *out_size = required;
    *out_alignment = alignof(max_align_t);
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus meiso_composition_media_transport_init(
    const MeisoCompositionMediaTransportConfig *config,
    MeisoMutableBuffer arena,
    MeisoCompositionMediaTransport **out_transport
)
{
    MeisoCompositionMediaTransport value;
    MeisoCompositionMediaTransport *transport;
    size_t required = 0U;
    uint32_t packet_capacity = 0U;
    size_t offset = MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES;
    size_t i;
    if (config == NULL || arena.data == NULL || out_transport == NULL ||
        ranges_overlap(config, sizeof(*config), arena.data, arena.capacity) ||
        ranges_overlap(
            out_transport, sizeof(*out_transport), arena.data, arena.capacity
        ) ||
        ranges_overlap(
            config, sizeof(*config), out_transport, sizeof(*out_transport)
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!calculate_required(config, &required, &packet_capacity)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG;
    }
    if (!pointer_aligned(arena.data, alignof(max_align_t)) ||
        arena.capacity < required) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY;
    }

    memset(&value, 0, sizeof(value));
    value.magic = MEDIA_TRANSPORT_MAGIC;
    value.local_role = config->local_role;
    value.phase = config->local_role == MEISO_SESSION_ROLE_HOST ?
        MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING :
        MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN;
    value.scope = config->scope;
    value.arena_begin = arena.data;
    value.arena_extent = arena.capacity;
    value.required_bytes = required;
    value.packet_scratch_capacity = packet_capacity;
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        const uint32_t capacity =
            config->routes[i].capability.max_access_unit_bytes;
        value.routes[i].config = config->routes[i];
        value.routes[i].primary.bytes = arena.data + offset;
        value.routes[i].primary.capacity = capacity;
        offset += capacity;
        value.routes[i].latest.bytes = arena.data + offset;
        value.routes[i].latest.capacity = capacity;
        offset += capacity;
    }
    value.packet_scratch = arena.data + offset;
    offset += packet_capacity;
    if (offset != required) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG;
    }

    memset(arena.data, 0, required);
    transport = (MeisoCompositionMediaTransport *)arena.data;
    *transport = value;
    *out_transport = transport;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static void metadata_from_unit(
    const MeisoCompositionMediaUnit *unit,
    uint64_t effective_drops,
    MediaFrameMetadata *out_metadata
)
{
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->binding = unit->binding;
    out_metadata->frame_sequence = unit->frame_sequence;
    out_metadata->source_time = unit->source_time;
    out_metadata->applied_scene_owner = unit->applied_scene_owner;
    out_metadata->applied_scene_state_version =
        unit->applied_scene_state_version;
    out_metadata->active_plan_generation = unit->active_plan_generation;
    out_metadata->world_generation = unit->world_generation;
    out_metadata->dependency_sequence = unit->dependency_sequence;
    out_metadata->producer_dropped_frames = effective_drops;
    out_metadata->pixel_format = unit->pixel_format;
    out_metadata->codec = unit->codec;
    out_metadata->width = unit->width;
    out_metadata->height = unit->height;
    out_metadata->flags = unit->flags;
    out_metadata->access_unit_bytes = (uint32_t)unit->access_unit.size;
}

static void metadata_from_frame(
    const MeisoCompositionMediaFrame *frame,
    MediaFrameMetadata *out_metadata
)
{
    memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->binding = frame->binding;
    out_metadata->frame_sequence = frame->frame_sequence;
    out_metadata->source_time = frame->source_time;
    out_metadata->applied_scene_owner = frame->applied_scene_owner;
    out_metadata->applied_scene_state_version =
        frame->applied_scene_state_version;
    out_metadata->active_plan_generation = frame->active_plan_generation;
    out_metadata->world_generation = frame->world_generation;
    out_metadata->dependency_sequence = frame->dependency_sequence;
    out_metadata->producer_dropped_frames = frame->producer_dropped_frames;
    out_metadata->pixel_format = frame->pixel_format;
    out_metadata->codec = frame->codec;
    out_metadata->width = frame->width;
    out_metadata->height = frame->height;
    out_metadata->flags = frame->flags &
        (MEISO_COMPOSITION_MEDIA_FRAME_KEY |
         MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY);
    out_metadata->access_unit_bytes = frame->access_unit_bytes;
}

static void unit_from_slot(
    const MediaFrameSlot *slot,
    MeisoCompositionMediaUnit *out_unit
)
{
    const MediaFrameMetadata *metadata = &slot->metadata;
    memset(out_unit, 0, sizeof(*out_unit));
    out_unit->binding = metadata->binding;
    out_unit->frame_sequence = metadata->frame_sequence;
    out_unit->source_time = metadata->source_time;
    out_unit->applied_scene_owner = metadata->applied_scene_owner;
    out_unit->applied_scene_state_version =
        metadata->applied_scene_state_version;
    out_unit->active_plan_generation = metadata->active_plan_generation;
    out_unit->world_generation = metadata->world_generation;
    out_unit->dependency_sequence = metadata->dependency_sequence;
    out_unit->producer_dropped_frames = metadata->producer_dropped_frames;
    out_unit->pixel_format = metadata->pixel_format;
    out_unit->codec = metadata->codec;
    out_unit->width = metadata->width;
    out_unit->height = metadata->height;
    out_unit->flags = metadata->flags;
    out_unit->access_unit.data = slot->bytes;
    out_unit->access_unit.size = metadata->access_unit_bytes;
}

static void frame_from_primary(
    const MediaRouteState *route,
    uint32_t fragment_size,
    MeisoCompositionMediaFrame *out_frame
)
{
    const MediaFrameMetadata *metadata = &route->primary.metadata;
    const uint32_t offset = route->primary_offset;
    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->binding = metadata->binding;
    out_frame->frame_sequence = metadata->frame_sequence;
    out_frame->source_time = metadata->source_time;
    out_frame->applied_scene_owner = metadata->applied_scene_owner;
    out_frame->applied_scene_state_version =
        metadata->applied_scene_state_version;
    out_frame->active_plan_generation = metadata->active_plan_generation;
    out_frame->world_generation = metadata->world_generation;
    out_frame->dependency_sequence = metadata->dependency_sequence;
    out_frame->producer_dropped_frames = metadata->producer_dropped_frames;
    out_frame->pixel_format = metadata->pixel_format;
    out_frame->codec = metadata->codec;
    out_frame->width = metadata->width;
    out_frame->height = metadata->height;
    out_frame->access_unit_bytes = metadata->access_unit_bytes;
    out_frame->fragment_offset = offset;
    out_frame->fragment.data = route->primary.bytes + offset;
    out_frame->fragment.size = fragment_size;
    out_frame->flags = metadata->flags;
    if (offset == 0U) {
        out_frame->flags |= MEISO_COMPOSITION_MEDIA_FRAME_BEGIN;
    }
    if (offset + fragment_size == metadata->access_unit_bytes) {
        out_frame->flags |= MEISO_COMPOSITION_MEDIA_FRAME_END;
    }
}

static MeisoCompositionMediaTransportStatus cache_outbound_packet(
    MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaPacketKind kind,
    MeisoCompositionMediaSource source,
    uint64_t frame_sequence,
    uint32_t fragment_offset,
    size_t packet_size,
    MeisoCompositionMediaPacketView *out_packet
)
{
    MeisoCompositionMediaPacketRef ref;
    MeisoCompositionMediaPacketView view;
    uint64_t serial;
    if (!checked_add_u64(
            transport->outbound_packet_serial_high_water, 1U, &serial
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (packet_size > UINT32_MAX) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY;
    }
    memset(&ref, 0, sizeof(ref));
    ref.session_id = transport->scope.session_id;
    ref.catalog_generation = transport->scope.catalog_generation;
    ref.serial = serial;
    ref.kind = kind;
    ref.source = source;
    ref.frame_sequence = frame_sequence;
    ref.fragment_offset = fragment_offset;
    ref.packet_bytes = (uint32_t)packet_size;
    view.ref = ref;
    view.bytes.data = transport->packet_scratch;
    view.bytes.size = packet_size;

    transport->outbound_packet_ref = ref;
    transport->outbound_packet_size = packet_size;
    transport->outbound_packet_serial_high_water = serial;
    transport->outbound_packet_pending = true;
    *out_packet = view;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_next_outbound(
    MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaPacketView *out_packet
)
{
    MeisoCompositionSessionV3Status codec_status;
    size_t packet_size = 0U;
    size_t scan;
    if (!handle_valid(transport) || out_packet == NULL ||
        overlaps_transport(transport, out_packet, sizeof(*out_packet))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->outbound_packet_pending) {
        MeisoCompositionMediaPacketView view;
        view.ref = transport->outbound_packet_ref;
        view.bytes.data = transport->packet_scratch;
        view.bytes.size = transport->outbound_packet_size;
        *out_packet = view;
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
    }
    if (transport->outbound_packet_serial_high_water == UINT64_MAX) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }

    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING) {
        codec_status = meiso_composition_media_open_encode(
            &transport->scope,
            (MeisoMutableBuffer){
                transport->packet_scratch,
                transport->packet_scratch_capacity
            },
            &packet_size
        );
        if (codec_status != MEISO_COMPOSITION_SESSION_V3_OK) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        return cache_outbound_packet(
            transport, MEISO_COMPOSITION_MEDIA_PACKET_OPEN, 0U, 0U, 0U,
            packet_size, out_packet
        );
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING) {
        const MediaRouteState *route;
        if (transport->bind_send_index >=
            MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        route = &transport->routes[transport->bind_send_index];
        codec_status = meiso_composition_media_binding_encode(
            &route->config.binding,
            (MeisoMutableBuffer){
                transport->packet_scratch,
                transport->packet_scratch_capacity
            },
            &packet_size
        );
        if (codec_status != MEISO_COMPOSITION_SESSION_V3_OK) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        return cache_outbound_packet(
            transport, MEISO_COMPOSITION_MEDIA_PACKET_BIND,
            route->config.binding.source, 0U, 0U, packet_size, out_packet
        );
    }
    if (transport->phase !=
        MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET;
    }

    for (scan = 0U; scan < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT;
         ++scan) {
        const size_t route_index =
            (transport->fragment_rr_cursor + scan) %
            MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT;
        MediaRouteState *route = &transport->routes[route_index];
        MeisoCompositionMediaFrame frame;
        uint32_t remaining;
        uint32_t fragment_size;
        if (!route->primary.occupied) {
            continue;
        }
        if (route->primary_offset >=
            route->primary.metadata.access_unit_bytes) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        remaining = route->primary.metadata.access_unit_bytes -
            route->primary_offset;
        fragment_size = remaining <
                route->config.capability.max_fragment_payload_bytes ?
            remaining : route->config.capability.max_fragment_payload_bytes;
        frame_from_primary(route, fragment_size, &frame);
        codec_status = meiso_composition_media_frame_encode(
            &frame,
            (MeisoMutableBuffer){
                transport->packet_scratch,
                transport->packet_scratch_capacity
            },
            &packet_size
        );
        if (codec_status != MEISO_COMPOSITION_SESSION_V3_OK) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        return cache_outbound_packet(
            transport, MEISO_COMPOSITION_MEDIA_PACKET_FRAME,
            route->config.binding.source,
            route->primary.metadata.frame_sequence,
            route->primary_offset, packet_size, out_packet
        );
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET;
}

static void promote_latest_to_primary(MediaRouteState *route)
{
    MediaFrameSlot temporary = route->primary;
    route->primary.occupied = false;
    route->primary_offset = 0U;
    if (route->latest.occupied) {
        route->primary = route->latest;
        route->latest = temporary;
        route->latest.occupied = false;
    }
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_complete_outbound(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaPacketRef *expected_packet
)
{
    if (!handle_valid(transport) || expected_packet == NULL ||
        overlaps_transport(
            transport, expected_packet, sizeof(*expected_packet)
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (!transport->outbound_packet_pending) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!packet_ref_equal(
            expected_packet, &transport->outbound_packet_ref
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
    }

    if (expected_packet->kind == MEISO_COMPOSITION_MEDIA_PACKET_OPEN) {
        if (transport->phase !=
            MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        transport->phase =
            MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_WAIT_BIND;
    } else if (expected_packet->kind ==
               MEISO_COMPOSITION_MEDIA_PACKET_BIND) {
        if (transport->phase !=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING ||
            transport->bind_send_index >=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT ||
            transport->routes[transport->bind_send_index]
                    .config.binding.source != expected_packet->source) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        ++transport->bind_send_index;
        if (transport->bind_send_index ==
            MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT) {
            transport->phase =
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE;
            transport->bound_route_mask = UINT8_C(0x03);
        }
    } else if (expected_packet->kind ==
               MEISO_COMPOSITION_MEDIA_PACKET_FRAME) {
        const int route_index = route_index_for_source(expected_packet->source);
        MediaRouteState *route;
        const uint32_t fragment_size = expected_packet->packet_bytes -
            MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
        uint32_t next_offset;
        if (transport->phase !=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE ||
            route_index < 0 ||
            expected_packet->packet_bytes <=
                MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        route = &transport->routes[(size_t)route_index];
        if (!route->primary.occupied ||
            route->primary.metadata.frame_sequence !=
                expected_packet->frame_sequence ||
            route->primary_offset != expected_packet->fragment_offset ||
            fragment_size > route->primary.metadata.access_unit_bytes -
                route->primary_offset) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
        }
        next_offset = route->primary_offset + fragment_size;
        route->primary_offset = next_offset;
        if (next_offset == route->primary.metadata.access_unit_bytes) {
            promote_latest_to_primary(route);
        }
        transport->fragment_rr_cursor = (uint8_t)
            (((size_t)route_index + 1U) %
             MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT);
    } else {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    transport->outbound_packet_pending = false;
    transport->outbound_packet_size = 0U;
    memset(
        &transport->outbound_packet_ref, 0,
        sizeof(transport->outbound_packet_ref)
    );
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_submit(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaUnit *unit
)
{
    const int route_index = unit != NULL ?
        route_index_for_source(unit->binding.source) : -1;
    MediaRouteState *route;
    MediaFrameSlot *destination;
    MediaFrameMetadata metadata;
    MeisoCompositionMediaTransportStatus binding_status;
    uint64_t new_transport_drops;
    bool replace_latest;
    if (!handle_valid(transport) || unit == NULL ||
        overlaps_transport(transport, unit, sizeof(*unit)) ||
        unit->access_unit.data == NULL ||
        overlaps_transport(
            transport, unit->access_unit.data, unit->access_unit.size
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE ||
        route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    binding_status = binding_difference(&route->config.binding, &unit->binding);
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if (!desired_equal(
            &route->config.effective,
            &(MeisoCompositionMediaDesired){
                unit->pixel_format,
                unit->codec,
                unit->width,
                unit->height,
                route->config.effective.fps_millihz,
                route->config.effective.bitrate_bps,
                route->config.effective.gop_frames,
                route->config.effective.recovery_flags
            }
        ) ||
        unit->frame_sequence == 0U ||
        !meiso_time_point_valid(unit->source_time) ||
        !scene_fields_valid(unit) || unit->dependency_sequence != 0U ||
        (unit->flags & MEISO_COMPOSITION_MEDIA_FRAME_KEY) == 0U ||
        (unit->flags &
         (MeisoCompositionMediaFrameFlags)~
             (MEISO_COMPOSITION_MEDIA_FRAME_KEY |
              MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U ||
        unit->access_unit.size !=
            selected_access_unit_bytes(&route->config.effective) ||
        unit->access_unit.size > route->primary.capacity) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (unit->frame_sequence <= route->submitted_sequence_high_water ||
        unit->producer_dropped_frames <
            route->submitted_producer_drop_high_water) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }

    replace_latest = route->primary.occupied && route->latest.occupied;
    new_transport_drops = route->transport_whole_frames_dropped;
    if (replace_latest &&
        !checked_add_u64(new_transport_drops, 1U, &new_transport_drops)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    metadata_from_unit(unit, unit->producer_dropped_frames, &metadata);
    destination = !route->primary.occupied ?
        &route->primary : &route->latest;
    memmove(
        destination->bytes, unit->access_unit.data, unit->access_unit.size
    );
    destination->metadata = metadata;
    destination->occupied = true;
    if (destination == &route->primary) {
        route->primary_offset = 0U;
    }
    route->submitted_sequence_high_water = unit->frame_sequence;
    route->submitted_producer_drop_high_water =
        unit->producer_dropped_frames;
    route->transport_whole_frames_dropped = new_transport_drops;
    return replace_latest ?
        MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED :
        MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static MeisoCompositionMediaTransportStatus receive_open(
    MeisoCompositionMediaTransport *transport,
    MeisoByteView packet
)
{
    MeisoCompositionMediaOpen open;
    MeisoCompositionSessionV3Status status =
        meiso_composition_media_open_decode(packet, &open);
    if (status == MEISO_COMPOSITION_SESSION_V3_TRUNCATED) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_EDGE ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    if (!open_equal(&transport->scope, &open)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static MeisoCompositionMediaTransportStatus receive_bind(
    MeisoCompositionMediaTransport *transport,
    MeisoByteView packet
)
{
    MeisoCompositionMediaBinding binding;
    MeisoCompositionSessionV3Status codec_status =
        meiso_composition_media_binding_decode(packet, &binding);
    MeisoCompositionMediaTransportStatus binding_status;
    const int route_index = codec_status == MEISO_COMPOSITION_SESSION_V3_OK ?
        route_index_for_source(binding.source) : -1;
    uint8_t route_bit;
    if (codec_status == MEISO_COMPOSITION_SESSION_V3_TRUNCATED) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (codec_status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_WAIT_BIND ||
        route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    binding_status = binding_difference(
        &transport->routes[(size_t)route_index].config.binding, &binding
    );
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    route_bit = (uint8_t)(UINT8_C(1) << (uint8_t)route_index);
    if ((transport->bound_route_mask & route_bit) != 0U) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
    }
    transport->bound_route_mask |= route_bit;
    if (transport->bound_route_mask == UINT8_C(0x03)) {
        transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE;
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

static bool precompute_publication(
    const MediaRouteState *route,
    const MediaFrameMetadata *metadata,
    uint64_t *out_sequence_gaps,
    uint64_t *out_latest_drops
)
{
    uint64_t gap;
    if (metadata->frame_sequence <= route->published_sequence_high_water ||
        metadata->producer_dropped_frames < route->producer_drop_high_water) {
        return false;
    }
    gap = route->published_sequence_high_water == 0U ?
        metadata->frame_sequence - 1U :
        metadata->frame_sequence - route->published_sequence_high_water - 1U;
    return checked_add_u64(
            route->sequence_gap_frames, gap, out_sequence_gaps
        ) &&
        checked_add_u64(
            route->unread_latest_frames_dropped,
            route->latest.occupied ? 1U : 0U, out_latest_drops
        );
}

static void publish_primary(
    MediaRouteState *route,
    uint64_t sequence_gaps,
    uint64_t latest_drops
)
{
    MediaFrameSlot temporary = route->latest;
    const uint64_t sequence = route->primary.metadata.frame_sequence;
    const uint64_t producer_drops =
        route->primary.metadata.producer_dropped_frames;
    route->latest = route->primary;
    route->primary = temporary;
    route->primary.occupied = false;
    route->primary_offset = 0U;
    route->published_sequence_high_water = sequence;
    route->producer_drop_high_water = producer_drops;
    route->sequence_gap_frames = sequence_gaps;
    route->unread_latest_frames_dropped = latest_drops;
}

static MeisoCompositionMediaTransportStatus receive_frame(
    MeisoCompositionMediaTransport *transport,
    MeisoByteView packet
)
{
    MeisoCompositionMediaFrame frame;
    MediaFrameMetadata metadata;
    MeisoCompositionSessionV3Status codec_status =
        meiso_composition_media_frame_decode(packet, &frame);
    MeisoCompositionMediaTransportStatus binding_status;
    MediaRouteState *route;
    int route_index;
    uint32_t end_offset;
    bool finish;
    uint64_t sequence_gaps = 0U;
    uint64_t latest_drops = 0U;
    uint64_t reassembly_drops = 0U;
    if (codec_status == MEISO_COMPOSITION_SESSION_V3_TRUNCATED) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    if (codec_status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST ||
        transport->phase != MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route_index = route_index_for_source(frame.binding.source);
    if (route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    route = &transport->routes[(size_t)route_index];
    binding_status = binding_difference(&route->config.binding, &frame.binding);
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if (frame.pixel_format != route->config.effective.pixel_format ||
        frame.codec != route->config.effective.codec ||
        frame.width != route->config.effective.width ||
        frame.height != route->config.effective.height ||
        frame.access_unit_bytes !=
            selected_access_unit_bytes(&route->config.effective) ||
        frame.access_unit_bytes > route->primary.capacity ||
        frame.fragment.size >
            route->config.capability.max_fragment_payload_bytes) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
    }
    metadata_from_frame(&frame, &metadata);
    end_offset = frame.fragment_offset + (uint32_t)frame.fragment.size;
    finish = end_offset == frame.access_unit_bytes;

    if (!route->primary.occupied) {
        if (frame.fragment_offset != 0U) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY;
        }
        if (frame.frame_sequence <= route->published_sequence_high_water ||
            frame.producer_dropped_frames < route->producer_drop_high_water) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
        }
        if (finish && !precompute_publication(
                route, &metadata, &sequence_gaps, &latest_drops
            )) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
        memmove(route->primary.bytes, frame.fragment.data, frame.fragment.size);
        route->primary.metadata = metadata;
        route->primary.occupied = true;
        route->primary_offset = end_offset;
        if (finish) {
            publish_primary(route, sequence_gaps, latest_drops);
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED;
        }
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED;
    }

    if (frame.frame_sequence < route->primary.metadata.frame_sequence ||
        frame.frame_sequence <= route->published_sequence_high_water) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }
    if (frame.frame_sequence == route->primary.metadata.frame_sequence) {
        if (!metadata_equal(&route->primary.metadata, &metadata)) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
        }
        if (frame.fragment_offset != route->primary_offset) {
            if (!checked_add_u64(
                    route->reassembly_frames_dropped, 1U,
                    &reassembly_drops
                )) {
                return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
            }
            route->primary.occupied = false;
            route->primary_offset = 0U;
            route->reassembly_frames_dropped = reassembly_drops;
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY;
        }
        if (finish && !precompute_publication(
                route, &metadata, &sequence_gaps, &latest_drops
            )) {
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
        }
        memmove(
            route->primary.bytes + frame.fragment_offset,
            frame.fragment.data, frame.fragment.size
        );
        route->primary_offset = end_offset;
        if (finish) {
            publish_primary(route, sequence_gaps, latest_drops);
            return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED;
        }
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED;
    }

    if (frame.fragment_offset == 0U &&
        (frame.producer_dropped_frames <
             route->primary.metadata.producer_dropped_frames ||
         frame.producer_dropped_frames < route->producer_drop_high_water)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY;
    }
    if (!checked_add_u64(
            route->reassembly_frames_dropped, 1U, &reassembly_drops
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    if (frame.fragment_offset != 0U) {
        route->primary.occupied = false;
        route->primary_offset = 0U;
        route->reassembly_frames_dropped = reassembly_drops;
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY;
    }
    if (finish && !precompute_publication(
            route, &metadata, &sequence_gaps, &latest_drops
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED;
    }
    memmove(route->primary.bytes, frame.fragment.data, frame.fragment.size);
    route->primary.metadata = metadata;
    route->primary.occupied = true;
    route->primary_offset = end_offset;
    route->reassembly_frames_dropped = reassembly_drops;
    if (finish) {
        publish_primary(route, sequence_gaps, latest_drops);
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED;
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_REASSEMBLY_REPLACED;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_receive_packet(
    MeisoCompositionMediaTransport *transport,
    MeisoByteView packet
)
{
    uint32_t magic;
    if (!handle_valid(transport) || packet.data == NULL ||
        overlaps_transport(transport, packet.data, packet.size)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (packet.size < 4U) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED;
    }
    magic = read_be_u32(packet.data);
    if (magic == MEDIA_OPEN_MAGIC) {
        return receive_open(transport, packet);
    }
    if (magic == MEDIA_BIND_MAGIC) {
        return receive_bind(transport, packet);
    }
    if (magic == MEDIA_FRAME_MAGIC) {
        return receive_frame(transport, packet);
    }
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_latest(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest
)
{
    MeisoCompositionMediaLatestView view;
    const int route_index = route_index_for_source(source);
    const MediaRouteState *route;
    if (!handle_valid(transport) || out_latest == NULL ||
        overlaps_transport(transport, out_latest, sizeof(*out_latest))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST || route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    if (!route->latest.occupied) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME;
    }
    memset(&view, 0, sizeof(view));
    view.ref.binding = route->latest.metadata.binding;
    view.ref.frame_sequence = route->latest.metadata.frame_sequence;
    unit_from_slot(&route->latest, &view.unit);
    *out_latest = view;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_release_latest(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaFrameRef *expected_frame
)
{
    const int route_index = expected_frame != NULL ?
        route_index_for_source(expected_frame->binding.source) : -1;
    MediaRouteState *route;
    MeisoCompositionMediaFrameRef current;
    MeisoCompositionMediaTransportStatus binding_status;
    if (!handle_valid(transport) || expected_frame == NULL ||
        overlaps_transport(
            transport, expected_frame, sizeof(*expected_frame)
        )) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (transport->local_role != MEISO_SESSION_ROLE_HOST || route_index < 0) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE;
    }
    route = &transport->routes[(size_t)route_index];
    binding_status = binding_difference(
        &route->config.binding, &expected_frame->binding
    );
    if (binding_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return binding_status;
    }
    if (!route->latest.occupied) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME;
    }
    current.binding = route->latest.metadata.binding;
    current.frame_sequence = route->latest.metadata.frame_sequence;
    if (!frame_ref_equal(expected_frame, &current)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT;
    }
    route->latest.occupied = false;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_terminate(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaOpen *expected_scope,
    MeisoCompositionMediaTerminalReason reason
)
{
    size_t i;
    if (!handle_valid(transport) || expected_scope == NULL ||
        overlaps_transport(transport, expected_scope, sizeof(*expected_scope))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (!terminal_reason_valid(reason)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    if (transport->phase ==
        MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL;
    }
    if (!open_equal(&transport->scope, expected_scope)) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH;
    }
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        MediaRouteState *route = &transport->routes[i];
        memset(route->primary.bytes, 0, route->primary.capacity);
        memset(route->latest.bytes, 0, route->latest.capacity);
        route->primary.occupied = false;
        route->latest.occupied = false;
        route->primary_offset = 0U;
    }
    memset(
        transport->packet_scratch, 0, transport->packet_scratch_capacity
    );
    transport->outbound_packet_pending = false;
    transport->outbound_packet_size = 0U;
    memset(
        &transport->outbound_packet_ref, 0,
        sizeof(transport->outbound_packet_ref)
    );
    transport->bound_route_mask = 0U;
    transport->phase = MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL;
    transport->terminal_reason = reason;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}

MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_snapshot(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaTransportSnapshot *out_snapshot
)
{
    MeisoCompositionMediaTransportSnapshot snapshot;
    size_t i;
    if (!handle_valid(transport) || out_snapshot == NULL ||
        overlaps_transport(transport, out_snapshot, sizeof(*out_snapshot))) {
        return MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.local_role = transport->local_role;
    snapshot.scope = transport->scope;
    snapshot.phase = transport->phase;
    snapshot.terminal_reason = transport->terminal_reason;
    snapshot.bound_route_mask = transport->bound_route_mask;
    snapshot.fragment_rr_cursor = transport->fragment_rr_cursor;
    snapshot.outbound_packet_pending = transport->outbound_packet_pending;
    snapshot.outbound_packet_kind = transport->outbound_packet_pending ?
        transport->outbound_packet_ref.kind : 0U;
    snapshot.outbound_packet_serial_high_water =
        transport->outbound_packet_serial_high_water;
    snapshot.arena_bytes = transport->required_bytes;
    for (i = 0U; i < MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT; ++i) {
        const MediaRouteState *route = &transport->routes[i];
        MeisoCompositionMediaRouteSnapshot *out_route = &snapshot.routes[i];
        out_route->source = route->config.binding.source;
        out_route->send_in_flight =
            transport->local_role == MEISO_SESSION_ROLE_EDGE &&
            route->primary.occupied;
        out_route->send_latest =
            transport->local_role == MEISO_SESSION_ROLE_EDGE &&
            route->latest.occupied;
        out_route->send_frame_sequence = out_route->send_in_flight ?
            route->primary.metadata.frame_sequence : 0U;
        out_route->send_fragment_offset = out_route->send_in_flight ?
            route->primary_offset : 0U;
        out_route->submitted_sequence_high_water =
            route->submitted_sequence_high_water;
        out_route->transport_whole_frames_dropped =
            route->transport_whole_frames_dropped;
        out_route->receive_reassembly =
            transport->local_role == MEISO_SESSION_ROLE_HOST &&
            route->primary.occupied;
        out_route->receive_latest =
            transport->local_role == MEISO_SESSION_ROLE_HOST &&
            route->latest.occupied;
        out_route->receive_frame_sequence = out_route->receive_reassembly ?
            route->primary.metadata.frame_sequence : 0U;
        out_route->receive_next_offset = out_route->receive_reassembly ?
            route->primary_offset : 0U;
        out_route->published_sequence_high_water =
            route->published_sequence_high_water;
        out_route->sequence_gap_frames = route->sequence_gap_frames;
        out_route->producer_drop_high_water = route->producer_drop_high_water;
        out_route->reassembly_frames_dropped =
            route->reassembly_frames_dropped;
        out_route->unread_latest_frames_dropped =
            route->unread_latest_frames_dropped;
    }
    *out_snapshot = snapshot;
    return MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;
}
