#ifndef MEISO_PROFILES_DEVICE_TELEMETRY_H
#define MEISO_PROFILES_DEVICE_TELEMETRY_H

#include <meiso/runtime/runtime.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_DEVICE_TELEMETRY_PROFILE_VERSION 1U
#define MEISO_DEVICE_TELEMETRY_RESOURCE_COUNT 3U
#define MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS 100U
#define MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS 10000U
#define MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS 100U
#define MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE 48U
#define MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE 8U
#define MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE 48U
#define MEISO_DEVICE_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE \
    (MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + \
     MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE)
#define MEISO_DEVICE_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE \
    (MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + \
     MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE)

typedef uint32_t MeisoDeviceTelemetryFieldMask;
enum {
    MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC = UINT32_C(1) << 0U,
    MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM = UINT32_C(1) << 1U,
    MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES = UINT32_C(1) << 2U,
    MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES = UINT32_C(1) << 3U,
    MEISO_DEVICE_TELEMETRY_FIELD_ALL = UINT32_C(0x0000000f)
};

typedef struct {
    MeisoDeviceTelemetryFieldMask supported_fields;
    uint32_t min_period_ms;
    uint32_t max_period_ms;
    uint32_t period_step_ms;
    int32_t min_temperature_millic;
    int32_t max_temperature_millic;
    uint32_t max_temperature_uncertainty_millic;
} MeisoDeviceTelemetryCapability;

typedef struct {
    MeisoDeviceTelemetryFieldMask selected_fields;
    uint32_t sample_period_ms;
} MeisoDeviceTelemetryParameters;

/* Every absent field has its canonical zero representation. */
typedef struct {
    MeisoDeviceTelemetryFieldMask selected_fields;
    MeisoDeviceTelemetryFieldMask present_fields;
    int32_t temperature_millic;
    uint32_t temperature_uncertainty_millic;
    uint32_t cpu_load_ppm;
    uint64_t memory_total_bytes;
    uint64_t memory_available_bytes;
} MeisoDeviceTelemetrySample;

typedef uint8_t MeisoDeviceTelemetryCodecStatus;
enum {
    MEISO_DEVICE_TELEMETRY_CODEC_OK = 0,
    MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT = 1,
    MEISO_DEVICE_TELEMETRY_CODEC_INVALID_VALUE = 2,
    MEISO_DEVICE_TELEMETRY_CODEC_CAPACITY = 3,
    MEISO_DEVICE_TELEMETRY_CODEC_TRUNCATED = 4,
    MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED = 5
};

/* Exact identity: DEVICE_T/_PROFILE:v1; endpoint DEVICE_T/ENDPOINT. */
const MeisoProfileRef *meiso_device_telemetry_profile(void);
const MeisoEndpointId *meiso_device_telemetry_endpoint(void);
const MeisoSchemaRef *meiso_device_telemetry_capability_schema(void);
const MeisoResourceDescriptor *meiso_device_telemetry_resources(
    size_t *out_count
);
const MeisoResourceDescriptor *meiso_device_telemetry_resource_for_facet(
    MeisoRuntimeFacet facet
);
bool meiso_device_telemetry_parameters_supported(
    const MeisoDeviceTelemetryCapability *capability,
    const MeisoDeviceTelemetryParameters *parameters
);

/*
 * Typed input, wire input/output, and out_size ranges must be disjoint. Alias
 * is INVALID_ARGUMENT with no mutation. Other encode failures set a disjoint
 * out_size to zero and leave output unchanged; decode failures leave the
 * typed output unchanged.
 */

MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_capability_encode(
    const MeisoDeviceTelemetryCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_capability_decode(
    MeisoByteView input,
    MeisoDeviceTelemetryCapability *out_value
);
MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_parameters_encode(
    const MeisoDeviceTelemetryParameters *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_parameters_decode(
    MeisoByteView input,
    MeisoDeviceTelemetryParameters *out_value
);
MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_sample_encode(
    const MeisoDeviceTelemetrySample *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDeviceTelemetryCodecStatus meiso_device_telemetry_sample_decode(
    MeisoByteView input,
    MeisoDeviceTelemetrySample *out_value
);

#ifdef __cplusplus
}
#endif

#endif
