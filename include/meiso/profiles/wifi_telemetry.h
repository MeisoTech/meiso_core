#ifndef MEISO_PROFILES_WIFI_TELEMETRY_H
#define MEISO_PROFILES_WIFI_TELEMETRY_H

#include <meiso/runtime/runtime.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_WIFI_TELEMETRY_PROFILE_VERSION 1U
#define MEISO_WIFI_TELEMETRY_RESOURCE_COUNT 3U

#define MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS 100U
#define MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS 10000U
#define MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS 100U

#define MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE 16U
#define MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE 8U
#define MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE 8U
#define MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE 88U

#define MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE                 \
    (MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +                              \
     MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE)
#define MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE                  \
    (MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE +                               \
     MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE)

typedef uint32_t MeisoWifiTelemetryFieldMask;
enum {
    MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE = UINT32_C(1) << 0U,
    MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM = UINT32_C(1) << 1U,
    MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES = UINT32_C(1) << 2U,
    MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES = UINT32_C(1) << 3U,
    MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS = UINT32_C(1) << 4U,
    MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS = UINT32_C(1) << 5U,
    MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED = UINT32_C(1) << 6U,
    MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED = UINT32_C(1) << 7U,
    MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS = UINT32_C(1) << 8U,
    MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS = UINT32_C(1) << 9U,
    MEISO_WIFI_TELEMETRY_FIELD_ALL = UINT32_C(0x000003ff)
};

#define MEISO_WIFI_TELEMETRY_SIGNAL_MIN_MBM (-20000)
#define MEISO_WIFI_TELEMETRY_SIGNAL_MAX_MBM 0
/* Catalog-embedded, immutable for one accepted catalog generation. */
typedef struct {
    MeisoWifiTelemetryFieldMask supported_fields;
    uint32_t min_period_ms;
    uint32_t max_period_ms;
    uint32_t period_step_ms;
} MeisoWifiTelemetryCapability;

/* Profile suffix of the Host-owned Runtime desired State. */
typedef struct {
    MeisoWifiTelemetryFieldMask selected_fields;
    uint32_t sample_period_ms;
} MeisoWifiTelemetryDesiredParameters;

/* Profile suffix of the Edge-owned Runtime actual State. */
typedef struct {
    MeisoWifiTelemetryFieldMask selected_fields;
    uint32_t sample_period_ms;
} MeisoWifiTelemetryEffectiveParameters;

/*
 * selected_fields is the effective selection for this production epoch.
 * present_fields is a subset. Every field without a present bit must use its
 * canonical zero representation, including selected-but-unavailable fields.
 */
typedef struct {
    MeisoWifiTelemetryFieldMask selected_fields;
    MeisoWifiTelemetryFieldMask present_fields;
    bool link_state;
    int32_t signal_mbm;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t rx_errors;
    uint64_t tx_errors;
} MeisoWifiTelemetrySample;

typedef uint8_t MeisoWifiTelemetryCodecStatus;
enum {
    MEISO_WIFI_TELEMETRY_CODEC_OK = 0,
    MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT = 1,
    MEISO_WIFI_TELEMETRY_CODEC_INVALID_VALUE = 2,
    MEISO_WIFI_TELEMETRY_CODEC_CAPACITY = 3,
    MEISO_WIFI_TELEMETRY_CODEC_TRUNCATED = 4,
    MEISO_WIFI_TELEMETRY_CODEC_MALFORMED = 5
};

/* Exact identity: WIFI_TLM/_PROFILE:v1; endpoint WIFI_TLM/ENDPOINT. */
const MeisoProfileRef *meiso_wifi_telemetry_profile(void);
const MeisoEndpointId *meiso_wifi_telemetry_endpoint(void);
const MeisoSchemaRef *meiso_wifi_telemetry_capability_schema(void);
const MeisoResourceDescriptor *meiso_wifi_telemetry_resources(
    size_t *out_count
);
const MeisoResourceDescriptor *meiso_wifi_telemetry_resource_for_facet(
    MeisoRuntimeFacet facet
);
bool meiso_wifi_telemetry_parameters_supported(
    const MeisoWifiTelemetryCapability *capability,
    const MeisoWifiTelemetryDesiredParameters *parameters
);

MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_capability_encode(
    const MeisoWifiTelemetryCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_capability_decode(
    MeisoByteView input,
    MeisoWifiTelemetryCapability *out_value
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_desired_parameters_encode(
    const MeisoWifiTelemetryDesiredParameters *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_desired_parameters_decode(
    MeisoByteView input,
    MeisoWifiTelemetryDesiredParameters *out_value
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_effective_parameters_encode(
    const MeisoWifiTelemetryEffectiveParameters *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_effective_parameters_decode(
    MeisoByteView input,
    MeisoWifiTelemetryEffectiveParameters *out_value
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_sample_encode(
    const MeisoWifiTelemetrySample *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoWifiTelemetryCodecStatus meiso_wifi_telemetry_sample_decode(
    MeisoByteView input,
    MeisoWifiTelemetrySample *out_value
);

#ifdef __cplusplus
}
#endif

#endif
