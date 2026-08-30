#include <meiso/profiles/wifi_telemetry.h>

#include <limits.h>
#include <string.h>

enum {
    SAMPLE_LINK_OFFSET = 8U,
    SAMPLE_SIGNAL_OFFSET = 16U,
    SAMPLE_RX_BYTES_OFFSET = 24U,
    SAMPLE_TX_BYTES_OFFSET = 32U,
    SAMPLE_RX_PACKETS_OFFSET = 40U,
    SAMPLE_TX_PACKETS_OFFSET = 48U,
    SAMPLE_RX_DROPPED_OFFSET = 56U,
    SAMPLE_TX_DROPPED_OFFSET = 64U,
    SAMPLE_RX_ERRORS_OFFSET = 72U,
    SAMPLE_TX_ERRORS_OFFSET = 80U
};

static const MeisoProfileRef WIFI_PROFILE = {
    {
        UINT64_C(0x574946495f544c4d),
        UINT64_C(0x5f50524f46494c45)
    },
    MEISO_WIFI_TELEMETRY_PROFILE_VERSION
};

static const MeisoEndpointId WIFI_ENDPOINT = {
    UINT64_C(0x574946495f544c4d),
    UINT64_C(0x454e44504f494e54)
};

static const MeisoSchemaRef WIFI_CAPABILITY_SCHEMA = {
    {
        UINT64_C(0x574946495f534348),
        UINT64_C(0x4341504142494c5f)
    },
    1U
};

static const MeisoResourceDescriptor WIFI_RESOURCES[
    MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
] = {
    {
        {
            UINT64_C(0x574946495f544c4d),
            UINT64_C(0x4445535f53544154)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x574946495f534348),
                UINT64_C(0x444553495245445f)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x574946495f544c4d),
            UINT64_C(0x4143545f53544154)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x574946495f534348),
                UINT64_C(0x41435455414c5f5f)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x574946495f544c4d),
            UINT64_C(0x444154415f535452)
        },
        MEISO_RESOURCE_KIND_STREAM,
        {
            {
                UINT64_C(0x574946495f534348),
                UINT64_C(0x53414d504c455f5f)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE
    }
};

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

static bool field_mask_valid(MeisoWifiTelemetryFieldMask value)
{
    return value != 0U &&
        (value & (MeisoWifiTelemetryFieldMask)
            ~MEISO_WIFI_TELEMETRY_FIELD_ALL) == 0U;
}

static bool period_valid(uint32_t value)
{
    return value >= MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS &&
        value <= MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS &&
        value % MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS == 0U;
}

static bool capability_valid(const MeisoWifiTelemetryCapability *value)
{
    return value != NULL && field_mask_valid(value->supported_fields) &&
        period_valid(value->min_period_ms) &&
        period_valid(value->max_period_ms) &&
        value->min_period_ms <= value->max_period_ms &&
        value->period_step_ms != 0U &&
        value->period_step_ms % MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS == 0U &&
        value->period_step_ms <= MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS &&
        (value->max_period_ms - value->min_period_ms) %
            value->period_step_ms == 0U;
}

static bool desired_valid(
    const MeisoWifiTelemetryDesiredParameters *value
)
{
    return value != NULL && field_mask_valid(value->selected_fields) &&
        period_valid(value->sample_period_ms);
}

static bool effective_valid(
    const MeisoWifiTelemetryEffectiveParameters *value
)
{
    if (value == NULL) {
        return false;
    }
    if (value->selected_fields == 0U || value->sample_period_ms == 0U) {
        return value->selected_fields == 0U && value->sample_period_ms == 0U;
    }
    return field_mask_valid(value->selected_fields) &&
        period_valid(value->sample_period_ms);
}

static bool selected_and_present(
    const MeisoWifiTelemetrySample *value,
    MeisoWifiTelemetryFieldMask field
)
{
    return (value->selected_fields & field) != 0U &&
        (value->present_fields & field) != 0U;
}

static bool zero_if_absent_u64(
    const MeisoWifiTelemetrySample *value,
    MeisoWifiTelemetryFieldMask field,
    uint64_t field_value
)
{
    return selected_and_present(value, field) || field_value == 0U;
}

static bool sample_valid(const MeisoWifiTelemetrySample *value)
{
    if (value == NULL || !field_mask_valid(value->selected_fields) ||
        (value->present_fields &
         (MeisoWifiTelemetryFieldMask)~value->selected_fields) != 0U) {
        return false;
    }
    if (!selected_and_present(
            value, MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE
        ) && value->link_state) {
        return false;
    }
    if (selected_and_present(
            value, MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM
        )) {
        if (value->signal_mbm < MEISO_WIFI_TELEMETRY_SIGNAL_MIN_MBM ||
            value->signal_mbm > MEISO_WIFI_TELEMETRY_SIGNAL_MAX_MBM) {
            return false;
        }
    } else if (value->signal_mbm != 0) {
        return false;
    }
    return zero_if_absent_u64(
               value, MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES, value->rx_bytes
           ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES, value->tx_bytes
        ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS,
            value->rx_packets
        ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS,
            value->tx_packets
        ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED,
            value->rx_dropped
        ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED,
            value->tx_dropped
        ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS,
            value->rx_errors
        ) &&
        zero_if_absent_u64(
            value, MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS,
            value->tx_errors
        );
}

static MeisoWifiTelemetryCodecStatus prepare_encode(
    const void *value,
    bool value_valid,
    MeisoMutableBuffer output,
    size_t required,
    size_t *out_size
)
{
    if (out_size == NULL) {
        return MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (value == NULL || output.data == NULL) {
        return MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (!value_valid) {
        return MEISO_WIFI_TELEMETRY_CODEC_INVALID_VALUE;
    }
    if (output.capacity < required) {
        return MEISO_WIFI_TELEMETRY_CODEC_CAPACITY;
    }
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

static MeisoWifiTelemetryCodecStatus prepare_decode(
    MeisoByteView input,
    size_t required,
    const void *out_value
)
{
    if (out_value == NULL || input.data == NULL) {
        return MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < required) {
        return MEISO_WIFI_TELEMETRY_CODEC_TRUNCATED;
    }
    if (input.size > required) {
        return MEISO_WIFI_TELEMETRY_CODEC_MALFORMED;
    }
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

const MeisoProfileRef *meiso_wifi_telemetry_profile(void)
{
    return &WIFI_PROFILE;
}

const MeisoEndpointId *meiso_wifi_telemetry_endpoint(void)
{
    return &WIFI_ENDPOINT;
}

const MeisoSchemaRef *meiso_wifi_telemetry_capability_schema(void)
{
    return &WIFI_CAPABILITY_SCHEMA;
}

const MeisoResourceDescriptor *meiso_wifi_telemetry_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEISO_WIFI_TELEMETRY_RESOURCE_COUNT;
    }
    return WIFI_RESOURCES;
}

const MeisoResourceDescriptor *meiso_wifi_telemetry_resource_for_facet(
    MeisoRuntimeFacet facet
)
{
    switch (facet) {
    case MEISO_RUNTIME_FACET_DESIRED:
        return &WIFI_RESOURCES[0];
    case MEISO_RUNTIME_FACET_ACTUAL:
        return &WIFI_RESOURCES[1];
    case MEISO_RUNTIME_FACET_DATA:
        return &WIFI_RESOURCES[2];
    default:
        return NULL;
    }
}

bool meiso_wifi_telemetry_parameters_supported(
    const MeisoWifiTelemetryCapability *capability,
    const MeisoWifiTelemetryDesiredParameters *parameters
)
{
    return capability_valid(capability) && desired_valid(parameters) &&
        (parameters->selected_fields &
         (MeisoWifiTelemetryFieldMask)~capability->supported_fields) == 0U &&
        parameters->sample_period_ms >= capability->min_period_ms &&
        parameters->sample_period_ms <= capability->max_period_ms &&
        (parameters->sample_period_ms - capability->min_period_ms) %
            capability->period_step_ms == 0U;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_capability_encode(
    const MeisoWifiTelemetryCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoWifiTelemetryCapability local;
    MeisoWifiTelemetryCodecStatus status = prepare_encode(
        value, capability_valid(value), output,
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE, out_size
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    local = *value;
    write_u32(output.data, local.supported_fields);
    write_u32(output.data + 4U, local.min_period_ms);
    write_u32(output.data + 8U, local.max_period_ms);
    write_u32(output.data + 12U, local.period_step_ms);
    *out_size = MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_capability_decode(
    MeisoByteView input,
    MeisoWifiTelemetryCapability *out_value
)
{
    MeisoWifiTelemetryCapability value;
    const MeisoWifiTelemetryCodecStatus status = prepare_decode(
        input, MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE, out_value
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    value.supported_fields = read_u32(input.data);
    value.min_period_ms = read_u32(input.data + 4U);
    value.max_period_ms = read_u32(input.data + 8U);
    value.period_step_ms = read_u32(input.data + 12U);
    if (!capability_valid(&value)) {
        return MEISO_WIFI_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_desired_parameters_encode(
    const MeisoWifiTelemetryDesiredParameters *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoWifiTelemetryDesiredParameters local;
    MeisoWifiTelemetryCodecStatus status = prepare_encode(
        value, desired_valid(value), output,
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE, out_size
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    local = *value;
    write_u32(output.data, local.selected_fields);
    write_u32(output.data + 4U, local.sample_period_ms);
    *out_size = MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_desired_parameters_decode(
    MeisoByteView input,
    MeisoWifiTelemetryDesiredParameters *out_value
)
{
    MeisoWifiTelemetryDesiredParameters value;
    const MeisoWifiTelemetryCodecStatus status = prepare_decode(
        input, MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE, out_value
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    value.selected_fields = read_u32(input.data);
    value.sample_period_ms = read_u32(input.data + 4U);
    if (!desired_valid(&value)) {
        return MEISO_WIFI_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_effective_parameters_encode(
    const MeisoWifiTelemetryEffectiveParameters *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoWifiTelemetryEffectiveParameters local;
    MeisoWifiTelemetryCodecStatus status = prepare_encode(
        value, effective_valid(value), output,
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE, out_size
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    local = *value;
    write_u32(output.data, local.selected_fields);
    write_u32(output.data + 4U, local.sample_period_ms);
    *out_size = MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_effective_parameters_decode(
    MeisoByteView input,
    MeisoWifiTelemetryEffectiveParameters *out_value
)
{
    MeisoWifiTelemetryEffectiveParameters value;
    const MeisoWifiTelemetryCodecStatus status = prepare_decode(
        input, MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE, out_value
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    value.selected_fields = read_u32(input.data);
    value.sample_period_ms = read_u32(input.data + 4U);
    if (!effective_valid(&value)) {
        return MEISO_WIFI_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_sample_encode(
    const MeisoWifiTelemetrySample *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoWifiTelemetrySample local;
    MeisoWifiTelemetryCodecStatus status = prepare_encode(
        value, sample_valid(value), output,
        MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE,
        out_size
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    local = *value;
    memset(output.data, 0, MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE);
    write_u32(output.data, local.selected_fields);
    write_u32(output.data + 4U, local.present_fields);
    output.data[SAMPLE_LINK_OFFSET] = local.link_state ? 1U : 0U;
    write_u32(output.data + SAMPLE_SIGNAL_OFFSET,
              signed_to_u32(local.signal_mbm));
    write_u64(output.data + SAMPLE_RX_BYTES_OFFSET, local.rx_bytes);
    write_u64(output.data + SAMPLE_TX_BYTES_OFFSET, local.tx_bytes);
    write_u64(output.data + SAMPLE_RX_PACKETS_OFFSET, local.rx_packets);
    write_u64(output.data + SAMPLE_TX_PACKETS_OFFSET, local.tx_packets);
    write_u64(output.data + SAMPLE_RX_DROPPED_OFFSET, local.rx_dropped);
    write_u64(output.data + SAMPLE_TX_DROPPED_OFFSET, local.tx_dropped);
    write_u64(output.data + SAMPLE_RX_ERRORS_OFFSET, local.rx_errors);
    write_u64(output.data + SAMPLE_TX_ERRORS_OFFSET, local.tx_errors);
    *out_size = MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_sample_decode(
    MeisoByteView input,
    MeisoWifiTelemetrySample *out_value
)
{
    MeisoWifiTelemetrySample value;
    const MeisoWifiTelemetryCodecStatus status = prepare_decode(
        input, MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE, out_value
    );

    if (status != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return status;
    }
    if (input.data[SAMPLE_LINK_OFFSET] > 1U ||
        !bytes_zero(input.data + SAMPLE_LINK_OFFSET + 1U, 7U) ||
        !bytes_zero(input.data + SAMPLE_SIGNAL_OFFSET + 4U, 4U)) {
        return MEISO_WIFI_TELEMETRY_CODEC_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.selected_fields = read_u32(input.data);
    value.present_fields = read_u32(input.data + 4U);
    value.link_state = input.data[SAMPLE_LINK_OFFSET] != 0U;
    value.signal_mbm = u32_to_signed(
        read_u32(input.data + SAMPLE_SIGNAL_OFFSET)
    );
    value.rx_bytes = read_u64(input.data + SAMPLE_RX_BYTES_OFFSET);
    value.tx_bytes = read_u64(input.data + SAMPLE_TX_BYTES_OFFSET);
    value.rx_packets = read_u64(input.data + SAMPLE_RX_PACKETS_OFFSET);
    value.tx_packets = read_u64(input.data + SAMPLE_TX_PACKETS_OFFSET);
    value.rx_dropped = read_u64(input.data + SAMPLE_RX_DROPPED_OFFSET);
    value.tx_dropped = read_u64(input.data + SAMPLE_TX_DROPPED_OFFSET);
    value.rx_errors = read_u64(input.data + SAMPLE_RX_ERRORS_OFFSET);
    value.tx_errors = read_u64(input.data + SAMPLE_TX_ERRORS_OFFSET);
    if (!sample_valid(&value)) {
        return MEISO_WIFI_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_WIFI_TELEMETRY_CODEC_OK;
}
