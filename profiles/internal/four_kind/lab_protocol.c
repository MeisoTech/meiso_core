#include "meiso_four_kind_lab_protocol_private.h"

/* Catalog/value codec owns immutable lab data and fixed-size value records. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

const uint8_t meiso_four_kind_lab_protocol_private_manifest[] =
    "profile=four-kind-lab-v0\n"
    "endpoint=edge-observation\n"
    "sources=wifi-network-telemetry,k230-on-die-temperature\n"
    "camera=unavailable\n"
    "imu=unavailable\n"
    "state=EdgeState:interface,carrier,temperature,window\n"
    "stream=TelemetrySample:elapsed,network-counters,optional-signal-mbm,"
    "corrected-millic,uncertainty-millic\n"
    "action=telemetry.reset_window\n"
    "blob=edge.capability.manifest\n"
    "sensitive=ssid,ip,mac,credentials-excluded\n";

const MeisoContentId meiso_four_kind_lab_protocol_private_manifest_content_id =
    {MEISO_CONTENT_ALGORITHM_SHA256,
     MEISO_SHA256_DIGEST_SIZE,
     {0x79U, 0x32U, 0xcdU, 0x21U, 0x13U, 0x1bU, 0x0eU, 0xecU,
      0x09U, 0x53U, 0x08U, 0x2cU, 0xd5U, 0x37U, 0x91U, 0x5cU,
      0x89U, 0xc2U, 0x32U, 0x7eU, 0xbbU, 0x1dU, 0x6cU, 0x70U,
      0x77U, 0xabU, 0x88U, 0xacU, 0x6aU, 0x6fU, 0xc9U, 0xe8U}};

const MeisoResourceDescriptor meiso_four_kind_lab_protocol_private_descriptors
    [MEISO_LAB_DESCRIPTOR_COUNT] = {
        {{UINT64_C(0x4d344c315f524553), UINT64_C(1)},
         MEISO_RESOURCE_KIND_STATE,
         {{UINT64_C(0x4d344c315f534348), UINT64_C(1)}, 1U},
         MEISO_OWNER_ROLE_EDGE,
         MEISO_LAB_EDGE_STATE_WIRE_SIZE},
        {{UINT64_C(0x4d344c315f524553), UINT64_C(2)},
         MEISO_RESOURCE_KIND_STREAM,
         {{UINT64_C(0x4d344c315f534348), UINT64_C(2)}, 1U},
         MEISO_OWNER_ROLE_EDGE,
         MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE},
        {{UINT64_C(0x4d344c315f524553), UINT64_C(3)},
         MEISO_RESOURCE_KIND_ACTION,
         {{UINT64_C(0x4d344c315f534348), UINT64_C(3)}, 1U},
         MEISO_OWNER_ROLE_EDGE,
         8U},
        {{UINT64_C(0x4d344c315f524553), UINT64_C(4)},
         MEISO_RESOURCE_KIND_BLOB,
         {{UINT64_C(0x4d344c315f534348), UINT64_C(4)}, 1U},
         MEISO_OWNER_ROLE_EDGE,
         sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U}};

bool meiso_four_kind_lab_protocol_private_ranges_overlap(const void *lhs,
                                                         size_t lhs_size,
                                                         const void *rhs,
                                                         size_t rhs_size)
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

void meiso_four_kind_lab_protocol_private_write_u16(uint8_t *destination,
                                                    uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8U);
    destination[1] = (uint8_t)value;
}

void meiso_four_kind_lab_protocol_private_write_u32(uint8_t *destination,
                                                    uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

void meiso_four_kind_lab_protocol_private_write_u64(uint8_t *destination,
                                                    uint64_t value)
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

uint16_t meiso_four_kind_lab_protocol_private_read_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] << 8U) | (uint16_t)source[1];
}

uint32_t meiso_four_kind_lab_protocol_private_read_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) | ((uint32_t)source[1] << 16U) |
           ((uint32_t)source[2] << 8U) | (uint32_t)source[3];
}

uint64_t meiso_four_kind_lab_protocol_private_read_u64(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56U) | ((uint64_t)source[1] << 48U) |
           ((uint64_t)source[2] << 40U) | ((uint64_t)source[3] << 32U) |
           ((uint64_t)source[4] << 24U) | ((uint64_t)source[5] << 16U) |
           ((uint64_t)source[6] << 8U) | (uint64_t)source[7];
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

void meiso_four_kind_lab_protocol_private_write_resource_id(
    uint8_t *destination, MeisoResourceId value)
{
    meiso_four_kind_lab_protocol_private_write_u64(destination, value.high);
    meiso_four_kind_lab_protocol_private_write_u64(destination + 8U, value.low);
}

MeisoResourceId
meiso_four_kind_lab_protocol_private_read_resource_id(const uint8_t *source)
{
    const MeisoResourceId value = {
        meiso_four_kind_lab_protocol_private_read_u64(source),
        meiso_four_kind_lab_protocol_private_read_u64(source + 8U)};
    return value;
}

void meiso_four_kind_lab_protocol_private_write_schema_ref(uint8_t *destination,
                                                           MeisoSchemaRef value)
{
    meiso_four_kind_lab_protocol_private_write_u64(destination, value.id.high);
    meiso_four_kind_lab_protocol_private_write_u64(destination + 8U,
                                                   value.id.low);
    meiso_four_kind_lab_protocol_private_write_u32(destination + 16U,
                                                   value.version);
}

MeisoSchemaRef
meiso_four_kind_lab_protocol_private_read_schema_ref(const uint8_t *source)
{
    const MeisoSchemaRef value = {
        {meiso_four_kind_lab_protocol_private_read_u64(source),
         meiso_four_kind_lab_protocol_private_read_u64(source + 8U)},
        meiso_four_kind_lab_protocol_private_read_u32(source + 16U)};
    return value;
}

void meiso_four_kind_lab_protocol_private_write_action_id(uint8_t *destination,
                                                          MeisoActionId value)
{
    meiso_four_kind_lab_protocol_private_write_u64(destination, value.high);
    meiso_four_kind_lab_protocol_private_write_u64(destination + 8U, value.low);
}

MeisoActionId
meiso_four_kind_lab_protocol_private_read_action_id(const uint8_t *source)
{
    const MeisoActionId value = {
        meiso_four_kind_lab_protocol_private_read_u64(source),
        meiso_four_kind_lab_protocol_private_read_u64(source + 8U)};
    return value;
}

void meiso_four_kind_lab_protocol_private_write_scope(uint8_t *destination,
                                                      MeisoOwnerScope value)
{
    memset(destination, 0, MEISO_FOUR_KIND_LAB_SCOPE_WIRE_SIZE);
    destination[0] = value.role;
    meiso_four_kind_lab_protocol_private_write_u64(destination + 8U,
                                                   value.incarnation);
}

bool meiso_four_kind_lab_protocol_private_read_scope(const uint8_t *source,
                                                     MeisoOwnerScope *out_scope)
{
    MeisoOwnerScope value;
    size_t index;

    for (index = 1U; index < 8U; ++index) {
        if (source[index] != 0U) {
            return false;
        }
    }
    value.role = source[0];
    value.incarnation =
        meiso_four_kind_lab_protocol_private_read_u64(source + 8U);
    if (!meiso_owner_scope_valid(value)) {
        return false;
    }
    *out_scope = value;
    return true;
}

void meiso_four_kind_lab_protocol_private_write_content_id(
    uint8_t *destination, const MeisoContentId *value)
{
    meiso_four_kind_lab_protocol_private_write_u16(destination,
                                                   value->algorithm);
    destination[2] = value->digest_size;
    destination[3] = 0U;
    memmove(destination + 4U, value->digest, MEISO_SHA256_DIGEST_SIZE);
}

bool meiso_four_kind_lab_protocol_private_read_content_id(
    const uint8_t *source, MeisoContentId *out_value)
{
    MeisoContentId value;

    memset(&value, 0, sizeof(value));
    value.algorithm = meiso_four_kind_lab_protocol_private_read_u16(source);
    value.digest_size = source[2];
    if (source[3] != 0U || value.algorithm != MEISO_CONTENT_ALGORITHM_SHA256 ||
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

bool meiso_four_kind_lab_protocol_private_bytes_zero(const uint8_t *bytes,
                                                     size_t size)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

bool meiso_four_kind_lab_protocol_private_edge_state_valid(
    const MeisoLabEdgeState *value)
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

bool meiso_four_kind_lab_protocol_private_telemetry_sample_valid(
    const MeisoLabTelemetrySample *value)
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

void meiso_four_kind_lab_protocol_private_encode_edge_state(
    const MeisoLabEdgeState *value, uint8_t *output)
{
    uint8_t flags = 0U;

    flags = (uint8_t)(flags |
                      (value->interface_present
                           ? MEISO_FOUR_KIND_LAB_EDGE_STATE_INTERFACE_PRESENT
                           : 0U));
    flags =
        (uint8_t)(flags | (value->carrier_present
                               ? MEISO_FOUR_KIND_LAB_EDGE_STATE_CARRIER_PRESENT
                               : 0U));
    flags = (uint8_t)(flags |
                      (value->temperature_present
                           ? MEISO_FOUR_KIND_LAB_EDGE_STATE_TEMPERATURE_PRESENT
                           : 0U));
    meiso_four_kind_lab_protocol_private_write_u64(output, value->window_id);
    output[8] = flags;
    memset(output + 9U, 0, 3U);
    meiso_four_kind_lab_protocol_private_write_u32(
        output + 12U, signed_to_u32(value->corrected_millic));
    meiso_four_kind_lab_protocol_private_write_u32(output + 16U,
                                                   value->uncertainty_millic);
    memset(output + 20U, 0, 4U);
}

bool meiso_four_kind_lab_protocol_private_decode_edge_state(
    const uint8_t *input, MeisoLabEdgeState *out_value)
{
    MeisoLabEdgeState value;
    const uint8_t flags = input[8];

    if ((flags & (uint8_t)~MEISO_FOUR_KIND_LAB_EDGE_STATE_KNOWN_FLAGS) != 0U ||
        !meiso_four_kind_lab_protocol_private_bytes_zero(input + 9U, 3U) ||
        !meiso_four_kind_lab_protocol_private_bytes_zero(input + 20U, 4U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.window_id = meiso_four_kind_lab_protocol_private_read_u64(input);
    value.interface_present =
        (flags & MEISO_FOUR_KIND_LAB_EDGE_STATE_INTERFACE_PRESENT) != 0U;
    value.carrier_present =
        (flags & MEISO_FOUR_KIND_LAB_EDGE_STATE_CARRIER_PRESENT) != 0U;
    value.temperature_present =
        (flags & MEISO_FOUR_KIND_LAB_EDGE_STATE_TEMPERATURE_PRESENT) != 0U;
    value.corrected_millic = u32_to_signed(
        meiso_four_kind_lab_protocol_private_read_u32(input + 12U));
    value.uncertainty_millic =
        meiso_four_kind_lab_protocol_private_read_u32(input + 16U);
    if (!meiso_four_kind_lab_protocol_private_edge_state_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

void meiso_four_kind_lab_protocol_private_encode_telemetry(
    const MeisoLabTelemetrySample *value, uint8_t *output)
{
    uint8_t flags = 0U;

    flags =
        (uint8_t)(flags | (value->signal_present
                               ? MEISO_FOUR_KIND_LAB_TELEMETRY_SIGNAL_PRESENT
                               : 0U));
    flags = (uint8_t)(flags |
                      (value->temperature_present
                           ? MEISO_FOUR_KIND_LAB_TELEMETRY_TEMPERATURE_PRESENT
                           : 0U));
    meiso_four_kind_lab_protocol_private_write_u64(output, value->window_id);
    meiso_four_kind_lab_protocol_private_write_u64(output + 8U,
                                                   value->elapsed_ns);
    meiso_four_kind_lab_protocol_private_write_u64(output + 16U,
                                                   value->rx_bytes);
    meiso_four_kind_lab_protocol_private_write_u64(output + 24U,
                                                   value->tx_bytes);
    meiso_four_kind_lab_protocol_private_write_u64(output + 32U,
                                                   value->rx_packets);
    meiso_four_kind_lab_protocol_private_write_u64(output + 40U,
                                                   value->tx_packets);
    meiso_four_kind_lab_protocol_private_write_u64(output + 48U,
                                                   value->rx_dropped);
    meiso_four_kind_lab_protocol_private_write_u64(output + 56U,
                                                   value->tx_dropped);
    meiso_four_kind_lab_protocol_private_write_u32(
        output + 64U, signed_to_u32(value->signal_mbm));
    meiso_four_kind_lab_protocol_private_write_u32(
        output + 68U, signed_to_u32(value->corrected_millic));
    meiso_four_kind_lab_protocol_private_write_u32(output + 72U,
                                                   value->uncertainty_millic);
    output[76] = flags;
    memset(output + 77U, 0, 3U);
}

bool meiso_four_kind_lab_protocol_private_decode_telemetry(
    const uint8_t *input, MeisoLabTelemetrySample *out_value)
{
    MeisoLabTelemetrySample value;
    const uint8_t flags = input[76];

    if ((flags & (uint8_t)~MEISO_FOUR_KIND_LAB_TELEMETRY_KNOWN_FLAGS) != 0U ||
        !meiso_four_kind_lab_protocol_private_bytes_zero(input + 77U, 3U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.window_id = meiso_four_kind_lab_protocol_private_read_u64(input);
    value.elapsed_ns =
        meiso_four_kind_lab_protocol_private_read_u64(input + 8U);
    value.rx_bytes = meiso_four_kind_lab_protocol_private_read_u64(input + 16U);
    value.tx_bytes = meiso_four_kind_lab_protocol_private_read_u64(input + 24U);
    value.rx_packets =
        meiso_four_kind_lab_protocol_private_read_u64(input + 32U);
    value.tx_packets =
        meiso_four_kind_lab_protocol_private_read_u64(input + 40U);
    value.rx_dropped =
        meiso_four_kind_lab_protocol_private_read_u64(input + 48U);
    value.tx_dropped =
        meiso_four_kind_lab_protocol_private_read_u64(input + 56U);
    value.signal_mbm = u32_to_signed(
        meiso_four_kind_lab_protocol_private_read_u32(input + 64U));
    value.corrected_millic = u32_to_signed(
        meiso_four_kind_lab_protocol_private_read_u32(input + 68U));
    value.uncertainty_millic =
        meiso_four_kind_lab_protocol_private_read_u32(input + 72U);
    value.signal_present =
        (flags & MEISO_FOUR_KIND_LAB_TELEMETRY_SIGNAL_PRESENT) != 0U;
    value.temperature_present =
        (flags & MEISO_FOUR_KIND_LAB_TELEMETRY_TEMPERATURE_PRESENT) != 0U;
    if (!meiso_four_kind_lab_protocol_private_telemetry_sample_valid(&value)) {
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
    return meiso_four_kind_lab_protocol_private_descriptors;
}

const MeisoResourceDescriptor *meiso_lab_descriptor(MeisoResourceKind kind)
{
    if (kind < MEISO_RESOURCE_KIND_STATE || kind > MEISO_RESOURCE_KIND_BLOB) {
        return NULL;
    }
    return &meiso_four_kind_lab_protocol_private_descriptors[(size_t)kind - 1U];
}

const uint8_t *meiso_lab_capability_manifest(size_t *out_size)
{
    if (out_size != NULL) {
        *out_size = sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U;
    }
    return meiso_four_kind_lab_protocol_private_manifest;
}

const MeisoContentId *meiso_lab_capability_manifest_content_id(void)
{
    return &meiso_four_kind_lab_protocol_private_manifest_content_id;
}

MeisoLabCodecStatus meiso_lab_edge_state_encode(const MeisoLabEdgeState *value,
                                                MeisoMutableBuffer output,
                                                size_t *out_size)
{
    if (value == NULL || out_size == NULL ||
        !meiso_mutable_buffer_valid(output) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            value, sizeof(*value), output.data, output.capacity) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            out_size, sizeof(*out_size), output.data, output.capacity)) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (!meiso_four_kind_lab_protocol_private_edge_state_valid(value)) {
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    if (output.capacity < MEISO_LAB_EDGE_STATE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_CAPACITY;
    }
    meiso_four_kind_lab_protocol_private_encode_edge_state(value, output.data);
    *out_size = MEISO_LAB_EDGE_STATE_WIRE_SIZE;
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus meiso_lab_edge_state_decode(MeisoByteView input,
                                                MeisoLabEdgeState *out_value)
{
    MeisoLabEdgeState value;

    if (out_value == NULL || !meiso_byte_view_valid(input) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            input.data, input.size, out_value, sizeof(*out_value))) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_LAB_EDGE_STATE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (input.size != MEISO_LAB_EDGE_STATE_WIRE_SIZE ||
        !meiso_four_kind_lab_protocol_private_decode_edge_state(input.data,
                                                                &value)) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus
meiso_lab_telemetry_sample_encode(const MeisoLabTelemetrySample *value,
                                  MeisoMutableBuffer output, size_t *out_size)
{
    if (value == NULL || out_size == NULL ||
        !meiso_mutable_buffer_valid(output) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            value, sizeof(*value), output.data, output.capacity) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            out_size, sizeof(*out_size), output.data, output.capacity)) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (!meiso_four_kind_lab_protocol_private_telemetry_sample_valid(value)) {
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    if (output.capacity < MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_CAPACITY;
    }
    meiso_four_kind_lab_protocol_private_encode_telemetry(value, output.data);
    *out_size = MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE;
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus
meiso_lab_telemetry_sample_decode(MeisoByteView input,
                                  MeisoLabTelemetrySample *out_value)
{
    MeisoLabTelemetrySample value;

    if (out_value == NULL || !meiso_byte_view_valid(input) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            input.data, input.size, out_value, sizeof(*out_value))) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (input.size != MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE ||
        !meiso_four_kind_lab_protocol_private_decode_telemetry(input.data,
                                                               &value)) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_LAB_CODEC_OK;
}
