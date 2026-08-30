#include "meiso_composition_edge_trace_codec_private.h"

/* Trace codec owns canonical bytes, integrity checks and semantic validation. */

#include <limits.h>
#include <stdint.h>
#include <string.h>

bool meiso_composition_edge_trace_private_checked_range(
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

bool meiso_composition_edge_trace_private_checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool meiso_composition_edge_trace_private_ranges_overlap(
    uintptr_t lhs_begin,
    uintptr_t lhs_end,
    uintptr_t rhs_begin,
    uintptr_t rhs_end
)
{
    return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

bool meiso_composition_edge_trace_private_range_disjoint(
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
    return meiso_composition_edge_trace_private_checked_range(lhs, lhs_size, &lhs_begin, &lhs_end) &&
        meiso_composition_edge_trace_private_checked_range(rhs, rhs_size, &rhs_begin, &rhs_end) &&
        !meiso_composition_edge_trace_private_ranges_overlap(lhs_begin, lhs_end, rhs_begin, rhs_end);
}

bool meiso_composition_edge_trace_private_bytes_zero(const uint8_t *bytes, size_t size)
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

uint16_t meiso_composition_edge_trace_private_get_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] << 8U) |
        (uint16_t)bytes[1];
}

uint32_t meiso_composition_edge_trace_private_get_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24U |
        (uint32_t)bytes[1] << 16U |
        (uint32_t)bytes[2] << 8U |
        (uint32_t)bytes[3];
}

uint64_t meiso_composition_edge_trace_private_get_u64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    size_t i;

    for (i = 0U; i < 8U; ++i) {
        value = value << 8U | bytes[i];
    }
    return value;
}

void meiso_composition_edge_trace_private_put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

void meiso_composition_edge_trace_private_put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

void meiso_composition_edge_trace_private_put_u64(uint8_t *bytes, uint64_t value)
{
    size_t i;

    for (i = 0U; i < 8U; ++i) {
        bytes[7U - i] = (uint8_t)value;
        value >>= 8U;
    }
}

uint32_t meiso_composition_edge_trace_private_crc32c_update(
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

uint32_t meiso_composition_edge_trace_private_crc32c(const uint8_t *bytes, size_t size)
{
    if (size == 0U) {
        return 0U;
    }
    return meiso_composition_edge_trace_private_crc32c_update(UINT32_MAX, bytes, size) ^ UINT32_MAX;
}

uint32_t meiso_composition_edge_trace_private_crc32c_pair(
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
    crc = meiso_composition_edge_trace_private_crc32c_update(UINT32_MAX, prefix, prefix_size);
    crc = meiso_composition_edge_trace_private_crc32c_update(crc, suffix, suffix_size);
    return crc ^ UINT32_MAX;
}

static bool meiso_composition_edge_trace_private_device_sample_zero(
    const MeisoDeviceTelemetrySample *sample
)
{
    return sample != NULL && sample->selected_fields == 0U &&
        sample->present_fields == 0U && sample->temperature_millic == 0 &&
        sample->temperature_uncertainty_millic == 0U &&
        sample->cpu_load_ppm == 0U && sample->memory_total_bytes == 0U &&
        sample->memory_available_bytes == 0U;
}

bool meiso_composition_edge_trace_private_config_valid(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    uint8_t dynamic_capability[
        MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES
    ];
    MeisoRuntimeEndpointDescriptor
        endpoints[MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT];
    MeisoCompositionProfileRouteDescriptor
        routes[MEISO_COMPOSITION_PROFILE_ROUTE_COUNT];
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
        meiso_composition_edge_trace_private_device_sample_zero(&config->device_sample) &&
        !config->composition_enabled &&
        !config->cpu_raster_preview_enabled &&
        config->composition_blob_slot_count == 0U &&
        config->composition_clock_epoch == 0U &&
        config->presentation_production_epoch == 0U &&
        config->scene_production_epoch == 0U &&
        config->catalog.render_media_capability.max_access_unit_bytes != 0U &&
        config->catalog.render_media_capability.max_access_unit_bytes <=
            MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES &&
        config->catalog.camera_media_capability.max_access_unit_bytes != 0U &&
        config->catalog.camera_media_capability.max_access_unit_bytes <=
            MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES &&
        meiso_composition_edge_simulator_memory_required(config) != 0U &&
        meiso_composition_profile_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){dynamic_capability,
                                 sizeof(dynamic_capability)},
            endpoints, MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            routes, MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) == MEISO_COMPOSITION_PROFILE_OK &&
        meiso_wifi_telemetry_sample_encode(
            &config->wifi_sample,
            (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)},
            &sample_size
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        sample_size == MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE &&
        (config->wifi_sample.selected_fields &
         ~config->catalog.wifi_capability.supported_fields) == 0U;
}

bool meiso_composition_edge_trace_private_encode_config(
    const MeisoCompositionEdgeSimulatorConfig *config,
    uint8_t output[MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES]
)
{
    size_t encoded_size;

    if (!meiso_composition_edge_trace_private_config_valid(config) || output == NULL) {
        return false;
    }
    memset(output, 0, MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES);
    meiso_composition_edge_trace_private_put_u64(output, config->catalog_generation);
    meiso_composition_edge_trace_private_put_u64(output + 8U, config->continuity_serial);
    output[16U] = config->desired_owner.role;
    meiso_composition_edge_trace_private_put_u64(output + 24U, config->desired_owner.incarnation);
    meiso_composition_edge_trace_private_put_u64(output + 32U, config->wifi_production_epoch);
    meiso_composition_edge_trace_private_put_u64(output + 40U, config->render_media_stream_epoch);
    meiso_composition_edge_trace_private_put_u64(output + 48U, config->camera_media_stream_epoch);
    meiso_composition_edge_trace_private_put_u64(output + 56U, config->catalog.wifi_incarnation);
    meiso_composition_edge_trace_private_put_u64(output + 64U, config->catalog.presentation_incarnation);
    meiso_composition_edge_trace_private_put_u64(output + 72U, config->catalog.scene_incarnation);
    meiso_composition_edge_trace_private_put_u64(output + 80U,
                  config->catalog.device_telemetry_incarnation);
    meiso_composition_edge_trace_private_put_u64(output + 88U, config->catalog.render_media_incarnation);
    meiso_composition_edge_trace_private_put_u64(output + 96U, config->catalog.camera_media_incarnation);
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
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE) {
        return false;
    }
    encoded_size = 0U;
    if (meiso_composition_media_capability_encode(
            &config->catalog.camera_media_capability,
            (MeisoMutableBuffer){output + 232U,
                                 MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE},
            &encoded_size
        ) != MEISO_COMPOSITION_PROFILE_OK ||
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

bool meiso_composition_edge_trace_private_decode_config(
    const uint8_t *input,
    size_t input_size,
    MeisoCompositionEdgeSimulatorConfig *out_config
)
{
    MeisoCompositionEdgeSimulatorConfig candidate;

    if (input == NULL || out_config == NULL ||
        input_size != MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES ||
        !meiso_composition_edge_trace_private_bytes_zero(input + 17U, 7U)) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.catalog_generation = meiso_composition_edge_trace_private_get_u64(input);
    candidate.continuity_serial = meiso_composition_edge_trace_private_get_u64(input + 8U);
    candidate.desired_owner.role = input[16U];
    candidate.desired_owner.incarnation = meiso_composition_edge_trace_private_get_u64(input + 24U);
    candidate.wifi_production_epoch = meiso_composition_edge_trace_private_get_u64(input + 32U);
    candidate.render_media_stream_epoch = meiso_composition_edge_trace_private_get_u64(input + 40U);
    candidate.camera_media_stream_epoch = meiso_composition_edge_trace_private_get_u64(input + 48U);
    candidate.catalog.wifi_incarnation = meiso_composition_edge_trace_private_get_u64(input + 56U);
    candidate.catalog.presentation_incarnation = meiso_composition_edge_trace_private_get_u64(input + 64U);
    candidate.catalog.scene_incarnation = meiso_composition_edge_trace_private_get_u64(input + 72U);
    candidate.catalog.device_telemetry_incarnation =
        meiso_composition_edge_trace_private_get_u64(input + 80U);
    candidate.catalog.render_media_incarnation =
        meiso_composition_edge_trace_private_get_u64(input + 88U);
    candidate.catalog.camera_media_incarnation =
        meiso_composition_edge_trace_private_get_u64(input + 96U);
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
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        meiso_composition_media_capability_decode(
            (MeisoByteView){input + 232U,
                            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE},
            &candidate.catalog.camera_media_capability
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        meiso_wifi_telemetry_sample_decode(
            (MeisoByteView){input + 296U,
                            MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE},
            &candidate.wifi_sample
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        !meiso_composition_edge_trace_private_config_valid(&candidate)) {
        return false;
    }
    *out_config = candidate;
    return true;
}

void meiso_composition_edge_trace_private_semantic_init(
    MeisoCompositionEdgeTraceSemanticState *state,
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

static bool meiso_composition_edge_trace_private_frame_size_exact(
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

static bool meiso_composition_edge_trace_private_frame_scene_valid(
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

bool meiso_composition_edge_trace_private_decode_wifi_record(
    const uint8_t *payload,
    size_t payload_size,
    bool *out_enabled,
    MeisoWifiTelemetryDesiredParameters *out_parameters
)
{
    MeisoWifiTelemetryDesiredParameters candidate = {0U, 0U};

    if (payload == NULL || out_enabled == NULL || out_parameters == NULL ||
        payload_size != 16U || payload[0] > 1U ||
        !meiso_composition_edge_trace_private_bytes_zero(payload + 1U, 7U)) {
        return false;
    }
    if (payload[0] == 0U) {
        if (!meiso_composition_edge_trace_private_bytes_zero(payload + 8U,
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

bool meiso_composition_edge_trace_private_decode_media_record(
    const uint8_t *payload,
    size_t payload_size,
    bool *out_enabled,
    MeisoCompositionMediaDesired *out_desired
)
{
    MeisoCompositionMediaDesired candidate = {0};

    if (payload == NULL || out_enabled == NULL || out_desired == NULL ||
        payload_size != 40U || payload[0] > 1U ||
        !meiso_composition_edge_trace_private_bytes_zero(payload + 1U, 7U)) {
        return false;
    }
    if (payload[0] == 0U) {
        if (!meiso_composition_edge_trace_private_bytes_zero(payload + 8U,
                              MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE)) {
            return false;
        }
    } else if (meiso_composition_media_desired_decode(
                   (MeisoByteView){
                       payload + 8U,
                       MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE
                   },
                   &candidate
               ) != MEISO_COMPOSITION_PROFILE_OK) {
        return false;
    }
    *out_enabled = payload[0] != 0U;
    *out_desired = candidate;
    return true;
}

bool meiso_composition_edge_trace_private_decode_frame_record(
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
        payload_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_FRAME_PAYLOAD || payload[1U] != 0U ||
        !meiso_composition_edge_trace_private_bytes_zero(payload + 12U, 4U) ||
        !meiso_composition_edge_trace_private_bytes_zero(payload + 49U, 7U) ||
        !meiso_composition_edge_trace_private_bytes_zero(payload + 108U, 4U)) {
        return false;
    }
    unit_size = meiso_composition_edge_trace_private_get_u32(payload + 104U);
    if ((size_t)unit_size !=
        payload_size - MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.source = payload[0U];
    candidate.flags = meiso_composition_edge_trace_private_get_u16(payload + 2U);
    candidate.pixel_format = meiso_composition_edge_trace_private_get_u16(payload + 4U);
    candidate.codec = meiso_composition_edge_trace_private_get_u16(payload + 6U);
    candidate.width = meiso_composition_edge_trace_private_get_u16(payload + 8U);
    candidate.height = meiso_composition_edge_trace_private_get_u16(payload + 10U);
    candidate.media_stream_epoch = meiso_composition_edge_trace_private_get_u64(payload + 16U);
    candidate.frame_sequence = meiso_composition_edge_trace_private_get_u64(payload + 24U);
    candidate.source_time.clock_epoch = meiso_composition_edge_trace_private_get_u64(payload + 32U);
    candidate.source_time.monotonic_ns = meiso_composition_edge_trace_private_get_u64(payload + 40U);
    candidate.applied_scene_owner.role = payload[48U];
    candidate.applied_scene_owner.incarnation = meiso_composition_edge_trace_private_get_u64(payload + 56U);
    candidate.applied_scene_state_version = meiso_composition_edge_trace_private_get_u64(payload + 64U);
    candidate.active_plan_generation = meiso_composition_edge_trace_private_get_u64(payload + 72U);
    candidate.world_generation = meiso_composition_edge_trace_private_get_u64(payload + 80U);
    candidate.dependency_sequence = meiso_composition_edge_trace_private_get_u64(payload + 88U);
    candidate.producer_dropped_frames = meiso_composition_edge_trace_private_get_u64(payload + 96U);
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
        !meiso_composition_edge_trace_private_frame_scene_valid(
            candidate.source, candidate.applied_scene_owner,
            candidate.applied_scene_state_version,
            candidate.active_plan_generation, candidate.world_generation,
            candidate.dependency_sequence
        ) ||
        !meiso_composition_edge_trace_private_frame_size_exact(candidate.pixel_format, candidate.width,
                                candidate.height, unit_size)) {
        return false;
    }
    *out_frame = candidate;
    return true;
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_private_semantic_apply(
    MeisoCompositionEdgeTraceSemanticState *state,
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
    MeisoCompositionEdgeTraceMediaState *route;
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
        if (!meiso_composition_edge_trace_private_decode_wifi_record(payload, payload_size, &enabled, &wifi)) {
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
        if (!meiso_composition_edge_trace_private_decode_media_record(payload, payload_size, &enabled,
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
                meiso_composition_edge_trace_private_get_u64(payload), meiso_composition_edge_trace_private_get_u64(payload + 8U)
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
        if (!meiso_composition_edge_trace_private_decode_frame_record(payload, payload_size,
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
            !meiso_composition_edge_trace_private_bytes_zero(payload + 1U, 7U)) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        state->terminated = true;
        return MEISO_COMPOSITION_EDGE_TRACE_OK;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG:
    default:
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
}
