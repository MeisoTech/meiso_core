#include "lab_protocol.h"

#include <limits.h>
#include <string.h>

enum {
    LAB_DESCRIPTOR_WIRE_SIZE = 44U,
    LAB_CONTENT_ID_WIRE_SIZE = 36U,
    LAB_SCOPE_WIRE_SIZE = 16U,
    LAB_RESOURCE_ID_WIRE_SIZE = 16U,
    LAB_ACTION_ID_WIRE_SIZE = 16U,
    LAB_SCHEMA_REF_WIRE_SIZE = 20U,
    LAB_REGISTRY_TOTAL_SIZE = 196U,
    LAB_REGISTRY_ACCEPT_TOTAL_SIZE = 20U,
    LAB_STATE_ACTUAL_TOTAL_SIZE = 80U,
    LAB_STATE_ACCEPT_TOTAL_SIZE = 56U,
    LAB_STREAM_METRICS_TOTAL_SIZE = 160U,
    LAB_ACTION_RESET_TOTAL_SIZE = 104U,
    LAB_ACTION_DELIVERY_ACCEPT_TOTAL_SIZE = 64U,
    LAB_ACTION_RESULT_TOTAL_SIZE = 80U,
    LAB_BLOB_ACCEPT_TOTAL_SIZE = 68U,
    LAB_CLOSE_TOTAL_SIZE = 20U
};

enum {
    EDGE_STATE_INTERFACE_PRESENT = 1U,
    EDGE_STATE_CARRIER_PRESENT = 2U,
    EDGE_STATE_TEMPERATURE_PRESENT = 8U,
    EDGE_STATE_KNOWN_FLAGS = 11U,
    TELEMETRY_SIGNAL_PRESENT = 1U,
    TELEMETRY_TEMPERATURE_PRESENT = 2U,
    TELEMETRY_KNOWN_FLAGS = 3U
};

static const uint8_t LAB_MANIFEST[] =
    "profile=four-kind-lab-v0\n"
    "endpoint=edge-observation\n"
    "sources=wifi-network-telemetry,k230-on-die-temperature\n"
    "camera=unavailable\n"
    "imu=unavailable\n"
    "state=EdgeState:interface,carrier,temperature,window\n"
    "stream=TelemetrySample:elapsed,network-counters,optional-signal-mbm,corrected-millic,uncertainty-millic\n"
    "action=telemetry.reset_window\n"
    "blob=edge.capability.manifest\n"
    "sensitive=ssid,ip,mac,credentials-excluded\n";

static const MeisoContentId LAB_MANIFEST_CONTENT_ID = {
    MEISO_CONTENT_ALGORITHM_SHA256,
    MEISO_SHA256_DIGEST_SIZE,
    {
        0x79U, 0x32U, 0xcdU, 0x21U, 0x13U, 0x1bU, 0x0eU, 0xecU,
        0x09U, 0x53U, 0x08U, 0x2cU, 0xd5U, 0x37U, 0x91U, 0x5cU,
        0x89U, 0xc2U, 0x32U, 0x7eU, 0xbbU, 0x1dU, 0x6cU, 0x70U,
        0x77U, 0xabU, 0x88U, 0xacU, 0x6aU, 0x6fU, 0xc9U, 0xe8U
    }
};

static const MeisoResourceDescriptor LAB_DESCRIPTORS[MEISO_LAB_DESCRIPTOR_COUNT] = {
    {
        {UINT64_C(0x4d344c315f524553), UINT64_C(1)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d344c315f534348), UINT64_C(1)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_LAB_EDGE_STATE_WIRE_SIZE
    },
    {
        {UINT64_C(0x4d344c315f524553), UINT64_C(2)},
        MEISO_RESOURCE_KIND_STREAM,
        {{UINT64_C(0x4d344c315f534348), UINT64_C(2)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE
    },
    {
        {UINT64_C(0x4d344c315f524553), UINT64_C(3)},
        MEISO_RESOURCE_KIND_ACTION,
        {{UINT64_C(0x4d344c315f534348), UINT64_C(3)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        8U
    },
    {
        {UINT64_C(0x4d344c315f524553), UINT64_C(4)},
        MEISO_RESOURCE_KIND_BLOB,
        {{UINT64_C(0x4d344c315f534348), UINT64_C(4)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        sizeof(LAB_MANIFEST) - 1U
    }
};

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static void write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8U);
    destination[1] = (uint8_t)value;
}

static void write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static void write_u64(uint8_t *destination, uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56U);
    destination[1] = (uint8_t)(value >> 48U);
    destination[2] = (uint8_t)(value >> 40U);
    destination[3] = (uint8_t)(value >> 32U);
    destination[4] = (uint8_t)(value >> 24U);
    destination[5] = (uint8_t)(value >> 16U);
    destination[6] = (uint8_t)(value >> 8U);
    destination[7] = (uint8_t)value;
}

static uint16_t read_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] << 8U) |
        (uint16_t)source[1];
}

static uint32_t read_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

static uint64_t read_u64(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56U) |
        ((uint64_t)source[1] << 48U) |
        ((uint64_t)source[2] << 40U) |
        ((uint64_t)source[3] << 32U) |
        ((uint64_t)source[4] << 24U) |
        ((uint64_t)source[5] << 16U) |
        ((uint64_t)source[6] << 8U) |
        (uint64_t)source[7];
}

static uint32_t signed_to_u32(int32_t value)
{
    if (value >= 0) {
        return (uint32_t)value;
    }
    return UINT32_MAX - (uint32_t)(-(value + 1));
}

static int32_t u32_to_signed(uint32_t value)
{
    if (value <= (uint32_t)INT32_MAX) {
        return (int32_t)value;
    }
    if (value == UINT32_C(0x80000000)) {
        return INT32_MIN;
    }
    return -(int32_t)(UINT32_MAX - value + 1U);
}

static void write_resource_id(uint8_t *destination, MeisoResourceId value)
{
    write_u64(destination, value.high);
    write_u64(destination + 8U, value.low);
}

static MeisoResourceId read_resource_id(const uint8_t *source)
{
    const MeisoResourceId value = {
        read_u64(source), read_u64(source + 8U)
    };
    return value;
}

static void write_schema_ref(uint8_t *destination, MeisoSchemaRef value)
{
    write_u64(destination, value.id.high);
    write_u64(destination + 8U, value.id.low);
    write_u32(destination + 16U, value.version);
}

static MeisoSchemaRef read_schema_ref(const uint8_t *source)
{
    const MeisoSchemaRef value = {
        {read_u64(source), read_u64(source + 8U)},
        read_u32(source + 16U)
    };
    return value;
}

static void write_action_id(uint8_t *destination, MeisoActionId value)
{
    write_u64(destination, value.high);
    write_u64(destination + 8U, value.low);
}

static MeisoActionId read_action_id(const uint8_t *source)
{
    const MeisoActionId value = {
        read_u64(source), read_u64(source + 8U)
    };
    return value;
}

static void write_scope(uint8_t *destination, MeisoOwnerScope value)
{
    memset(destination, 0, LAB_SCOPE_WIRE_SIZE);
    destination[0] = value.role;
    write_u64(destination + 8U, value.incarnation);
}

static bool read_scope(const uint8_t *source, MeisoOwnerScope *out_scope)
{
    MeisoOwnerScope value;
    size_t index;

    for (index = 1U; index < 8U; ++index) {
        if (source[index] != 0U) {
            return false;
        }
    }
    value.role = source[0];
    value.incarnation = read_u64(source + 8U);
    if (!meiso_owner_scope_valid(value)) {
        return false;
    }
    *out_scope = value;
    return true;
}

static void write_content_id(uint8_t *destination, const MeisoContentId *value)
{
    write_u16(destination, value->algorithm);
    destination[2] = value->digest_size;
    destination[3] = 0U;
    memmove(destination + 4U, value->digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool read_content_id(const uint8_t *source, MeisoContentId *out_value)
{
    MeisoContentId value;

    memset(&value, 0, sizeof(value));
    value.algorithm = read_u16(source);
    value.digest_size = source[2];
    if (source[3] != 0U ||
        value.algorithm != MEISO_CONTENT_ALGORITHM_SHA256 ||
        value.digest_size != MEISO_SHA256_DIGEST_SIZE) {
        return false;
    }
    memmove(value.digest, source + 4U, MEISO_SHA256_DIGEST_SIZE);
    if (!meiso_content_id_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
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

static bool edge_state_valid(const MeisoLabEdgeState *value)
{
    if (value == NULL || value->window_id == 0U) {
        return false;
    }
    if (!value->temperature_present) {
        return value->corrected_millic == 0 && value->uncertainty_millic == 0U;
    }
    return value->corrected_millic >= MEISO_LAB_TEMPERATURE_MIN_MILLIC &&
        value->corrected_millic <= MEISO_LAB_TEMPERATURE_MAX_MILLIC &&
        value->uncertainty_millic ==
            MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
}

static bool telemetry_sample_valid(const MeisoLabTelemetrySample *value)
{
    if (value == NULL || value->window_id == 0U) {
        return false;
    }
    if ((!value->signal_present && value->signal_mbm != 0) ||
        (value->signal_present &&
         (value->signal_mbm < MEISO_LAB_SIGNAL_MIN_MBM ||
          value->signal_mbm > MEISO_LAB_SIGNAL_MAX_MBM))) {
        return false;
    }
    if (!value->temperature_present) {
        return value->corrected_millic == 0 && value->uncertainty_millic == 0U;
    }
    return value->corrected_millic >= MEISO_LAB_TEMPERATURE_MIN_MILLIC &&
        value->corrected_millic <= MEISO_LAB_TEMPERATURE_MAX_MILLIC &&
        value->uncertainty_millic ==
            MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
}

static void encode_edge_state_unchecked(
    const MeisoLabEdgeState *value,
    uint8_t *output
)
{
    uint8_t flags = 0U;

    flags = (uint8_t)(flags |
        (value->interface_present ? EDGE_STATE_INTERFACE_PRESENT : 0U));
    flags = (uint8_t)(flags |
        (value->carrier_present ? EDGE_STATE_CARRIER_PRESENT : 0U));
    flags = (uint8_t)(flags |
        (value->temperature_present ? EDGE_STATE_TEMPERATURE_PRESENT : 0U));
    write_u64(output, value->window_id);
    output[8] = flags;
    memset(output + 9U, 0, 3U);
    write_u32(output + 12U, signed_to_u32(value->corrected_millic));
    write_u32(output + 16U, value->uncertainty_millic);
    memset(output + 20U, 0, 4U);
}

static bool decode_edge_state_unchecked(
    const uint8_t *input,
    MeisoLabEdgeState *out_value
)
{
    MeisoLabEdgeState value;
    const uint8_t flags = input[8];

    if ((flags & (uint8_t)~EDGE_STATE_KNOWN_FLAGS) != 0U ||
        !bytes_zero(input + 9U, 3U) || !bytes_zero(input + 20U, 4U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.window_id = read_u64(input);
    value.interface_present = (flags & EDGE_STATE_INTERFACE_PRESENT) != 0U;
    value.carrier_present = (flags & EDGE_STATE_CARRIER_PRESENT) != 0U;
    value.temperature_present =
        (flags & EDGE_STATE_TEMPERATURE_PRESENT) != 0U;
    value.corrected_millic = u32_to_signed(read_u32(input + 12U));
    value.uncertainty_millic = read_u32(input + 16U);
    if (!edge_state_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

static void encode_telemetry_unchecked(
    const MeisoLabTelemetrySample *value,
    uint8_t *output
)
{
    uint8_t flags = 0U;

    flags = (uint8_t)(flags |
        (value->signal_present ? TELEMETRY_SIGNAL_PRESENT : 0U));
    flags = (uint8_t)(flags |
        (value->temperature_present ? TELEMETRY_TEMPERATURE_PRESENT : 0U));
    write_u64(output, value->window_id);
    write_u64(output + 8U, value->elapsed_ns);
    write_u64(output + 16U, value->rx_bytes);
    write_u64(output + 24U, value->tx_bytes);
    write_u64(output + 32U, value->rx_packets);
    write_u64(output + 40U, value->tx_packets);
    write_u64(output + 48U, value->rx_dropped);
    write_u64(output + 56U, value->tx_dropped);
    write_u32(output + 64U, signed_to_u32(value->signal_mbm));
    write_u32(output + 68U, signed_to_u32(value->corrected_millic));
    write_u32(output + 72U, value->uncertainty_millic);
    output[76] = flags;
    memset(output + 77U, 0, 3U);
}

static bool decode_telemetry_unchecked(
    const uint8_t *input,
    MeisoLabTelemetrySample *out_value
)
{
    MeisoLabTelemetrySample value;
    const uint8_t flags = input[76];

    if ((flags & (uint8_t)~TELEMETRY_KNOWN_FLAGS) != 0U ||
        !bytes_zero(input + 77U, 3U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.window_id = read_u64(input);
    value.elapsed_ns = read_u64(input + 8U);
    value.rx_bytes = read_u64(input + 16U);
    value.tx_bytes = read_u64(input + 24U);
    value.rx_packets = read_u64(input + 32U);
    value.tx_packets = read_u64(input + 40U);
    value.rx_dropped = read_u64(input + 48U);
    value.tx_dropped = read_u64(input + 56U);
    value.signal_mbm = u32_to_signed(read_u32(input + 64U));
    value.corrected_millic = u32_to_signed(read_u32(input + 68U));
    value.uncertainty_millic = read_u32(input + 72U);
    value.signal_present = (flags & TELEMETRY_SIGNAL_PRESENT) != 0U;
    value.temperature_present =
        (flags & TELEMETRY_TEMPERATURE_PRESENT) != 0U;
    if (!telemetry_sample_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

const MeisoResourceDescriptor *meiso_lab_descriptors(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = MEISO_LAB_DESCRIPTOR_COUNT;
    }
    return LAB_DESCRIPTORS;
}

const MeisoResourceDescriptor *meiso_lab_descriptor(MeisoResourceKind kind)
{
    if (kind < MEISO_RESOURCE_KIND_STATE || kind > MEISO_RESOURCE_KIND_BLOB) {
        return NULL;
    }
    return &LAB_DESCRIPTORS[(size_t)kind - 1U];
}

const uint8_t *meiso_lab_capability_manifest(size_t *out_size)
{
    if (out_size != NULL) {
        *out_size = sizeof(LAB_MANIFEST) - 1U;
    }
    return LAB_MANIFEST;
}

const MeisoContentId *meiso_lab_capability_manifest_content_id(void)
{
    return &LAB_MANIFEST_CONTENT_ID;
}

MeisoLabCodecStatus meiso_lab_edge_state_encode(
    const MeisoLabEdgeState *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    if (value == NULL || out_size == NULL ||
        !meiso_mutable_buffer_valid(output) ||
        ranges_overlap(value, sizeof(*value), output.data, output.capacity) ||
        ranges_overlap(out_size, sizeof(*out_size), output.data, output.capacity)) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (!edge_state_valid(value)) {
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    if (output.capacity < MEISO_LAB_EDGE_STATE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_CAPACITY;
    }
    encode_edge_state_unchecked(value, output.data);
    *out_size = MEISO_LAB_EDGE_STATE_WIRE_SIZE;
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus meiso_lab_edge_state_decode(
    MeisoByteView input,
    MeisoLabEdgeState *out_value
)
{
    MeisoLabEdgeState value;

    if (out_value == NULL || !meiso_byte_view_valid(input) ||
        ranges_overlap(input.data, input.size, out_value, sizeof(*out_value))) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_LAB_EDGE_STATE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (input.size != MEISO_LAB_EDGE_STATE_WIRE_SIZE ||
        !decode_edge_state_unchecked(input.data, &value)) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus meiso_lab_telemetry_sample_encode(
    const MeisoLabTelemetrySample *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    if (value == NULL || out_size == NULL ||
        !meiso_mutable_buffer_valid(output) ||
        ranges_overlap(value, sizeof(*value), output.data, output.capacity) ||
        ranges_overlap(out_size, sizeof(*out_size), output.data, output.capacity)) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (!telemetry_sample_valid(value)) {
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    if (output.capacity < MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_CAPACITY;
    }
    encode_telemetry_unchecked(value, output.data);
    *out_size = MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE;
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus meiso_lab_telemetry_sample_decode(
    MeisoByteView input,
    MeisoLabTelemetrySample *out_value
)
{
    MeisoLabTelemetrySample value;

    if (out_value == NULL || !meiso_byte_view_valid(input) ||
        ranges_overlap(input.data, input.size, out_value, sizeof(*out_value))) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (input.size != MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE ||
        !decode_telemetry_unchecked(input.data, &value)) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_LAB_CODEC_OK;
}

static bool edge_scope_valid(MeisoOwnerScope value)
{
    return meiso_owner_scope_valid(value) &&
        value.role == MEISO_OWNER_ROLE_EDGE;
}

static bool action_result_valid(const MeisoLabActionResult *value)
{
    if (value == NULL || !meiso_action_id_valid(value->id) ||
        !edge_scope_valid(value->executor)) {
        return false;
    }
    if (value->state == MEISO_ACTION_STATE_SUCCEEDED) {
        return value->new_window_id != 0U;
    }
    if (value->new_window_id != 0U) {
        return false;
    }
    return value->state == MEISO_ACTION_STATE_ACCEPTED ||
        value->state == MEISO_ACTION_STATE_FAILED ||
        value->state == MEISO_ACTION_STATE_REJECTED ||
        value->state == MEISO_ACTION_STATE_EXPIRED ||
        value->state == MEISO_ACTION_STATE_INDETERMINATE;
}

static bool manifest_exact(MeisoLabBlobManifest value)
{
    return meiso_content_id_equal(
            &value.content_id, &LAB_MANIFEST_CONTENT_ID
        ) && value.content.size == sizeof(LAB_MANIFEST) - 1U &&
        value.content.data != NULL && memcmp(
            value.content.data, LAB_MANIFEST, sizeof(LAB_MANIFEST) - 1U
        ) == 0;
}

static bool message_size_and_valid(
    const MeisoLabMessage *message,
    size_t *out_size
)
{
    size_t size;

    if (message == NULL || out_size == NULL ||
        message->registry_generation != MEISO_LAB_REGISTRY_GENERATION) {
        return false;
    }
    switch (message->type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        size = LAB_REGISTRY_TOTAL_SIZE;
        if (message->body.registry.descriptor_count !=
            MEISO_LAB_DESCRIPTOR_COUNT) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        size = LAB_REGISTRY_ACCEPT_TOTAL_SIZE;
        if (message->body.registry_accept.descriptor_count !=
            MEISO_LAB_DESCRIPTOR_COUNT) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        size = LAB_STATE_ACTUAL_TOTAL_SIZE;
        if (!edge_scope_valid(message->body.state_actual.owner) ||
            message->body.state_actual.version == 0U ||
            !edge_state_valid(&message->body.state_actual.value)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        size = LAB_STATE_ACCEPT_TOTAL_SIZE;
        if (!edge_scope_valid(message->body.state_accept.owner) ||
            message->body.state_accept.version == 0U) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        size = LAB_STREAM_METRICS_TOTAL_SIZE;
        if (!edge_scope_valid(message->body.stream_metrics.producer) ||
            message->body.stream_metrics.epoch == 0U ||
            message->body.stream_metrics.sequence == 0U ||
            !meiso_time_point_valid(message->body.stream_metrics.source_time) ||
            !telemetry_sample_valid(&message->body.stream_metrics.value)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        size = LAB_ACTION_RESET_TOTAL_SIZE;
        if (!meiso_action_id_valid(message->body.action_reset.id) ||
            !edge_scope_valid(message->body.action_reset.executor)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        size = LAB_ACTION_DELIVERY_ACCEPT_TOTAL_SIZE;
        if (!meiso_action_id_valid(
                message->body.action_delivery_accept.id
            ) || !edge_scope_valid(
                message->body.action_delivery_accept.executor
            )) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        size = LAB_ACTION_RESULT_TOTAL_SIZE;
        if (!action_result_valid(&message->body.action_result)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        size = MEISO_LAB_WIRE_HEADER_SIZE + 4U +
            LAB_RESOURCE_ID_WIRE_SIZE + LAB_CONTENT_ID_WIRE_SIZE + 4U +
            (sizeof(LAB_MANIFEST) - 1U);
        if (!manifest_exact(message->body.blob_manifest)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        size = LAB_BLOB_ACCEPT_TOTAL_SIZE;
        if (!meiso_content_id_equal(
                &message->body.blob_accept.content_id,
                &LAB_MANIFEST_CONTENT_ID
            )) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        size = LAB_CLOSE_TOTAL_SIZE;
        if (message->body.close.status != MEISO_LAB_CLOSE_PASS &&
            message->body.close.status != MEISO_LAB_CLOSE_FAIL) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (size > MEISO_LAB_MAX_MESSAGE_SIZE || size > UINT32_MAX) {
        return false;
    }
    *out_size = size;
    return true;
}

static void write_descriptor(
    uint8_t *destination,
    const MeisoResourceDescriptor *descriptor
)
{
    write_resource_id(destination, descriptor->id);
    destination[16] = descriptor->kind;
    destination[17] = descriptor->owner;
    destination[18] = 0U;
    destination[19] = 0U;
    write_schema_ref(destination + 20U, descriptor->schema);
    write_u32(destination + 40U, (uint32_t)descriptor->max_payload_size);
}

static bool read_descriptor(
    const uint8_t *source,
    MeisoResourceDescriptor *out_descriptor
)
{
    MeisoResourceDescriptor value;

    memset(&value, 0, sizeof(value));
    value.id = read_resource_id(source);
    value.kind = source[16];
    value.owner = source[17];
    if (source[18] != 0U || source[19] != 0U) {
        return false;
    }
    value.schema = read_schema_ref(source + 20U);
    value.max_payload_size = (size_t)read_u32(source + 40U);
    if (meiso_resource_descriptor_validate(&value) != MEISO_DESCRIPTOR_VALID) {
        return false;
    }
    *out_descriptor = value;
    return true;
}

static void write_message_header(
    uint8_t *output,
    MeisoLabMessageType type,
    size_t total_size
)
{
    output[0] = (uint8_t)'M';
    output[1] = (uint8_t)'4';
    output[2] = (uint8_t)'L';
    output[3] = (uint8_t)'1';
    output[4] = MEISO_LAB_WIRE_VERSION;
    output[5] = type;
    output[6] = 0U;
    output[7] = 0U;
    write_u32(output + 8U, (uint32_t)total_size);
    write_u32(output + MEISO_LAB_WIRE_HEADER_SIZE,
              MEISO_LAB_REGISTRY_GENERATION);
}

MeisoLabCodecStatus meiso_lab_message_encode(
    const MeisoLabMessage *message,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    const MeisoResourceDescriptor *state = &LAB_DESCRIPTORS[0];
    const MeisoResourceDescriptor *stream = &LAB_DESCRIPTORS[1];
    const MeisoResourceDescriptor *action = &LAB_DESCRIPTORS[2];
    const MeisoResourceDescriptor *blob = &LAB_DESCRIPTORS[3];
    size_t total_size;
    uint8_t *body;
    size_t index;

    if (message == NULL || out_size == NULL ||
        !meiso_mutable_buffer_valid(output) ||
        ranges_overlap(message, sizeof(*message), output.data, output.capacity) ||
        ranges_overlap(out_size, sizeof(*out_size), output.data, output.capacity)) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (!message_size_and_valid(message, &total_size)) {
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    if (output.capacity < total_size) {
        return MEISO_LAB_CODEC_CAPACITY;
    }
    write_message_header(output.data, message->type, total_size);
    body = output.data + MEISO_LAB_WIRE_HEADER_SIZE + 4U;

    switch (message->type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        write_u32(body, MEISO_LAB_DESCRIPTOR_COUNT);
        body += 4U;
        for (index = 0U; index < MEISO_LAB_DESCRIPTOR_COUNT; ++index) {
            write_descriptor(body, &LAB_DESCRIPTORS[index]);
            body += LAB_DESCRIPTOR_WIRE_SIZE;
        }
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        write_u32(body, MEISO_LAB_DESCRIPTOR_COUNT);
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        write_resource_id(body, state->id);
        write_scope(body + 16U, message->body.state_actual.owner);
        write_u64(body + 32U, message->body.state_actual.version);
        encode_edge_state_unchecked(
            &message->body.state_actual.value, body + 40U
        );
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        write_resource_id(body, state->id);
        write_scope(body + 16U, message->body.state_accept.owner);
        write_u64(body + 32U, message->body.state_accept.version);
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        write_resource_id(body, stream->id);
        write_scope(body + 16U, message->body.stream_metrics.producer);
        write_u64(body + 32U, message->body.stream_metrics.epoch);
        write_u64(body + 40U, message->body.stream_metrics.sequence);
        write_u64(
            body + 48U,
            message->body.stream_metrics.source_time.clock_epoch
        );
        write_u64(
            body + 56U,
            message->body.stream_metrics.source_time.monotonic_ns
        );
        encode_telemetry_unchecked(
            &message->body.stream_metrics.value, body + 64U
        );
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        write_resource_id(body, action->id);
        write_action_id(body + 16U, message->body.action_reset.id);
        write_scope(body + 32U, message->body.action_reset.executor);
        write_resource_id(body + 48U, stream->id);
        write_schema_ref(body + 64U, action->schema);
        body[84] = MEISO_ACTION_RETRY_NONE;
        memset(body + 85U, 0, 3U);
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        write_resource_id(body, action->id);
        write_action_id(
            body + 16U, message->body.action_delivery_accept.id
        );
        write_scope(
            body + 32U, message->body.action_delivery_accept.executor
        );
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        write_resource_id(body, action->id);
        write_action_id(body + 16U, message->body.action_result.id);
        write_scope(body + 32U, message->body.action_result.executor);
        body[48] = message->body.action_result.state;
        memset(body + 49U, 0, 7U);
        write_u64(body + 56U, message->body.action_result.new_window_id);
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        write_resource_id(body, blob->id);
        write_content_id(body + 16U, &LAB_MANIFEST_CONTENT_ID);
        write_u32(body + 52U, (uint32_t)(sizeof(LAB_MANIFEST) - 1U));
        memmove(body + 56U, LAB_MANIFEST, sizeof(LAB_MANIFEST) - 1U);
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        write_resource_id(body, blob->id);
        write_content_id(body + 16U, &LAB_MANIFEST_CONTENT_ID);
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        body[0] = message->body.close.status;
        memset(body + 1U, 0, 3U);
        break;
    default:
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    *out_size = total_size;
    return MEISO_LAB_CODEC_OK;
}

static bool fixed_resource_id(
    const uint8_t *source,
    MeisoResourceKind kind
)
{
    const MeisoResourceDescriptor *descriptor = meiso_lab_descriptor(kind);
    return descriptor != NULL && meiso_resource_id_equal(
        read_resource_id(source), descriptor->id
    );
}

static MeisoLabCodecStatus decode_header(
    MeisoByteView input,
    MeisoLabMessageType *out_type
)
{
    uint32_t declared_size;

    if (input.size < MEISO_LAB_WIRE_HEADER_SIZE) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (input.data[0] != (uint8_t)'M' || input.data[1] != (uint8_t)'4' ||
        input.data[2] != (uint8_t)'L' || input.data[3] != (uint8_t)'1' ||
        input.data[6] != 0U || input.data[7] != 0U) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    if (input.data[4] != MEISO_LAB_WIRE_VERSION) {
        return MEISO_LAB_CODEC_UNSUPPORTED;
    }
    if (input.data[5] < MEISO_LAB_MESSAGE_REGISTRY ||
        input.data[5] > MEISO_LAB_MESSAGE_CLOSE) {
        return MEISO_LAB_CODEC_UNSUPPORTED;
    }
    declared_size = read_u32(input.data + 8U);
    if (declared_size > MEISO_LAB_MAX_MESSAGE_SIZE ||
        declared_size < MEISO_LAB_WIRE_HEADER_SIZE) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    if ((size_t)declared_size > input.size) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if ((size_t)declared_size != input.size) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    *out_type = input.data[5];
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus meiso_lab_message_decode(
    MeisoByteView input,
    MeisoLabMessage *out_message
)
{
    MeisoLabMessage message;
    MeisoLabMessageType type;
    MeisoLabCodecStatus header_status;
    const uint8_t *body;
    MeisoResourceDescriptor descriptor;
    MeisoContentId content_id;
    size_t expected_size;
    size_t index;

    if (out_message == NULL || !meiso_byte_view_valid(input) ||
        ranges_overlap(input.data, input.size, out_message,
                       sizeof(*out_message))) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    header_status = decode_header(input, &type);
    if (header_status != MEISO_LAB_CODEC_OK) {
        return header_status;
    }
    if (input.size < MEISO_LAB_WIRE_HEADER_SIZE + 4U) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (read_u32(input.data + MEISO_LAB_WIRE_HEADER_SIZE) !=
        MEISO_LAB_REGISTRY_GENERATION) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    memset(&message, 0, sizeof(message));
    message.type = type;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    body = input.data + MEISO_LAB_WIRE_HEADER_SIZE + 4U;

    switch (type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        expected_size = LAB_REGISTRY_TOTAL_SIZE;
        if (input.size != expected_size ||
            read_u32(body) != MEISO_LAB_DESCRIPTOR_COUNT) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        body += 4U;
        for (index = 0U; index < MEISO_LAB_DESCRIPTOR_COUNT; ++index) {
            if (!read_descriptor(body, &descriptor) ||
                !meiso_resource_descriptor_equal(
                    &descriptor, &LAB_DESCRIPTORS[index]
                )) {
                return MEISO_LAB_CODEC_MALFORMED;
            }
            body += LAB_DESCRIPTOR_WIRE_SIZE;
        }
        message.body.registry.descriptor_count = MEISO_LAB_DESCRIPTOR_COUNT;
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        if (input.size != LAB_REGISTRY_ACCEPT_TOTAL_SIZE ||
            read_u32(body) != MEISO_LAB_DESCRIPTOR_COUNT) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.registry_accept.descriptor_count =
            MEISO_LAB_DESCRIPTOR_COUNT;
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        if (input.size != LAB_STATE_ACTUAL_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_STATE) ||
            !read_scope(body + 16U, &message.body.state_actual.owner) ||
            !edge_scope_valid(message.body.state_actual.owner)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.state_actual.version = read_u64(body + 32U);
        if (message.body.state_actual.version == 0U ||
            !decode_edge_state_unchecked(
                body + 40U, &message.body.state_actual.value
            )) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        if (input.size != LAB_STATE_ACCEPT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_STATE) ||
            !read_scope(body + 16U, &message.body.state_accept.owner) ||
            !edge_scope_valid(message.body.state_accept.owner)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.state_accept.version = read_u64(body + 32U);
        if (message.body.state_accept.version == 0U) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        if (input.size != LAB_STREAM_METRICS_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_STREAM) ||
            !read_scope(body + 16U, &message.body.stream_metrics.producer) ||
            !edge_scope_valid(message.body.stream_metrics.producer)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.stream_metrics.epoch = read_u64(body + 32U);
        message.body.stream_metrics.sequence = read_u64(body + 40U);
        message.body.stream_metrics.source_time.clock_epoch =
            read_u64(body + 48U);
        message.body.stream_metrics.source_time.monotonic_ns =
            read_u64(body + 56U);
        if (message.body.stream_metrics.epoch == 0U ||
            message.body.stream_metrics.sequence == 0U ||
            !meiso_time_point_valid(
                message.body.stream_metrics.source_time
            ) || !decode_telemetry_unchecked(
                body + 64U, &message.body.stream_metrics.value
            )) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        if (input.size != LAB_ACTION_RESET_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_ACTION) ||
            !fixed_resource_id(body + 48U, MEISO_RESOURCE_KIND_STREAM) ||
            !meiso_schema_ref_equal(
                read_schema_ref(body + 64U), LAB_DESCRIPTORS[2].schema
            ) || body[84] != MEISO_ACTION_RETRY_NONE ||
            !bytes_zero(body + 85U, 3U)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_reset.id = read_action_id(body + 16U);
        if (!read_scope(body + 32U, &message.body.action_reset.executor) ||
            !meiso_action_id_valid(message.body.action_reset.id) ||
            !edge_scope_valid(message.body.action_reset.executor)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        if (input.size != LAB_ACTION_DELIVERY_ACCEPT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_ACTION)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_delivery_accept.id = read_action_id(body + 16U);
        if (!read_scope(
                body + 32U,
                &message.body.action_delivery_accept.executor
            ) || !meiso_action_id_valid(
                message.body.action_delivery_accept.id
            ) || !edge_scope_valid(
                message.body.action_delivery_accept.executor
            )) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        if (input.size != LAB_ACTION_RESULT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_ACTION) ||
            !bytes_zero(body + 49U, 7U)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_result.id = read_action_id(body + 16U);
        if (!read_scope(body + 32U, &message.body.action_result.executor)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_result.state = body[48];
        message.body.action_result.new_window_id = read_u64(body + 56U);
        if (!action_result_valid(&message.body.action_result)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        expected_size = MEISO_LAB_WIRE_HEADER_SIZE + 4U +
            LAB_RESOURCE_ID_WIRE_SIZE + LAB_CONTENT_ID_WIRE_SIZE + 4U +
            (sizeof(LAB_MANIFEST) - 1U);
        if (input.size != expected_size ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_BLOB) ||
            !read_content_id(body + 16U, &content_id) ||
            !meiso_content_id_equal(
                &content_id, &LAB_MANIFEST_CONTENT_ID
            ) || read_u32(body + 52U) != sizeof(LAB_MANIFEST) - 1U ||
            memcmp(
                body + 56U, LAB_MANIFEST, sizeof(LAB_MANIFEST) - 1U
            ) != 0) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.blob_manifest.content_id = content_id;
        message.body.blob_manifest.content = (MeisoByteView){
            body + 56U, sizeof(LAB_MANIFEST) - 1U
        };
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        if (input.size != LAB_BLOB_ACCEPT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_BLOB) ||
            !read_content_id(body + 16U, &content_id) ||
            !meiso_content_id_equal(
                &content_id, &LAB_MANIFEST_CONTENT_ID
            )) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.blob_accept.content_id = content_id;
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        if (input.size != LAB_CLOSE_TOTAL_SIZE ||
            !bytes_zero(body + 1U, 3U) ||
            (body[0] != MEISO_LAB_CLOSE_PASS &&
             body[0] != MEISO_LAB_CLOSE_FAIL)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.close.status = body[0];
        break;
    default:
        return MEISO_LAB_CODEC_UNSUPPORTED;
    }
    *out_message = message;
    return MEISO_LAB_CODEC_OK;
}

void meiso_lab_order_init(MeisoLabOrder *order)
{
    if (order != NULL) {
        memset(order, 0, sizeof(*order));
    }
}

static bool action_identity_matches(
    const MeisoLabOrder *order,
    MeisoActionId id,
    MeisoOwnerScope executor
)
{
    return meiso_action_id_equal(order->action_id, id) &&
        meiso_owner_scope_equal(order->action_executor, executor);
}

MeisoLabOrderStatus meiso_lab_order_advance(
    MeisoLabOrder *order,
    MeisoLabSender sender,
    const MeisoLabMessage *message
)
{
    size_t ignored_size;
    bool matches = false;

    if (order == NULL || message == NULL ||
        (sender != MEISO_LAB_SENDER_HOST &&
         sender != MEISO_LAB_SENDER_EDGE)) {
        return MEISO_LAB_ORDER_INVALID_ARGUMENT;
    }
    if (order->terminal != 0U) {
        return MEISO_LAB_ORDER_ALREADY_TERMINAL;
    }
    if (!message_size_and_valid(message, &ignored_size)) {
        order->terminal = 2U;
        return MEISO_LAB_ORDER_UNEXPECTED;
    }
    switch (order->step) {
    case 0U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_REGISTRY;
        break;
    case 1U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
            message->type == MEISO_LAB_MESSAGE_REGISTRY_ACCEPT;
        break;
    case 2U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_STATE_ACTUAL;
        if (matches) {
            order->state_owner = message->body.state_actual.owner;
            order->state_version = message->body.state_actual.version;
            order->window_id = message->body.state_actual.value.window_id;
        }
        break;
    case 3U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
            message->type == MEISO_LAB_MESSAGE_STATE_ACCEPT &&
            meiso_owner_scope_equal(
                order->state_owner, message->body.state_accept.owner
            ) && order->state_version == message->body.state_accept.version;
        break;
    case 4U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_STREAM_METRICS &&
            meiso_owner_scope_equal(
                order->state_owner,
                message->body.stream_metrics.producer
            ) && order->window_id ==
                message->body.stream_metrics.value.window_id;
        break;
    case 5U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
            message->type == MEISO_LAB_MESSAGE_ACTION_RESET &&
            meiso_owner_scope_equal(
                order->state_owner,
                message->body.action_reset.executor
            );
        if (matches) {
            order->action_id = message->body.action_reset.id;
            order->action_executor = message->body.action_reset.executor;
        }
        break;
    case 6U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT &&
            action_identity_matches(
                order,
                message->body.action_delivery_accept.id,
                message->body.action_delivery_accept.executor
            );
        break;
    case 7U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_ACTION_RESULT &&
            message->body.action_result.state ==
                MEISO_ACTION_STATE_ACCEPTED &&
            action_identity_matches(
                order,
                message->body.action_result.id,
                message->body.action_result.executor
            );
        break;
    case 8U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_ACTION_RESULT &&
            message->body.action_result.state ==
                MEISO_ACTION_STATE_SUCCEEDED &&
            message->body.action_result.new_window_id != order->window_id &&
            action_identity_matches(
                order,
                message->body.action_result.id,
                message->body.action_result.executor
            );
        break;
    case 9U:
        matches = sender == MEISO_LAB_SENDER_EDGE &&
            message->type == MEISO_LAB_MESSAGE_BLOB_MANIFEST;
        break;
    case 10U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
            message->type == MEISO_LAB_MESSAGE_BLOB_ACCEPT;
        break;
    case 11U:
        matches = sender == MEISO_LAB_SENDER_HOST &&
            message->type == MEISO_LAB_MESSAGE_CLOSE &&
            message->body.close.status == MEISO_LAB_CLOSE_PASS;
        break;
    default:
        break;
    }

    if (!matches) {
        order->terminal = 2U;
        return MEISO_LAB_ORDER_UNEXPECTED;
    }
    order->step = (uint8_t)(order->step + 1U);
    if (order->step == 12U) {
        order->terminal = 1U;
        return MEISO_LAB_ORDER_COMPLETE;
    }
    return MEISO_LAB_ORDER_ADVANCED;
}
