#include <meiso/profiles/device_telemetry.h>

#include <limits.h>
#include <string.h>

enum {
    CAPABILITY_MAGIC = 0x44544350U, /* DTCP */
    SAMPLE_MAGIC = 0x44545330U,     /* DTS0 */
    CODEC_VERSION = 1U,
    CPU_LOAD_MAX_PPM = 1000000U
};

static const MeisoProfileRef PROFILE = {
    {UINT64_C(0x4445564943455f54), UINT64_C(0x5f50524f46494c45)},
    MEISO_DEVICE_TELEMETRY_PROFILE_VERSION
};
static const MeisoEndpointId ENDPOINT = {
    UINT64_C(0x4445564943455f54), UINT64_C(0x454e44504f494e54)
};
static const MeisoSchemaRef CAPABILITY_SCHEMA = {
    {UINT64_C(0x4445564943455f53), UINT64_C(0x4341504142494c5f)}, 1U
};
static const MeisoResourceDescriptor RESOURCES[
    MEISO_DEVICE_TELEMETRY_RESOURCE_COUNT
] = {
    {
        {UINT64_C(0x4445564943455f54), UINT64_C(0x4445535f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4445564943455f53), UINT64_C(0x444553495245445f)}, 1U},
        MEISO_OWNER_ROLE_HOST,
        MEISO_DEVICE_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x4445564943455f54), UINT64_C(0x4143545f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4445564943455f53), UINT64_C(0x41435455414c5f5f)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_DEVICE_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x4445564943455f54), UINT64_C(0x444154415f535452)},
        MEISO_RESOURCE_KIND_STREAM,
        {{UINT64_C(0x4445564943455f53), UINT64_C(0x53414d504c455f5f)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE
    }
};

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void put_u64(uint8_t *out, uint64_t value)
{
    out[0] = (uint8_t)(value >> 56U);
    out[1] = (uint8_t)(value >> 48U);
    out[2] = (uint8_t)(value >> 40U);
    out[3] = (uint8_t)(value >> 32U);
    out[4] = (uint8_t)(value >> 24U);
    out[5] = (uint8_t)(value >> 16U);
    out[6] = (uint8_t)(value >> 8U);
    out[7] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
        ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

static uint64_t get_u64(const uint8_t *in)
{
    return ((uint64_t)in[0] << 56U) | ((uint64_t)in[1] << 48U) |
        ((uint64_t)in[2] << 40U) | ((uint64_t)in[3] << 32U) |
        ((uint64_t)in[4] << 24U) | ((uint64_t)in[5] << 16U) |
        ((uint64_t)in[6] << 8U) | (uint64_t)in[7];
}

static uint32_t signed_to_u32(int32_t value)
{
    return value >= 0 ? (uint32_t)value :
        UINT32_MAX - (uint32_t)(-(value + 1));
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
    size_t i;
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

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

static bool mask_valid(MeisoDeviceTelemetryFieldMask mask)
{
    return mask != 0U &&
        (mask & (MeisoDeviceTelemetryFieldMask)
            ~MEISO_DEVICE_TELEMETRY_FIELD_ALL) == 0U;
}

static bool period_valid(uint32_t period)
{
    return period >= MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS &&
        period <= MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS &&
        period % MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS == 0U;
}

static bool capability_valid(const MeisoDeviceTelemetryCapability *value)
{
    const bool temperature = value != NULL &&
        (value->supported_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U;
    if (value == NULL || !mask_valid(value->supported_fields) ||
        !period_valid(value->min_period_ms) ||
        !period_valid(value->max_period_ms) ||
        value->min_period_ms > value->max_period_ms ||
        value->period_step_ms == 0U ||
        value->period_step_ms > MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS ||
        value->period_step_ms % MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS != 0U ||
        (value->max_period_ms - value->min_period_ms) %
            value->period_step_ms != 0U) {
        return false;
    }
    return temperature ?
        value->min_temperature_millic < value->max_temperature_millic &&
        value->max_temperature_uncertainty_millic != 0U :
        value->min_temperature_millic == 0 &&
        value->max_temperature_millic == 0 &&
        value->max_temperature_uncertainty_millic == 0U;
}

static bool parameters_valid(const MeisoDeviceTelemetryParameters *value)
{
    return value != NULL && mask_valid(value->selected_fields) &&
        period_valid(value->sample_period_ms);
}

static bool sample_valid(const MeisoDeviceTelemetrySample *value)
{
    const MeisoDeviceTelemetryFieldMask temperature =
        MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC;
    const MeisoDeviceTelemetryFieldMask cpu =
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
    const MeisoDeviceTelemetryFieldMask total =
        MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES;
    const MeisoDeviceTelemetryFieldMask available =
        MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES;
    if (value == NULL || !mask_valid(value->selected_fields) ||
        (value->present_fields & ~value->selected_fields) != 0U) {
        return false;
    }
    if ((value->present_fields & temperature) == 0U) {
        if (value->temperature_millic != 0 ||
            value->temperature_uncertainty_millic != 0U) {
            return false;
        }
    } else if (value->temperature_uncertainty_millic == 0U) {
        return false;
    }
    if ((value->present_fields & cpu) == 0U) {
        if (value->cpu_load_ppm != 0U) {
            return false;
        }
    } else if (value->cpu_load_ppm > CPU_LOAD_MAX_PPM) {
        return false;
    }
    if ((value->present_fields & total) == 0U &&
        value->memory_total_bytes != 0U) {
        return false;
    }
    if ((value->present_fields & available) == 0U &&
        value->memory_available_bytes != 0U) {
        return false;
    }
    return (value->present_fields & (total | available)) !=
            (total | available) ||
        value->memory_available_bytes <= value->memory_total_bytes;
}

static MeisoDeviceTelemetryCodecStatus prepare_encode(
    const void *value,
    size_t value_size,
    bool valid,
    MeisoMutableBuffer output,
    size_t required,
    size_t *out_size
)
{
    if (out_size == NULL) {
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (value != NULL &&
        ranges_overlap(value, value_size, out_size, sizeof(*out_size))) {
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (value == NULL || output.data == NULL) {
        *out_size = 0U;
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (ranges_overlap(value, value_size, output.data, required) ||
        ranges_overlap(output.data, required, out_size, sizeof(*out_size))) {
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (!valid) {
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_VALUE;
    }
    return output.capacity < required ?
        MEISO_DEVICE_TELEMETRY_CODEC_CAPACITY :
        MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

static MeisoDeviceTelemetryCodecStatus prepare_decode(
    MeisoByteView input,
    size_t required,
    void *output,
    size_t output_size
)
{
    if (input.data == NULL || output == NULL) {
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (ranges_overlap(input.data, input.size, output, output_size)) {
        return MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < required) {
        return MEISO_DEVICE_TELEMETRY_CODEC_TRUNCATED;
    }
    if (input.size > required) {
        return MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED;
    }
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

const MeisoProfileRef *meiso_device_telemetry_profile(void)
{
    return &PROFILE;
}

const MeisoEndpointId *meiso_device_telemetry_endpoint(void)
{
    return &ENDPOINT;
}

const MeisoSchemaRef *meiso_device_telemetry_capability_schema(void)
{
    return &CAPABILITY_SCHEMA;
}

const MeisoResourceDescriptor *meiso_device_telemetry_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEISO_DEVICE_TELEMETRY_RESOURCE_COUNT;
    }
    return RESOURCES;
}

const MeisoResourceDescriptor *meiso_device_telemetry_resource_for_facet(
    MeisoRuntimeFacet facet
)
{
    switch (facet) {
    case MEISO_RUNTIME_FACET_DESIRED:
        return &RESOURCES[0];
    case MEISO_RUNTIME_FACET_ACTUAL:
        return &RESOURCES[1];
    case MEISO_RUNTIME_FACET_DATA:
        return &RESOURCES[2];
    default:
        return NULL;
    }
}

bool meiso_device_telemetry_parameters_supported(
    const MeisoDeviceTelemetryCapability *capability,
    const MeisoDeviceTelemetryParameters *parameters
)
{
    return capability_valid(capability) && parameters_valid(parameters) &&
        (parameters->selected_fields & ~capability->supported_fields) == 0U &&
        parameters->sample_period_ms >= capability->min_period_ms &&
        parameters->sample_period_ms <= capability->max_period_ms &&
        (parameters->sample_period_ms - capability->min_period_ms) %
            capability->period_step_ms == 0U;
}

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_capability_encode(
    const MeisoDeviceTelemetryCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoDeviceTelemetryCodecStatus status = prepare_encode(
        value, sizeof(*value), capability_valid(value), output,
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE, out_size
    );
    if (status != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE);
    put_u32(output.data, CAPABILITY_MAGIC);
    put_u16(output.data + 4U, CODEC_VERSION);
    put_u32(output.data + 8U, value->supported_fields);
    put_u32(output.data + 12U, value->min_period_ms);
    put_u32(output.data + 16U, value->max_period_ms);
    put_u32(output.data + 20U, value->period_step_ms);
    put_u32(output.data + 24U, signed_to_u32(value->min_temperature_millic));
    put_u32(output.data + 28U, signed_to_u32(value->max_temperature_millic));
    put_u32(output.data + 32U, value->max_temperature_uncertainty_millic);
    put_u32(output.data + 36U, CPU_LOAD_MAX_PPM);
    *out_size = MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE;
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_capability_decode(
    MeisoByteView input,
    MeisoDeviceTelemetryCapability *out_value
)
{
    MeisoDeviceTelemetryCapability value;
    MeisoDeviceTelemetryCodecStatus status = prepare_decode(
        input, MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
        return status;
    }
    if (get_u32(input.data) != CAPABILITY_MAGIC ||
        get_u16(input.data + 4U) != CODEC_VERSION ||
        !bytes_zero(input.data + 6U, 2U) ||
        get_u32(input.data + 36U) != CPU_LOAD_MAX_PPM ||
        !bytes_zero(input.data + 40U, 8U)) {
        return MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED;
    }
    value.supported_fields = get_u32(input.data + 8U);
    value.min_period_ms = get_u32(input.data + 12U);
    value.max_period_ms = get_u32(input.data + 16U);
    value.period_step_ms = get_u32(input.data + 20U);
    value.min_temperature_millic = u32_to_signed(get_u32(input.data + 24U));
    value.max_temperature_millic = u32_to_signed(get_u32(input.data + 28U));
    value.max_temperature_uncertainty_millic = get_u32(input.data + 32U);
    if (!capability_valid(&value)) {
        return MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_parameters_encode(
    const MeisoDeviceTelemetryParameters *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoDeviceTelemetryCodecStatus status = prepare_encode(
        value, sizeof(*value), parameters_valid(value), output,
        MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE, out_size
    );
    if (status != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
        return status;
    }
    put_u32(output.data, value->selected_fields);
    put_u32(output.data + 4U, value->sample_period_ms);
    *out_size = MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE;
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_parameters_decode(
    MeisoByteView input,
    MeisoDeviceTelemetryParameters *out_value
)
{
    MeisoDeviceTelemetryParameters value;
    MeisoDeviceTelemetryCodecStatus status = prepare_decode(
        input, MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
        return status;
    }
    value.selected_fields = get_u32(input.data);
    value.sample_period_ms = get_u32(input.data + 4U);
    if (!parameters_valid(&value)) {
        return MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_sample_encode(
    const MeisoDeviceTelemetrySample *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoDeviceTelemetryCodecStatus status = prepare_encode(
        value, sizeof(*value), sample_valid(value), output,
        MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE, out_size
    );
    if (status != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE);
    put_u32(output.data, SAMPLE_MAGIC);
    put_u16(output.data + 4U, CODEC_VERSION);
    put_u32(output.data + 8U, value->selected_fields);
    put_u32(output.data + 12U, value->present_fields);
    put_u32(output.data + 16U, signed_to_u32(value->temperature_millic));
    put_u32(output.data + 20U, value->temperature_uncertainty_millic);
    put_u32(output.data + 24U, value->cpu_load_ppm);
    put_u64(output.data + 32U, value->memory_total_bytes);
    put_u64(output.data + 40U, value->memory_available_bytes);
    *out_size = MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE;
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_sample_decode(
    MeisoByteView input,
    MeisoDeviceTelemetrySample *out_value
)
{
    MeisoDeviceTelemetrySample value;
    MeisoDeviceTelemetryCodecStatus status = prepare_decode(
        input, MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
        return status;
    }
    if (get_u32(input.data) != SAMPLE_MAGIC ||
        get_u16(input.data + 4U) != CODEC_VERSION ||
        !bytes_zero(input.data + 6U, 2U) ||
        !bytes_zero(input.data + 28U, 4U)) {
        return MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED;
    }
    value.selected_fields = get_u32(input.data + 8U);
    value.present_fields = get_u32(input.data + 12U);
    value.temperature_millic = u32_to_signed(get_u32(input.data + 16U));
    value.temperature_uncertainty_millic = get_u32(input.data + 20U);
    value.cpu_load_ppm = get_u32(input.data + 24U);
    value.memory_total_bytes = get_u64(input.data + 32U);
    value.memory_available_bytes = get_u64(input.data + 40U);
    if (!sample_valid(&value)) {
        return MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_DEVICE_TELEMETRY_CODEC_OK;
}
