#include "meiso_composition_edge_trace.h"

#include "composition_virtual_edge_host_internal.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define TRACE_WRITER_MAGIC UINT64_C(0x4d45435457525431)
#define TRACE_READER_MAGIC UINT64_C(0x4d45435452445231)
#define TRACE_FLAG_SYNTHETIC UINT32_C(1)
#define TRACE_MIN_BYTES                                                    \
    (MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +                          \
     MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +                  \
     MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES +                         \
     MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +                  \
     MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES + 8U)
#define TRACE_MAX_FRAME_PAYLOAD                                           \
    (MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES +                    \
     MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES)

typedef struct {
    bool pending;
    bool pending_enabled;
    MeisoCompositionMediaDesired pending_desired;
    bool active;
    MeisoCompositionMediaDesired active_desired;
    uint64_t active_epoch;
    uint64_t next_epoch;
    bool epoch_available;
    uint64_t sequence_high_water;
    uint64_t drop_high_water;
} TraceMediaState;

typedef struct {
    MeisoCompositionEdgeSimulatorConfig config;
    bool activated;
    bool terminated;
    bool wifi_pending;
    bool wifi_pending_enabled;
    MeisoWifiTelemetryDesiredParameters wifi_pending_desired;
    uint64_t next_wifi_epoch;
    bool wifi_epoch_available;
    TraceMediaState media[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
} TraceSemanticState;

struct MeisoCompositionEdgeTraceWriter {
    uint64_t magic;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    uint8_t *output;
    size_t capacity;
    size_t size;
    uint32_t record_count;
    bool finalized;
    TraceSemanticState semantic;
};

struct MeisoCompositionEdgeTraceReader {
    uint64_t magic;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t trace_begin;
    uintptr_t trace_end;
    const uint8_t *trace;
    size_t trace_size;
    size_t cursor;
    uint32_t record_count;
    uint32_t next_sequence;
    uintptr_t bound_handle;
    MeisoCompositionEdgeSimulatorConfig config;
};

static bool trace_checked_range(
    const void *pointer,
    size_t size,
    uintptr_t *out_begin,
    uintptr_t *out_end
)
{
    uintptr_t begin;

    if (pointer == NULL || size == 0U || out_begin == NULL || out_end == NULL) {
        return false;
    }
    begin = (uintptr_t)pointer;
    if (begin > UINTPTR_MAX - size) {
        return false;
    }
    *out_begin = begin;
    *out_end = begin + size;
    return true;
}

static bool trace_checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool trace_ranges_overlap(
    uintptr_t lhs_begin,
    uintptr_t lhs_end,
    uintptr_t rhs_begin,
    uintptr_t rhs_end
)
{
    return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

static bool trace_range_disjoint(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    uintptr_t lhs_begin;
    uintptr_t lhs_end;
    uintptr_t rhs_begin;
    uintptr_t rhs_end;

    if ((lhs == NULL && lhs_size == 0U) ||
        (rhs == NULL && rhs_size == 0U)) {
        return true;
    }
    return trace_checked_range(lhs, lhs_size, &lhs_begin, &lhs_end) &&
        trace_checked_range(rhs, rhs_size, &rhs_begin, &rhs_end) &&
        !trace_ranges_overlap(lhs_begin, lhs_end, rhs_begin, rhs_end);
}

static bool trace_bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t i;

    if (bytes == NULL && size != 0U) {
        return false;
    }
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

static uint16_t trace_get_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8U) |
        (uint16_t)bytes[1];
}

static uint32_t trace_get_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24U |
        (uint32_t)bytes[1] << 16U |
        (uint32_t)bytes[2] << 8U |
        (uint32_t)bytes[3];
}

static uint64_t trace_get_u64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    size_t i;

    for (i = 0U; i < 8U; ++i) {
        value = value << 8U | bytes[i];
    }
    return value;
}

static void trace_put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void trace_put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void trace_put_u64(uint8_t *bytes, uint64_t value)
{
    size_t i;

    for (i = 0U; i < 8U; ++i) {
        bytes[7U - i] = (uint8_t)value;
        value >>= 8U;
    }
}

static uint32_t trace_crc32c_update(
    uint32_t crc,
    const uint8_t *bytes,
    size_t size
)
{
    size_t i;
    unsigned bit;

    for (i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                ((crc & 1U) != 0U ? UINT32_C(0x82f63b78) : 0U);
        }
    }
    return crc;
}

static uint32_t trace_crc32c(const uint8_t *bytes, size_t size)
{
    if (size == 0U) {
        return 0U;
    }
    return trace_crc32c_update(UINT32_MAX, bytes, size) ^ UINT32_MAX;
}

static uint32_t trace_crc32c_pair(
    const uint8_t *prefix,
    size_t prefix_size,
    const uint8_t *suffix,
    size_t suffix_size
)
{
    uint32_t crc;

    if (prefix_size == 0U && suffix_size == 0U) {
        return 0U;
    }
    crc = trace_crc32c_update(UINT32_MAX, prefix, prefix_size);
    crc = trace_crc32c_update(crc, suffix, suffix_size);
    return crc ^ UINT32_MAX;
}

static bool trace_device_sample_zero(
    const MeisoDeviceTelemetrySample *sample
)
{
    return sample != NULL && sample->selected_fields == 0U &&
        sample->present_fields == 0U && sample->temperature_millic == 0 &&
        sample->temperature_uncertainty_millic == 0U &&
        sample->cpu_load_ppm == 0U && sample->memory_total_bytes == 0U &&
        sample->memory_available_bytes == 0U;
}

static bool trace_config_valid(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    uint8_t dynamic_capability[
        MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES
    ];
    MeisoRuntimeEndpointDescriptor
        endpoints[MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT];
    MeisoCompositionSessionV3RouteDescriptor
        routes[MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT];
    uint8_t sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t sample_size = 0U;

    return config != NULL && config->catalog_generation != 0U &&
        config->continuity_serial != 0U &&
        meiso_owner_scope_valid(config->desired_owner) &&
        config->wifi_production_epoch != 0U &&
        config->render_media_stream_epoch != 0U &&
        config->camera_media_stream_epoch != 0U &&
        !config->device_telemetry_enabled &&
        config->device_production_epoch == 0U &&
        trace_device_sample_zero(&config->device_sample) &&
        config->catalog.render_media_capability.max_access_unit_bytes != 0U &&
        config->catalog.render_media_capability.max_access_unit_bytes <=
            MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES &&
        config->catalog.camera_media_capability.max_access_unit_bytes != 0U &&
        config->catalog.camera_media_capability.max_access_unit_bytes <=
            MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES &&
        meiso_composition_edge_simulator_memory_required(config) != 0U &&
        meiso_composition_session_v3_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){dynamic_capability,
                                 sizeof(dynamic_capability)},
            endpoints, MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
            routes, MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
        ) == MEISO_COMPOSITION_SESSION_V3_OK &&
        meiso_wifi_telemetry_sample_encode(
            &config->wifi_sample,
            (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)},
            &sample_size
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        sample_size == MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE &&
        (config->wifi_sample.selected_fields &
         ~config->catalog.wifi_capability.supported_fields) == 0U;
}

static bool trace_encode_config(
    const MeisoCompositionEdgeSimulatorConfig *config,
    uint8_t output[MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES]
)
{
    size_t encoded_size;

    if (!trace_config_valid(config) || output == NULL) {
        return false;
    }
    memset(output, 0, MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES);
    trace_put_u64(output, config->catalog_generation);
    trace_put_u64(output + 8U, config->continuity_serial);
    output[16U] = config->desired_owner.role;
    trace_put_u64(output + 24U, config->desired_owner.incarnation);
    trace_put_u64(output + 32U, config->wifi_production_epoch);
    trace_put_u64(output + 40U, config->render_media_stream_epoch);
    trace_put_u64(output + 48U, config->camera_media_stream_epoch);
    trace_put_u64(output + 56U, config->catalog.wifi_incarnation);
    trace_put_u64(output + 64U, config->catalog.presentation_incarnation);
    trace_put_u64(output + 72U, config->catalog.scene_incarnation);
    trace_put_u64(output + 80U,
                  config->catalog.device_telemetry_incarnation);
    trace_put_u64(output + 88U, config->catalog.render_media_incarnation);
    trace_put_u64(output + 96U, config->catalog.camera_media_incarnation);
    encoded_size = 0U;
    if (meiso_wifi_telemetry_capability_encode(
            &config->catalog.wifi_capability,
            (MeisoMutableBuffer){output + 104U,
                                 MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE},
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        encoded_size != MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE) {
        return false;
    }
    encoded_size = 0U;
    if (meiso_device_telemetry_capability_encode(
            &config->catalog.device_telemetry_capability,
            (MeisoMutableBuffer){output + 120U,
                                 MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE},
            &encoded_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        encoded_size != MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE) {
        return false;
    }
    encoded_size = 0U;
    if (meiso_composition_media_capability_encode(
            &config->catalog.render_media_capability,
            (MeisoMutableBuffer){output + 168U,
                                 MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE},
            &encoded_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE) {
        return false;
    }
    encoded_size = 0U;
    if (meiso_composition_media_capability_encode(
            &config->catalog.camera_media_capability,
            (MeisoMutableBuffer){output + 232U,
                                 MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE},
            &encoded_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE) {
        return false;
    }
    encoded_size = 0U;
    return meiso_wifi_telemetry_sample_encode(
               &config->wifi_sample,
               (MeisoMutableBuffer){output + 296U,
                                    MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE},
               &encoded_size
           ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        encoded_size == MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE;
}

static bool trace_decode_config(
    const uint8_t *input,
    size_t input_size,
    MeisoCompositionEdgeSimulatorConfig *out_config
)
{
    MeisoCompositionEdgeSimulatorConfig candidate;

    if (input == NULL || out_config == NULL ||
        input_size != MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES ||
        !trace_bytes_zero(input + 17U, 7U)) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.catalog_generation = trace_get_u64(input);
    candidate.continuity_serial = trace_get_u64(input + 8U);
    candidate.desired_owner.role = input[16U];
    candidate.desired_owner.incarnation = trace_get_u64(input + 24U);
    candidate.wifi_production_epoch = trace_get_u64(input + 32U);
    candidate.render_media_stream_epoch = trace_get_u64(input + 40U);
    candidate.camera_media_stream_epoch = trace_get_u64(input + 48U);
    candidate.catalog.wifi_incarnation = trace_get_u64(input + 56U);
    candidate.catalog.presentation_incarnation = trace_get_u64(input + 64U);
    candidate.catalog.scene_incarnation = trace_get_u64(input + 72U);
    candidate.catalog.device_telemetry_incarnation =
        trace_get_u64(input + 80U);
    candidate.catalog.render_media_incarnation =
        trace_get_u64(input + 88U);
    candidate.catalog.camera_media_incarnation =
        trace_get_u64(input + 96U);
    if (meiso_wifi_telemetry_capability_decode(
            (MeisoByteView){input + 104U,
                            MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE},
            &candidate.catalog.wifi_capability
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        meiso_device_telemetry_capability_decode(
            (MeisoByteView){input + 120U,
                            MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE},
            &candidate.catalog.device_telemetry_capability
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        meiso_composition_media_capability_decode(
            (MeisoByteView){input + 168U,
                            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE},
            &candidate.catalog.render_media_capability
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        meiso_composition_media_capability_decode(
            (MeisoByteView){input + 232U,
                            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE},
            &candidate.catalog.camera_media_capability
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        meiso_wifi_telemetry_sample_decode(
            (MeisoByteView){input + 296U,
                            MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE},
            &candidate.wifi_sample
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        !trace_config_valid(&candidate)) {
        return false;
    }
    *out_config = candidate;
    return true;
}

static void trace_semantic_init(
    TraceSemanticState *state,
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->next_wifi_epoch = config->wifi_production_epoch;
    state->wifi_epoch_available = true;
    state->media[0].next_epoch = config->render_media_stream_epoch;
    state->media[1].next_epoch = config->camera_media_stream_epoch;
    state->media[0].epoch_available = true;
    state->media[1].epoch_available = true;
}

static bool trace_frame_size_exact(
    MeisoCompositionMediaPixelFormat pixel_format,
    uint16_t width,
    uint16_t height,
    uint32_t access_unit_bytes
)
{
    uint64_t pixels = (uint64_t)width * height;
    uint64_t bytes;

    switch (pixel_format) {
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888:
        bytes = pixels * 4U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565:
    case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV:
        bytes = pixels * 2U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_NV12:
        if ((width & 1U) != 0U || (height & 1U) != 0U) {
            return false;
        }
        bytes = pixels + pixels / 2U;
        break;
    default:
        return false;
    }
    return bytes == access_unit_bytes;
}

static bool trace_frame_scene_valid(
    MeisoCompositionMediaSource source,
    MeisoOwnerScope owner,
    uint64_t scene_version,
    uint64_t plan_generation,
    uint64_t world_generation,
    uint64_t dependency_sequence
)
{
    if (dependency_sequence != 0U) {
        return false;
    }
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return meiso_owner_scope_valid(owner) &&
            owner.role == MEISO_OWNER_ROLE_HOST && scene_version != 0U &&
            plan_generation != 0U && world_generation != 0U;
    }
    return source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA &&
        owner.role == 0U && owner.incarnation == 0U &&
        scene_version == 0U && plan_generation == 0U &&
        world_generation == 0U;
}

static bool trace_decode_wifi_record(
    const uint8_t *payload,
    size_t payload_size,
    bool *out_enabled,
    MeisoWifiTelemetryDesiredParameters *out_parameters
)
{
    MeisoWifiTelemetryDesiredParameters candidate = {0U, 0U};

    if (payload == NULL || out_enabled == NULL || out_parameters == NULL ||
        payload_size != 16U || payload[0] > 1U ||
        !trace_bytes_zero(payload + 1U, 7U)) {
        return false;
    }
    if (payload[0] == 0U) {
        if (!trace_bytes_zero(payload + 8U,
                              MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE)) {
            return false;
        }
    } else if (meiso_wifi_telemetry_desired_parameters_decode(
                   (MeisoByteView){
                       payload + 8U,
                       MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
                   },
                   &candidate
               ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return false;
    }
    *out_enabled = payload[0] != 0U;
    *out_parameters = candidate;
    return true;
}

static bool trace_decode_media_record(
    const uint8_t *payload,
    size_t payload_size,
    bool *out_enabled,
    MeisoCompositionMediaDesired *out_desired
)
{
    MeisoCompositionMediaDesired candidate = {0};

    if (payload == NULL || out_enabled == NULL || out_desired == NULL ||
        payload_size != 40U || payload[0] > 1U ||
        !trace_bytes_zero(payload + 1U, 7U)) {
        return false;
    }
    if (payload[0] == 0U) {
        if (!trace_bytes_zero(payload + 8U,
                              MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE)) {
            return false;
        }
    } else if (meiso_composition_media_desired_decode(
                   (MeisoByteView){
                       payload + 8U,
                       MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE
                   },
                   &candidate
               ) != MEISO_COMPOSITION_SESSION_V3_OK) {
        return false;
    }
    *out_enabled = payload[0] != 0U;
    *out_desired = candidate;
    return true;
}

static bool trace_decode_frame_record(
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *unit_override,
    MeisoCompositionEdgeSimulatorFrame *out_frame
)
{
    MeisoCompositionEdgeSimulatorFrame candidate;
    uint32_t unit_size;

    if (payload == NULL || out_frame == NULL ||
        payload_size < MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES ||
        payload_size > TRACE_MAX_FRAME_PAYLOAD || payload[1U] != 0U ||
        !trace_bytes_zero(payload + 12U, 4U) ||
        !trace_bytes_zero(payload + 49U, 7U) ||
        !trace_bytes_zero(payload + 108U, 4U)) {
        return false;
    }
    unit_size = trace_get_u32(payload + 104U);
    if ((size_t)unit_size !=
        payload_size - MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.source = payload[0U];
    candidate.flags = trace_get_u16(payload + 2U);
    candidate.pixel_format = trace_get_u16(payload + 4U);
    candidate.codec = trace_get_u16(payload + 6U);
    candidate.width = trace_get_u16(payload + 8U);
    candidate.height = trace_get_u16(payload + 10U);
    candidate.media_stream_epoch = trace_get_u64(payload + 16U);
    candidate.frame_sequence = trace_get_u64(payload + 24U);
    candidate.source_time.clock_epoch = trace_get_u64(payload + 32U);
    candidate.source_time.monotonic_ns = trace_get_u64(payload + 40U);
    candidate.applied_scene_owner.role = payload[48U];
    candidate.applied_scene_owner.incarnation = trace_get_u64(payload + 56U);
    candidate.applied_scene_state_version = trace_get_u64(payload + 64U);
    candidate.active_plan_generation = trace_get_u64(payload + 72U);
    candidate.world_generation = trace_get_u64(payload + 80U);
    candidate.dependency_sequence = trace_get_u64(payload + 88U);
    candidate.producer_dropped_frames = trace_get_u64(payload + 96U);
    candidate.access_unit.data = unit_override != NULL ? unit_override :
        payload + MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES;
    candidate.access_unit.size = unit_size;
    if (!meiso_time_point_valid(candidate.source_time) || unit_size == 0U ||
        candidate.media_stream_epoch == 0U ||
        candidate.frame_sequence == 0U ||
        (candidate.flags & MEISO_COMPOSITION_MEDIA_FRAME_KEY) == 0U ||
        (candidate.flags &
         (MeisoCompositionMediaFrameFlags)~
             (MEISO_COMPOSITION_MEDIA_FRAME_KEY |
              MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U ||
        !trace_frame_scene_valid(
            candidate.source, candidate.applied_scene_owner,
            candidate.applied_scene_state_version,
            candidate.active_plan_generation, candidate.world_generation,
            candidate.dependency_sequence
        ) ||
        !trace_frame_size_exact(candidate.pixel_format, candidate.width,
                                candidate.height, unit_size)) {
        return false;
    }
    *out_frame = candidate;
    return true;
}

static MeisoCompositionEdgeTraceStatus trace_semantic_apply(
    TraceSemanticState *state,
    MeisoCompositionEdgeTraceRecordKind kind,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *frame_unit_override
)
{
    bool enabled;
    MeisoWifiTelemetryDesiredParameters wifi;
    MeisoCompositionMediaDesired media;
    MeisoCompositionEdgeSimulatorFrame frame;
    TraceMediaState *route;
    size_t index;

    if (state == NULL || state->terminated) {
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
    switch (kind) {
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE:
        if (payload_size != 0U || state->activated) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        state->activated = true;
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI:
        if (!trace_decode_wifi_record(payload, payload_size, &enabled, &wifi)) {
            return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
        }
        if ((enabled &&
             (!state->wifi_epoch_available ||
              !meiso_wifi_telemetry_parameters_supported(
                  &state->config.catalog.wifi_capability, &wifi))) ||
            (!enabled && (wifi.selected_fields != 0U ||
                          wifi.sample_period_ms != 0U))) {
            return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
        }
        state->wifi_pending = true;
        state->wifi_pending_enabled = enabled;
        state->wifi_pending_desired = wifi;
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA:
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_CAMERA_MEDIA:
        if (!trace_decode_media_record(payload, payload_size, &enabled,
                                       &media)) {
            return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
        }
        index = kind == MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA ?
            0U : 1U;
        route = &state->media[index];
        if (!enabled) {
            route->pending = true;
            route->pending_enabled = false;
            memset(&route->pending_desired, 0,
                   sizeof(route->pending_desired));
            return MEISO_COMPOSITION_EDGE_TRACE_OK;
        }
        if (!route->epoch_available ||
            !meiso_composition_media_desired_supported(
                index == 0U ?
                    &state->config.catalog.render_media_capability :
                    &state->config.catalog.camera_media_capability,
                &media)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
        }
        route->pending = true;
        route->pending_enabled = enabled;
        route->pending_desired = media;
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP:
        if (!state->activated || payload == NULL || payload_size != 16U ||
            !meiso_time_point_valid((MeisoTimePoint){
                trace_get_u64(payload), trace_get_u64(payload + 8U)
            })) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        if (state->wifi_pending) {
            state->wifi_pending = false;
            if (state->wifi_pending_enabled) {
                if (state->next_wifi_epoch == UINT64_MAX) {
                    state->wifi_epoch_available = false;
                } else {
                    ++state->next_wifi_epoch;
                }
            }
        } else {
            index = state->media[0].pending ? 0U :
                (state->media[1].pending ? 1U : SIZE_MAX);
            if (index != SIZE_MAX) {
                route = &state->media[index];
                route->pending = false;
                route->active = route->pending_enabled;
                route->active_desired = route->pending_desired;
                route->active_epoch = route->pending_enabled ?
                    route->next_epoch : 0U;
                route->sequence_high_water = 0U;
                route->drop_high_water = 0U;
                if (route->pending_enabled) {
                    if (route->next_epoch == UINT64_MAX) {
                        route->epoch_available = false;
                    } else {
                        ++route->next_epoch;
                    }
                }
            }
        }
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME:
        if (!state->activated) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        if (!trace_decode_frame_record(payload, payload_size,
                                       frame_unit_override, &frame)) {
            return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
        }
        if (frame.source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
            index = 0U;
        } else if (frame.source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
            index = 1U;
        } else {
            return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
        }
        route = &state->media[index];
        if (!route->active || frame.media_stream_epoch != route->active_epoch ||
            frame.pixel_format != route->active_desired.pixel_format ||
            frame.codec != route->active_desired.codec ||
            frame.width != route->active_desired.width ||
            frame.height != route->active_desired.height ||
            frame.frame_sequence <= route->sequence_high_water ||
            frame.producer_dropped_frames < route->drop_high_water ||
            frame.access_unit.size >
                (index == 0U ?
                     state->config.catalog.render_media_capability
                         .max_access_unit_bytes :
                     state->config.catalog.camera_media_capability
                         .max_access_unit_bytes)) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        route->sequence_high_water = frame.frame_sequence;
        route->drop_high_water = frame.producer_dropped_frames;
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE:
        if (!state->activated || payload == NULL || payload_size != 8U ||
            payload[0] == MEISO_EDGE_TERMINAL_NONE ||
            payload[0] > MEISO_EDGE_TERMINAL_INTERNAL_FAILURE ||
            !trace_bytes_zero(payload + 1U, 7U)) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        state->terminated = true;
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG:
    default:
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
}

static void trace_write_record_header(
    uint8_t output[MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES],
    MeisoCompositionEdgeTraceRecordKind kind,
    uint32_t payload_size,
    uint32_t sequence,
    uint32_t payload_crc
)
{
    memset(output, 0, MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES);
    output[0] = kind;
    trace_put_u16(output + 2U,
                  MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES);
    trace_put_u32(output + 4U, payload_size);
    trace_put_u32(output + 8U, sequence);
    trace_put_u32(output + 12U, payload_crc);
}

static bool trace_writer_valid(
    const MeisoCompositionEdgeTraceWriter *writer
)
{
    return writer != NULL && writer->magic == TRACE_WRITER_MAGIC &&
        writer->arena_begin == (uintptr_t)writer &&
        writer->arena_end > writer->arena_begin &&
        writer->arena_end - writer->arena_begin == sizeof(*writer) &&
        writer->output != NULL &&
        writer->output_begin == (uintptr_t)writer->output &&
        writer->output_end > writer->output_begin &&
        writer->output_end - writer->output_begin == writer->capacity &&
        writer->capacity >= TRACE_MIN_BYTES &&
        writer->capacity <= MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES &&
        writer->size >= MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
                MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
                MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES &&
        writer->size <= writer->capacity &&
        writer->record_count >= 1U &&
        writer->record_count <= MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS &&
        !trace_ranges_overlap(writer->arena_begin, writer->arena_end,
                              writer->output_begin, writer->output_end);
}

static bool trace_writer_input_disjoint(
    const MeisoCompositionEdgeTraceWriter *writer,
    const void *input,
    size_t input_size
)
{
    uintptr_t begin;
    uintptr_t end;

    if (input == NULL && input_size == 0U) {
        return true;
    }
    return trace_checked_range(input, input_size, &begin, &end) &&
        !trace_ranges_overlap(begin, end, writer->arena_begin,
                              writer->arena_end) &&
        !trace_ranges_overlap(begin, end, writer->output_begin,
                              writer->output_end);
}

static MeisoCompositionEdgeTraceStatus trace_writer_append(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoCompositionEdgeTraceRecordKind kind,
    const uint8_t *prefix,
    size_t prefix_size,
    const uint8_t *suffix,
    size_t suffix_size
)
{
    TraceSemanticState semantic;
    uint8_t header[MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES];
    size_t payload_size;
    size_t record_size;
    size_t next_size;
    uint32_t sequence;
    uint32_t payload_crc;
    MeisoCompositionEdgeTraceStatus status;

    if (!trace_writer_valid(writer) || writer->finalized ||
        (prefix_size != 0U && prefix == NULL) ||
        (suffix_size != 0U && suffix == NULL) ||
        !trace_writer_input_disjoint(writer, prefix, prefix_size) ||
        !trace_writer_input_disjoint(writer, suffix, suffix_size) ||
        !trace_range_disjoint(prefix, prefix_size, suffix, suffix_size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!trace_checked_add(prefix_size, suffix_size, &payload_size) ||
        payload_size > TRACE_MAX_FRAME_PAYLOAD ||
        payload_size > UINT32_MAX ||
        !trace_checked_add(MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES,
                           payload_size, &record_size) ||
        !trace_checked_add(writer->size, record_size, &next_size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_CAPACITY;
    }
    if (writer->record_count == MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS ||
        next_size > writer->capacity ||
        next_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES) {
        return MEISO_COMPOSITION_EDGE_TRACE_CAPACITY;
    }
    semantic = writer->semantic;
    status = trace_semantic_apply(
        &semantic, kind, prefix, payload_size,
        kind == MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME ? suffix : NULL
    );
    if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
        return status;
    }
    sequence = writer->record_count + 1U;
    payload_crc = trace_crc32c_pair(prefix, prefix_size, suffix, suffix_size);
    trace_write_record_header(header, kind, (uint32_t)payload_size, sequence,
                              payload_crc);
    memmove(writer->output + writer->size, header, sizeof(header));
    if (prefix_size != 0U) {
        memmove(writer->output + writer->size + sizeof(header),
                prefix, prefix_size);
    }
    if (suffix_size != 0U) {
        memmove(writer->output + writer->size + sizeof(header) + prefix_size,
                suffix, suffix_size);
    }
    writer->semantic = semantic;
    writer->size = next_size;
    writer->record_count = sequence;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

size_t meiso_composition_edge_trace_writer_alignment_required(void)
{
    return _Alignof(MeisoCompositionEdgeTraceWriter);
}

size_t meiso_composition_edge_trace_writer_memory_required(void)
{
    return sizeof(MeisoCompositionEdgeTraceWriter);
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_init(
    void *arena,
    size_t arena_size,
    MeisoMutableBuffer output,
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoCompositionEdgeTraceWriter **out_writer
)
{
    MeisoCompositionEdgeTraceWriter candidate;
    uint8_t config_bytes[MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES];
    uint8_t record_header[
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES
    ];
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    uintptr_t config_begin;
    uintptr_t config_end;
    uintptr_t result_begin;
    uintptr_t result_end;
    size_t initial_size = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES;

    if (arena == NULL || output.data == NULL || config == NULL ||
        out_writer == NULL || output.capacity < TRACE_MIN_BYTES ||
        output.capacity > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES ||
        !trace_checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !trace_checked_range(output.data, output.capacity,
                             &output_begin, &output_end) ||
        !trace_checked_range(config, sizeof(*config),
                             &config_begin, &config_end) ||
        !trace_checked_range(out_writer, sizeof(*out_writer),
                             &result_begin, &result_end) ||
        trace_ranges_overlap(config_begin, config_end,
                             result_begin, result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if ((arena_begin % _Alignof(MeisoCompositionEdgeTraceWriter)) != 0U) {
        return MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(MeisoCompositionEdgeTraceWriter)) {
        return MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL;
    }
    arena_end = arena_begin + sizeof(MeisoCompositionEdgeTraceWriter);
    if (trace_ranges_overlap(arena_begin, arena_end,
                             output_begin, output_end) ||
        trace_ranges_overlap(arena_begin, arena_end,
                             config_begin, config_end) ||
        trace_ranges_overlap(arena_begin, arena_end,
                             result_begin, result_end) ||
        trace_ranges_overlap(output_begin, output_end,
                             config_begin, config_end) ||
        trace_ranges_overlap(output_begin, output_end,
                             result_begin, result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!trace_encode_config(config, config_bytes)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.magic = TRACE_WRITER_MAGIC;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = arena_end;
    candidate.output_begin = output_begin;
    candidate.output_end = output_end;
    candidate.output = output.data;
    candidate.capacity = output.capacity;
    candidate.size = initial_size;
    candidate.record_count = 1U;
    trace_semantic_init(&candidate.semantic, config);
    trace_write_record_header(
        record_header, MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG,
        MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES, 1U,
        trace_crc32c(config_bytes, sizeof(config_bytes))
    );
    memset(output.data, 0, MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES);
    memmove(output.data + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES,
            record_header, sizeof(record_header));
    memmove(output.data + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
                sizeof(record_header),
            config_bytes, sizeof(config_bytes));
    memset(arena, 0, sizeof(candidate));
    memmove(arena, &candidate, sizeof(candidate));
    *out_writer = (MeisoCompositionEdgeTraceWriter *)arena;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_activate(
    MeisoCompositionEdgeTraceWriter *writer
)
{
    return trace_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE,
        NULL, 0U, NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_wifi(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters
)
{
    uint8_t payload[16U];
    size_t encoded_size = 0U;

    if (!trace_writer_valid(writer) ||
        (enabled && parameters == NULL) ||
        (!enabled && parameters != NULL) ||
        !trace_writer_input_disjoint(
            writer, parameters, parameters == NULL ? 0U : sizeof(*parameters)
        )) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    memset(payload, 0, sizeof(payload));
    payload[0] = enabled ? 1U : 0U;
    if (enabled &&
        (meiso_wifi_telemetry_desired_parameters_encode(
             parameters,
             (MeisoMutableBuffer){
                 payload + 8U,
                 MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
             },
             &encoded_size
         ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
         encoded_size !=
             MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    return trace_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI,
        payload, sizeof(payload), NULL, 0U
    );
}

static MeisoCompositionEdgeTraceStatus trace_writer_record_media(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoCompositionEdgeTraceRecordKind kind,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
)
{
    uint8_t payload[40U];
    size_t encoded_size = 0U;

    if (!trace_writer_valid(writer) ||
        (enabled && desired == NULL) || (!enabled && desired != NULL) ||
        !trace_writer_input_disjoint(
            writer, desired, desired == NULL ? 0U : sizeof(*desired)
        )) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    memset(payload, 0, sizeof(payload));
    payload[0] = enabled ? 1U : 0U;
    if (enabled &&
        (meiso_composition_media_desired_encode(
             desired,
             (MeisoMutableBuffer){
                 payload + 8U, MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE
             },
             &encoded_size
         ) != MEISO_COMPOSITION_SESSION_V3_OK ||
         encoded_size != MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    return trace_writer_append(
        writer, kind, payload, sizeof(payload), NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_render_media(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
)
{
    return trace_writer_record_media(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA,
        enabled, desired
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_camera_media(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
)
{
    return trace_writer_record_media(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_CAMERA_MEDIA,
        enabled, desired
    );
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_record_step(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoTimePoint now
)
{
    uint8_t payload[16U];

    if (!trace_writer_valid(writer) || !meiso_time_point_valid(now)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    trace_put_u64(payload, now.clock_epoch);
    trace_put_u64(payload + 8U, now.monotonic_ns);
    return trace_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP,
        payload, sizeof(payload), NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_frame(
    MeisoCompositionEdgeTraceWriter *writer,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    uint8_t prefix[MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES];

    if (!trace_writer_valid(writer) || frame == NULL ||
        !meiso_byte_view_valid(frame->access_unit) ||
        frame->access_unit.size == 0U ||
        frame->access_unit.size > UINT32_MAX ||
        !trace_writer_input_disjoint(writer, frame, sizeof(*frame)) ||
        !trace_writer_input_disjoint(writer, frame->access_unit.data,
                                     frame->access_unit.size) ||
        !trace_range_disjoint(frame, sizeof(*frame), frame->access_unit.data,
                              frame->access_unit.size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    memset(prefix, 0, sizeof(prefix));
    prefix[0] = frame->source;
    trace_put_u16(prefix + 2U, frame->flags);
    trace_put_u16(prefix + 4U, frame->pixel_format);
    trace_put_u16(prefix + 6U, frame->codec);
    trace_put_u16(prefix + 8U, frame->width);
    trace_put_u16(prefix + 10U, frame->height);
    trace_put_u64(prefix + 16U, frame->media_stream_epoch);
    trace_put_u64(prefix + 24U, frame->frame_sequence);
    trace_put_u64(prefix + 32U, frame->source_time.clock_epoch);
    trace_put_u64(prefix + 40U, frame->source_time.monotonic_ns);
    prefix[48U] = frame->applied_scene_owner.role;
    trace_put_u64(prefix + 56U, frame->applied_scene_owner.incarnation);
    trace_put_u64(prefix + 64U, frame->applied_scene_state_version);
    trace_put_u64(prefix + 72U, frame->active_plan_generation);
    trace_put_u64(prefix + 80U, frame->world_generation);
    trace_put_u64(prefix + 88U, frame->dependency_sequence);
    trace_put_u64(prefix + 96U, frame->producer_dropped_frames);
    trace_put_u32(prefix + 104U, (uint32_t)frame->access_unit.size);
    return trace_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME,
        prefix, sizeof(prefix), frame->access_unit.data,
        frame->access_unit.size
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_terminate(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoEdgeTerminalReason reason
)
{
    uint8_t payload[8U] = {0U};

    if (!trace_writer_valid(writer) || reason == MEISO_EDGE_TERMINAL_NONE ||
        reason > MEISO_EDGE_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    payload[0] = reason;
    return trace_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE,
        payload, sizeof(payload), NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_finalize(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoByteView *out_trace
)
{
    uint8_t header[MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES];

    if (!trace_writer_valid(writer) || out_trace == NULL ||
        !trace_writer_input_disjoint(writer, out_trace, sizeof(*out_trace))) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (writer->finalized || !writer->semantic.activated ||
        !writer->semantic.terminated || writer->record_count < 3U ||
        writer->size > UINT32_MAX) {
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
    memset(header, 0, sizeof(header));
    memmove(header, "MECT", 4U);
    trace_put_u16(header + 4U, MEISO_COMPOSITION_EDGE_TRACE_VERSION);
    trace_put_u16(header + 6U, MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES);
    trace_put_u32(header + 8U, TRACE_FLAG_SYNTHETIC);
    trace_put_u32(header + 12U, writer->record_count);
    trace_put_u32(header + 16U, (uint32_t)writer->size);
    trace_put_u32(
        header + 20U,
        trace_crc32c(
            writer->output + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES,
            writer->size - MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES
        )
    );
    trace_put_u32(header + 24U, trace_crc32c(header, sizeof(header)));
    memmove(writer->output, header, sizeof(header));
    writer->finalized = true;
    *out_trace = (MeisoByteView){writer->output, writer->size};
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

static bool trace_reader_valid(const MeisoCompositionEdgeTraceReader *reader)
{
    return reader != NULL && reader->magic == TRACE_READER_MAGIC &&
        reader->arena_begin == (uintptr_t)reader &&
        reader->arena_end > reader->arena_begin &&
        reader->arena_end - reader->arena_begin == sizeof(*reader) &&
        reader->trace != NULL && reader->trace_begin == (uintptr_t)reader->trace &&
        reader->trace_end > reader->trace_begin &&
        reader->trace_end - reader->trace_begin == reader->trace_size &&
        reader->trace_size >= TRACE_MIN_BYTES &&
        reader->trace_size <= MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES &&
        reader->cursor <= reader->trace_size && reader->record_count >= 3U &&
        reader->record_count <= MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS &&
        reader->next_sequence >= 2U &&
        reader->next_sequence <= reader->record_count + 1U &&
        !trace_ranges_overlap(reader->arena_begin, reader->arena_end,
                              reader->trace_begin, reader->trace_end);
}

static bool trace_header_valid(
    const uint8_t *trace,
    size_t trace_size,
    uint32_t *out_record_count
)
{
    uint8_t header[MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES];
    uint32_t stored_header_crc;
    uint32_t stored_body_crc;
    uint32_t record_count;

    if (trace == NULL || out_record_count == NULL ||
        trace_size < TRACE_MIN_BYTES ||
        trace_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES ||
        memcmp(trace, "MECT", 4U) != 0 ||
        trace_get_u16(trace + 4U) != MEISO_COMPOSITION_EDGE_TRACE_VERSION ||
        trace_get_u16(trace + 6U) !=
            MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES ||
        trace_get_u32(trace + 8U) != TRACE_FLAG_SYNTHETIC ||
        trace_get_u32(trace + 16U) != trace_size ||
        !trace_bytes_zero(trace + 28U, 4U)) {
        return false;
    }
    record_count = trace_get_u32(trace + 12U);
    if (record_count < 3U ||
        record_count > MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS) {
        return false;
    }
    stored_body_crc = trace_get_u32(trace + 20U);
    if (stored_body_crc != trace_crc32c(
            trace + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES,
            trace_size - MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES
        )) {
        return false;
    }
    memmove(header, trace, sizeof(header));
    stored_header_crc = trace_get_u32(header + 24U);
    memset(header + 24U, 0, 4U);
    if (stored_header_crc != trace_crc32c(header, sizeof(header))) {
        return false;
    }
    *out_record_count = record_count;
    return true;
}

static MeisoCompositionEdgeTraceStatus trace_record_at(
    const uint8_t *trace,
    size_t trace_size,
    size_t cursor,
    uint32_t expected_sequence,
    MeisoCompositionEdgeTraceRecordKind *out_kind,
    const uint8_t **out_payload,
    size_t *out_payload_size,
    size_t *out_next_cursor
)
{
    const uint8_t *header;
    const uint8_t *payload;
    size_t payload_size;
    size_t next_cursor;

    if (trace == NULL || out_kind == NULL || out_payload == NULL ||
        out_payload_size == NULL || out_next_cursor == NULL ||
        cursor > trace_size ||
        trace_size - cursor <
            MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES) {
        return MEISO_COMPOSITION_EDGE_TRACE_TRUNCATED;
    }
    header = trace + cursor;
    if (header[1U] != 0U ||
        trace_get_u16(header + 2U) !=
            MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES ||
        trace_get_u32(header + 8U) != expected_sequence ||
        !trace_bytes_zero(header + 16U, 8U)) {
        return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
    }
    payload_size = trace_get_u32(header + 4U);
    if (payload_size > TRACE_MAX_FRAME_PAYLOAD ||
        !trace_checked_add(
            cursor + MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES,
            payload_size, &next_cursor
        ) || next_cursor > trace_size) {
        return MEISO_COMPOSITION_EDGE_TRACE_TRUNCATED;
    }
    payload = header + MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES;
    if (trace_get_u32(header + 12U) !=
        trace_crc32c(payload, payload_size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    if (header[0U] < MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG ||
        header[0U] > MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE) {
        return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
    }
    *out_kind = header[0U];
    *out_payload = payload;
    *out_payload_size = payload_size;
    *out_next_cursor = next_cursor;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

size_t meiso_composition_edge_trace_reader_alignment_required(void)
{
    return _Alignof(MeisoCompositionEdgeTraceReader);
}

size_t meiso_composition_edge_trace_reader_memory_required(void)
{
    return sizeof(MeisoCompositionEdgeTraceReader);
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_reader_init(
    void *arena,
    size_t arena_size,
    MeisoByteView trace,
    MeisoCompositionEdgeTraceReader **out_reader
)
{
    MeisoCompositionEdgeTraceReader candidate;
    MeisoCompositionEdgeSimulatorConfig config;
    TraceSemanticState semantic;
    MeisoCompositionEdgeTraceRecordKind kind;
    const uint8_t *payload;
    size_t payload_size;
    size_t cursor;
    size_t next_cursor;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t trace_begin;
    uintptr_t trace_end;
    uintptr_t result_begin;
    uintptr_t result_end;
    uint32_t record_count;
    uint32_t sequence;
    MeisoCompositionEdgeTraceStatus status;

    if (arena == NULL || out_reader == NULL ||
        !meiso_byte_view_valid(trace) || trace.size < TRACE_MIN_BYTES ||
        trace.size > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES ||
        !trace_checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !trace_checked_range(trace.data, trace.size, &trace_begin, &trace_end) ||
        !trace_checked_range(out_reader, sizeof(*out_reader),
                             &result_begin, &result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if ((arena_begin % _Alignof(MeisoCompositionEdgeTraceReader)) != 0U) {
        return MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(MeisoCompositionEdgeTraceReader)) {
        return MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL;
    }
    arena_end = arena_begin + sizeof(MeisoCompositionEdgeTraceReader);
    if (trace_ranges_overlap(arena_begin, arena_end, trace_begin, trace_end) ||
        trace_ranges_overlap(arena_begin, arena_end,
                             result_begin, result_end) ||
        trace_ranges_overlap(trace_begin, trace_end,
                             result_begin, result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!trace_header_valid(trace.data, trace.size, &record_count)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    cursor = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES;
    status = trace_record_at(
        trace.data, trace.size, cursor, 1U, &kind, &payload,
        &payload_size, &next_cursor
    );
    if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
        return status;
    }
    if (kind != MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG ||
        payload_size != MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES ||
        !trace_decode_config(payload, payload_size, &config)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    cursor = next_cursor;
    trace_semantic_init(&semantic, &config);
    for (sequence = 2U; sequence <= record_count; ++sequence) {
        status = trace_record_at(
            trace.data, trace.size, cursor, sequence, &kind, &payload,
            &payload_size, &next_cursor
        );
        if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
            return status;
        }
        status = trace_semantic_apply(
            &semantic, kind, payload, payload_size, NULL
        );
        if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
            return status;
        }
        if (semantic.terminated && sequence != record_count) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        cursor = next_cursor;
    }
    if (cursor != trace.size || !semantic.activated ||
        !semantic.terminated) {
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.magic = TRACE_READER_MAGIC;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = arena_end;
    candidate.trace_begin = trace_begin;
    candidate.trace_end = trace_end;
    candidate.trace = trace.data;
    candidate.trace_size = trace.size;
    candidate.cursor = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES;
    candidate.record_count = record_count;
    candidate.next_sequence = 2U;
    candidate.config = config;
    memset(arena, 0, sizeof(candidate));
    memmove(arena, &candidate, sizeof(candidate));
    *out_reader = (MeisoCompositionEdgeTraceReader *)arena;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_reader_simulator_config(
    const MeisoCompositionEdgeTraceReader *reader,
    MeisoCompositionEdgeSimulatorConfig *out_config
)
{
    uintptr_t output_begin;
    uintptr_t output_end;
    uint32_t record_count;

    if (!trace_reader_valid(reader) || out_config == NULL ||
        !trace_checked_range(out_config, sizeof(*out_config),
                             &output_begin, &output_end) ||
        trace_ranges_overlap(output_begin, output_end,
                             reader->arena_begin, reader->arena_end) ||
        trace_ranges_overlap(output_begin, output_end,
                             reader->trace_begin, reader->trace_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!trace_header_valid(reader->trace, reader->trace_size, &record_count) ||
        record_count != reader->record_count) {
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    *out_config = reader->config;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

static bool trace_replay_ranges_valid(
    const MeisoCompositionEdgeTraceReader *reader,
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeTraceReplayResult *out_result
)
{
    uintptr_t output_begin;
    uintptr_t output_end;

    return reader != NULL && handle != NULL && out_result != NULL &&
        trace_checked_range(out_result, sizeof(*out_result),
                            &output_begin, &output_end) &&
        !trace_ranges_overlap(output_begin, output_end,
                              reader->arena_begin, reader->arena_end) &&
        !trace_ranges_overlap(output_begin, output_end,
                              reader->trace_begin, reader->trace_end) &&
        meiso_composition_edge_internal_simulator_storage_disjoint(
            handle, reader, sizeof(*reader)
        ) &&
        meiso_composition_edge_internal_simulator_storage_disjoint(
            handle, reader->trace, reader->trace_size
        ) &&
        meiso_composition_edge_internal_simulator_storage_disjoint(
            handle, out_result, sizeof(*out_result)
        );
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_reader_replay_next(
    MeisoCompositionEdgeTraceReader *reader,
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeTraceReplayResult *out_result
)
{
    MeisoCompositionEdgeTraceReplayResult result;
    MeisoCompositionEdgeTraceRecordKind kind;
    const uint8_t *payload;
    size_t payload_size;
    size_t next_cursor;
    bool enabled;
    MeisoWifiTelemetryDesiredParameters wifi;
    MeisoCompositionMediaDesired media;
    MeisoCompositionEdgeSimulatorFrame frame;
    MeisoEdgeCommandReceipt receipt;
    MeisoCompositionEdgeSimulatorTransition transition;
    MeisoEdgeStatus edge_status;
    MeisoCompositionEdgeTraceStatus status;

    if (!trace_reader_valid(reader) ||
        !trace_replay_ranges_valid(reader, handle, out_result)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (reader->cursor == reader->trace_size) {
        return MEISO_COMPOSITION_EDGE_TRACE_COMPLETE;
    }
    if ((reader->bound_handle == 0U &&
         !meiso_composition_edge_internal_simulator_fresh_matches(
             handle, &reader->config
         )) ||
        (reader->bound_handle != 0U &&
         reader->bound_handle != (uintptr_t)handle)) {
        return MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED;
    }
    status = trace_record_at(
        reader->trace, reader->trace_size, reader->cursor,
        reader->next_sequence, &kind, &payload, &payload_size, &next_cursor
    );
    if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
        return status;
    }
    memset(&result, 0, sizeof(result));
    result.record_sequence = reader->next_sequence;
    result.record_kind = kind;
    switch (kind) {
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE:
        edge_status = meiso_composition_edge_simulator_activate(handle);
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI:
        if (!trace_decode_wifi_record(payload, payload_size, &enabled, &wifi)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = meiso_composition_edge_set_wifi(
            handle, enabled, enabled ? &wifi : NULL, &receipt
        );
        if (edge_status == MEISO_EDGE_OK) {
            result.receipt_present = true;
            result.receipt = receipt;
        }
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA:
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_CAMERA_MEDIA:
        if (!trace_decode_media_record(payload, payload_size,
                                       &enabled, &media)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = kind ==
                MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA ?
            meiso_composition_edge_set_render_media(
                handle, enabled, enabled ? &media : NULL, &receipt
            ) :
            meiso_composition_edge_set_camera_media(
                handle, enabled, enabled ? &media : NULL, &receipt
            );
        if (edge_status == MEISO_EDGE_OK) {
            result.receipt_present = true;
            result.receipt = receipt;
        }
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP:
        if (payload_size != 16U) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = meiso_composition_edge_simulator_step(
            handle,
            (MeisoTimePoint){trace_get_u64(payload),
                             trace_get_u64(payload + 8U)},
            &transition
        );
        if (edge_status == MEISO_EDGE_OK) {
            result.transition_present = true;
            result.transition = transition;
        }
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME:
        if (!trace_decode_frame_record(payload, payload_size, NULL, &frame)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status =
            meiso_composition_edge_simulator_publish_frame(handle, &frame);
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE:
        if (payload_size != 8U) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = meiso_composition_edge_simulator_terminate(
            handle, payload[0]
        );
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG:
    default:
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    if (edge_status != MEISO_EDGE_OK) {
        return MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED;
    }
    reader->bound_handle = (uintptr_t)handle;
    reader->cursor = next_cursor;
    ++reader->next_sequence;
    *out_result = result;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}
