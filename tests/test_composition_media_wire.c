#include "composition_session_v2.h"
#include "composition_session_v3.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                               \
    if (!(condition)) {                                                     \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                            \
        return false;                                                       \
    }                                                                       \
} while (0)

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

static uint64_t fnv1a64(const uint8_t *bytes, size_t size)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t i;
    for (i = 0U; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static MeisoSessionConfig base_session_config(void)
{
    MeisoSessionConfig value;
    memset(&value, 0, sizeof(value));
    value.local_role = MEISO_SESSION_ROLE_HOST;
    value.local_peer_id = (MeisoPeerId){1U, 2U};
    value.local_incarnation = 3U;
    value.local_nonce = (MeisoSessionNonce){4U, 5U};
    value.expected_peer_id = (MeisoPeerId){6U, 7U};
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.profile = *meiso_composition_scene_profile();
    value.limits = (MeisoSessionLimits){16384U, 8192U, 32U};
    return value;
}

#define CHECK_DEVICE_ALIAS(Type, Value, Encode, Decode, Wire, WireSize) do {  \
    union { max_align_t alignment; Type typed; uint8_t bytes[256]; }          \
        typed_overlap;                                                        \
    union { max_align_t alignment; size_t size; uint8_t bytes[256]; }         \
        size_overlap;                                                         \
    union { max_align_t alignment; Type typed; uint8_t bytes[256]; }          \
        decode_overlap;                                                       \
    uint8_t typed_before[sizeof(typed_overlap.bytes)];                         \
    uint8_t size_before[sizeof(size_overlap.bytes)];                           \
    uint8_t decode_before[sizeof(decode_overlap.bytes)];                       \
    size_t separate_size = 77U;                                               \
    memset(&typed_overlap, 0xa5, sizeof(typed_overlap));                       \
    typed_overlap.typed = (Value);                                             \
    memcpy(typed_before, typed_overlap.bytes, sizeof(typed_before));           \
    CHECK(Encode(&typed_overlap.typed,                                         \
                 (MeisoMutableBuffer){typed_overlap.bytes, (WireSize)},        \
                 &separate_size) ==                                           \
          MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT);                     \
    CHECK(separate_size == 77U &&                                             \
          memcmp(typed_overlap.bytes, typed_before, sizeof(typed_before)) ==   \
              0);                                                             \
    memset(&size_overlap, 0x5a, sizeof(size_overlap));                         \
    memcpy(size_before, size_overlap.bytes, sizeof(size_before));              \
    CHECK(Encode(&(Value),                                                     \
                 (MeisoMutableBuffer){size_overlap.bytes, (WireSize)},         \
                 &size_overlap.size) ==                                       \
          MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT);                     \
    CHECK(memcmp(size_overlap.bytes, size_before, sizeof(size_before)) == 0);  \
    memset(&decode_overlap, 0x3c, sizeof(decode_overlap));                     \
    memcpy(decode_overlap.bytes, (Wire), (WireSize));                          \
    memcpy(decode_before, decode_overlap.bytes, sizeof(decode_before));        \
    CHECK(Decode((MeisoByteView){decode_overlap.bytes, (WireSize)},            \
                 &decode_overlap.typed) ==                                    \
          MEISO_DEVICE_TELEMETRY_CODEC_INVALID_ARGUMENT);                     \
    CHECK(memcmp(decode_overlap.bytes, decode_before,                          \
                 sizeof(decode_before)) == 0);                                \
} while (0)

#define CHECK_MEDIA_ALIAS(Type, Value, Encode, Decode, Wire, WireSize) do {   \
    union { max_align_t alignment; Type typed; uint8_t bytes[256]; }          \
        typed_overlap;                                                        \
    union { max_align_t alignment; size_t size; uint8_t bytes[256]; }         \
        size_overlap;                                                         \
    union { max_align_t alignment; Type typed; uint8_t bytes[256]; }          \
        decode_overlap;                                                       \
    uint8_t typed_before[sizeof(typed_overlap.bytes)];                         \
    uint8_t size_before[sizeof(size_overlap.bytes)];                           \
    uint8_t decode_before[sizeof(decode_overlap.bytes)];                       \
    size_t separate_size = 77U;                                               \
    memset(&typed_overlap, 0xa5, sizeof(typed_overlap));                       \
    typed_overlap.typed = (Value);                                             \
    memcpy(typed_before, typed_overlap.bytes, sizeof(typed_before));           \
    CHECK(Encode(&typed_overlap.typed,                                         \
                 (MeisoMutableBuffer){typed_overlap.bytes, (WireSize)},        \
                 &separate_size) ==                                           \
          MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);                     \
    CHECK(separate_size == 77U &&                                             \
          memcmp(typed_overlap.bytes, typed_before, sizeof(typed_before)) ==   \
              0);                                                             \
    memset(&size_overlap, 0x5a, sizeof(size_overlap));                         \
    memcpy(size_before, size_overlap.bytes, sizeof(size_before));              \
    CHECK(Encode(&(Value),                                                     \
                 (MeisoMutableBuffer){size_overlap.bytes, (WireSize)},         \
                 &size_overlap.size) ==                                       \
          MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);                     \
    CHECK(memcmp(size_overlap.bytes, size_before, sizeof(size_before)) == 0);  \
    memset(&decode_overlap, 0x3c, sizeof(decode_overlap));                     \
    memcpy(decode_overlap.bytes, (Wire), (WireSize));                          \
    memcpy(decode_before, decode_overlap.bytes, sizeof(decode_before));        \
    CHECK(Decode((MeisoByteView){decode_overlap.bytes, (WireSize)},            \
                 &decode_overlap.typed) ==                                    \
          MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);                     \
    CHECK(memcmp(decode_overlap.bytes, decode_before,                          \
                 sizeof(decode_before)) == 0);                                \
} while (0)

static MeisoCompositionSessionV3CatalogConfig catalog_config(void)
{
    MeisoCompositionSessionV3CatalogConfig value;
    memset(&value, 0, sizeof(value));
    value.wifi_incarnation = 11U;
    value.presentation_incarnation = 12U;
    value.scene_incarnation = 13U;
    value.device_telemetry_incarnation = 14U;
    value.render_media_incarnation = 15U;
    value.camera_media_incarnation = 16U;
    value.wifi_capability = (MeisoWifiTelemetryCapability){
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        100U, 1000U, 100U
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 |
            MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGB565,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        64U, 48U, 320U, 240U, 10000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 640U, 480U, 30000U, 460800U,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static bool test_device_literals(void)
{
    const MeisoDeviceTelemetryCapability value = {
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    const uint8_t golden[MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE] = {
        0x44U, 0x54U, 0x43U, 0x50U, 0x00U, 0x01U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x0fU, 0x00U, 0x00U, 0x00U, 0x64U,
        0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x00U, 0x00U, 0x64U,
        0xffU, 0xffU, 0x63U, 0xc0U, 0x00U, 0x01U, 0xe8U, 0x48U,
        0x00U, 0x00U, 0x03U, 0xe8U, 0x00U, 0x0fU, 0x42U, 0x40U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };
    uint8_t bytes[sizeof(golden)];
    MeisoDeviceTelemetryCapability decoded;
    size_t size = 99U;
    CHECK(meiso_device_telemetry_profile()->id.high ==
          UINT64_C(0x4445564943455f54));
    CHECK(meiso_device_telemetry_capability_encode(
              &value, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &size
          ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(golden) && memcmp(bytes, golden, sizeof(golden)) == 0);
    CHECK(meiso_device_telemetry_capability_decode(
              (MeisoByteView){golden, sizeof(golden)}, &decoded
          ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(decoded.supported_fields == value.supported_fields);
    CHECK(decoded.min_period_ms == value.min_period_ms);
    CHECK(decoded.max_period_ms == value.max_period_ms);
    CHECK(decoded.period_step_ms == value.period_step_ms);
    CHECK(decoded.min_temperature_millic == value.min_temperature_millic);
    CHECK(decoded.max_temperature_millic == value.max_temperature_millic);
    CHECK(decoded.max_temperature_uncertainty_millic ==
          value.max_temperature_uncertainty_millic);
    bytes[6] = 1U;
    CHECK(meiso_device_telemetry_capability_decode(
              (MeisoByteView){bytes, sizeof(bytes)}, &decoded
          ) == MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED);
    return true;
}

static bool test_device_codec_boundaries(void)
{
    const MeisoDeviceTelemetryCapability capability = {
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    const MeisoDeviceTelemetryParameters parameters = {
        MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC |
            MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
        500U
    };
    const MeisoDeviceTelemetrySample sample = {
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        42125, 250U, 765432U,
        UINT64_C(0x0000000200000000), UINT64_C(0x0000000180000000)
    };
    const uint8_t parameters_golden[8] = {
        0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x01U, 0xf4U
    };
    uint8_t capability_wire[MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE];
    uint8_t parameters_wire[MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE];
    uint8_t sample_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t trailing[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE + 1U];
    uint8_t before[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoDeviceTelemetryParameters parameters_out;
    MeisoDeviceTelemetrySample sample_out;
    MeisoDeviceTelemetrySample invalid_sample;
    MeisoDeviceTelemetryCapability capability_out;
    size_t size = 0U;

    CHECK(meiso_device_telemetry_parameters_supported(
        &capability, &parameters
    ));
    CHECK(meiso_device_telemetry_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){parameters_wire, sizeof(parameters_wire)},
        &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(parameters_golden) &&
          memcmp(parameters_wire, parameters_golden,
                 sizeof(parameters_golden)) == 0);
    CHECK(meiso_device_telemetry_parameters_decode(
        (MeisoByteView){parameters_wire, sizeof(parameters_wire)},
        &parameters_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(parameters_out.selected_fields == parameters.selected_fields &&
          parameters_out.sample_period_ms == parameters.sample_period_ms);

    CHECK(meiso_device_telemetry_sample_encode(
        &sample, (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)}, &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(sample_wire) &&
          memcmp(sample_wire, "DTS0\0\1\0\0", 8U) == 0);
    CHECK(meiso_device_telemetry_sample_decode(
        (MeisoByteView){sample_wire, sizeof(sample_wire)}, &sample_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(sample_out.selected_fields == sample.selected_fields &&
          sample_out.present_fields == sample.present_fields &&
          sample_out.temperature_millic == sample.temperature_millic &&
          sample_out.temperature_uncertainty_millic ==
              sample.temperature_uncertainty_millic &&
          sample_out.cpu_load_ppm == sample.cpu_load_ppm &&
          sample_out.memory_total_bytes == sample.memory_total_bytes &&
          sample_out.memory_available_bytes == sample.memory_available_bytes);

    memset(capability_wire, 0xa5, sizeof(capability_wire));
    memcpy(before, capability_wire, sizeof(capability_wire));
    size = 99U;
    CHECK(meiso_device_telemetry_capability_encode(
        &capability,
        (MeisoMutableBuffer){capability_wire, sizeof(capability_wire) - 1U},
        &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_CAPACITY);
    CHECK(size == 0U &&
          memcmp(capability_wire, before, sizeof(capability_wire)) == 0);
    size = 99U;
    CHECK(meiso_device_telemetry_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){capability_wire,
                             MEISO_DEVICE_TELEMETRY_PARAMETERS_WIRE_SIZE - 1U},
        &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_CAPACITY);
    CHECK(size == 0U &&
          memcmp(capability_wire, before, sizeof(capability_wire)) == 0);
    size = 99U;
    CHECK(meiso_device_telemetry_sample_encode(
        &sample,
        (MeisoMutableBuffer){capability_wire,
                             MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE - 1U},
        &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_CAPACITY);
    CHECK(size == 0U &&
          memcmp(capability_wire, before, sizeof(capability_wire)) == 0);
    invalid_sample = sample;
    invalid_sample.cpu_load_ppm = 1000001U;
    size = 99U;
    CHECK(meiso_device_telemetry_sample_encode(
        &invalid_sample,
        (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)}, &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_INVALID_VALUE);
    CHECK(size == 0U &&
          memcmp(capability_wire, before, sizeof(capability_wire)) == 0);
    memset(&capability_out, 0xa5, sizeof(capability_out));
    memcpy(before, &capability_out, sizeof(capability_out));
    CHECK(meiso_device_telemetry_capability_decode(
        (MeisoByteView){capability_wire, sizeof(capability_wire) - 1U},
        &capability_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_TRUNCATED);
    CHECK(memcmp(&capability_out, before, sizeof(capability_out)) == 0);

    memset(&parameters_out, 0xa5, sizeof(parameters_out));
    memcpy(before, &parameters_out, sizeof(parameters_out));
    CHECK(meiso_device_telemetry_parameters_decode(
        (MeisoByteView){parameters_wire, sizeof(parameters_wire) - 1U},
        &parameters_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_TRUNCATED);
    CHECK(memcmp(&parameters_out, before, sizeof(parameters_out)) == 0);
    memset(&sample_out, 0xa5, sizeof(sample_out));
    memcpy(before, &sample_out, sizeof(sample_out));
    CHECK(meiso_device_telemetry_sample_decode(
        (MeisoByteView){sample_wire, sizeof(sample_wire) - 1U}, &sample_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_TRUNCATED);
    CHECK(memcmp(&sample_out, before, sizeof(sample_out)) == 0);
    memcpy(trailing, capability_wire, sizeof(capability_wire));
    trailing[sizeof(capability_wire)] = 0U;
    memset(&capability_out, 0xa5, sizeof(capability_out));
    memcpy(before, &capability_out, sizeof(capability_out));
    CHECK(meiso_device_telemetry_capability_decode(
        (MeisoByteView){trailing, sizeof(capability_wire) + 1U},
        &capability_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED);
    CHECK(memcmp(&capability_out, before, sizeof(capability_out)) == 0);
    memcpy(trailing, parameters_wire, sizeof(parameters_wire));
    trailing[sizeof(parameters_wire)] = 0U;
    memset(&parameters_out, 0xa5, sizeof(parameters_out));
    memcpy(before, &parameters_out, sizeof(parameters_out));
    CHECK(meiso_device_telemetry_parameters_decode(
        (MeisoByteView){trailing, sizeof(parameters_wire) + 1U},
        &parameters_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED);
    CHECK(memcmp(&parameters_out, before, sizeof(parameters_out)) == 0);
    memcpy(trailing, sample_wire, sizeof(sample_wire));
    trailing[sizeof(sample_wire)] = 0U;
    memset(&sample_out, 0xa5, sizeof(sample_out));
    memcpy(before, &sample_out, sizeof(sample_out));
    CHECK(meiso_device_telemetry_sample_decode(
        (MeisoByteView){trailing, sizeof(sample_wire) + 1U}, &sample_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED);
    CHECK(memcmp(&sample_out, before, sizeof(sample_out)) == 0);
    sample_wire[28] = 1U;
    CHECK(meiso_device_telemetry_sample_decode(
        (MeisoByteView){sample_wire, sizeof(sample_wire)}, &sample_out
    ) == MEISO_DEVICE_TELEMETRY_CODEC_MALFORMED);
    CHECK(memcmp(&sample_out, before, sizeof(sample_out)) == 0);
    CHECK(meiso_device_telemetry_capability_encode(
        &capability,
        (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)}, &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK(meiso_device_telemetry_sample_encode(
        &sample, (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)}, &size
    ) == MEISO_DEVICE_TELEMETRY_CODEC_OK);
    CHECK_DEVICE_ALIAS(
        MeisoDeviceTelemetryCapability, capability,
        meiso_device_telemetry_capability_encode,
        meiso_device_telemetry_capability_decode,
        capability_wire, sizeof(capability_wire)
    );
    CHECK_DEVICE_ALIAS(
        MeisoDeviceTelemetryParameters, parameters,
        meiso_device_telemetry_parameters_encode,
        meiso_device_telemetry_parameters_decode,
        parameters_wire, sizeof(parameters_wire)
    );
    CHECK_DEVICE_ALIAS(
        MeisoDeviceTelemetrySample, sample,
        meiso_device_telemetry_sample_encode,
        meiso_device_telemetry_sample_decode,
        sample_wire, sizeof(sample_wire)
    );
    return true;
}

static bool test_profile_and_catalog(void)
{
    MeisoCompositionSessionV3CatalogConfig config = catalog_config();
    uint8_t capability[MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    uint8_t catalog[MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE];
    size_t size = 0U;
    size_t i;

    CHECK(meiso_composition_scene_profile()->version == 1U);
    CHECK(meiso_composition_scene_profile_v2()->version == 2U);
    CHECK(meiso_composition_scene_profile_v3()->version == 3U);
    CHECK(meiso_composition_scene_profile_v3()->id.high ==
          UINT64_C(0x4b3233305f53434e));
    CHECK(meiso_composition_session_v3_build_catalog(
              &config, (MeisoMutableBuffer){capability, sizeof(capability)},
              endpoints, MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
              routes, MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(endpoints[3].capability.size == 48U);
    CHECK(routes[14].delivery ==
          MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    CHECK(routes[17].delivery ==
          MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    CHECK(meiso_composition_session_v3_catalog_encode(
              7U, endpoints, MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
              routes, MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT,
              (MeisoMutableBuffer){catalog, sizeof(catalog)}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(size == MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE);
    CHECK(memcmp(catalog, "MRT0\x04\x01\0\0\0\0\x07\xf0", 12U) == 0);
    CHECK(catalog[27] == 18U);
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT; ++i) {
        const uint8_t expected = i == 14U || i == 17U ?
            MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA :
            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
        CHECK(catalog[592U + i * 80U + 72U] == expected);
        CHECK(bytes_are(catalog + 592U + i * 80U + 73U, 0U, 7U));
    }
    memset(catalog, 0xa5, sizeof(catalog));
    size = 44U;
    CHECK(meiso_composition_session_v3_catalog_encode(
              7U, endpoints, MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
              routes, MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT,
              (MeisoMutableBuffer){catalog, sizeof(catalog) - 1U}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && bytes_are(catalog, 0xa5, sizeof(catalog)));
    return true;
}

static bool test_profile_catalog_shape_and_failures(void)
{
    static const MeisoRuntimeFacet facets[18] = {
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_IMMUTABLE, MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoCompositionSessionV3CatalogConfig config = catalog_config();
    const MeisoProfileRef *v1 = meiso_composition_scene_profile();
    const MeisoProfileRef *v2 = meiso_composition_scene_profile_v2();
    const MeisoProfileRef *v3 = meiso_composition_scene_profile_v3();
    MeisoSessionConfig session = base_session_config();
    MeisoSessionConfig session_before;
    uint8_t capability[MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES];
    uint8_t capability_before[sizeof(capability)];
    MeisoRuntimeEndpointDescriptor endpoints[6];
    MeisoRuntimeEndpointDescriptor endpoints_before[6];
    MeisoRuntimeEndpointDescriptor catalog_alias_endpoints[6];
    MeisoCompositionSessionV3RouteDescriptor routes[18];
    MeisoCompositionSessionV3RouteDescriptor routes_before[18];
    uint8_t catalog[MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE];
    uint8_t catalog_alias[MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE];
    uint8_t catalog_alias_before[sizeof(catalog_alias)];
    const MeisoResourceDescriptor *wifi_resources;
    const MeisoResourceDescriptor *scene_resources;
    const MeisoResourceDescriptor *device_resources;
    const MeisoResourceDescriptor *render_resources;
    const MeisoResourceDescriptor *camera_resources;
    const MeisoResourceDescriptor *expected_resources[18];
    const MeisoEndpointId *expected_endpoint[6];
    const uint64_t expected_incarnation[6] = {
        11U, 12U, 13U, 14U, 15U, 16U
    };
    size_t wifi_count = 0U;
    size_t scene_count = 0U;
    size_t device_count = 0U;
    size_t render_count = 0U;
    size_t camera_count = 0U;
    size_t size = 0U;
    size_t i;
    union {
        max_align_t alignment;
        MeisoCompositionSessionV3CatalogConfig config;
        size_t size;
        uint8_t bytes[MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE];
    } alias;
    union {
        max_align_t alignment;
        MeisoRuntimeEndpointDescriptor endpoints[6];
        MeisoCompositionSessionV3RouteDescriptor routes[18];
        uint8_t bytes[sizeof(routes)];
    } descriptor_alias;
    uint8_t alias_before[sizeof(alias.bytes)];
    uint8_t descriptor_alias_before[sizeof(descriptor_alias.bytes)];

    CHECK(v1->id.high == UINT64_C(0x4b3233305f53434e) &&
          v1->id.low == UINT64_C(0x5f4c41425f563030) && v1->version == 1U);
    CHECK(v2->id.high == v1->id.high && v2->id.low == v1->id.low &&
          v2->version == 2U);
    CHECK(v3->id.high == v1->id.high && v3->id.low == v1->id.low &&
          v3->version == 3U);
    CHECK(MEISO_COMPOSITION_SCENE_PROFILE_V1_RUNTIME_WIRE_VERSION == 2U &&
          MEISO_COMPOSITION_SCENE_PROFILE_V2_RUNTIME_WIRE_VERSION == 3U &&
          MEISO_COMPOSITION_SESSION_V3_RUNTIME_WIRE_VERSION == 4U);
    CHECK(MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT == 3U &&
          MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT == 9U &&
          MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE == 920U &&
          MEISO_COMPOSITION_SESSION_V2_MAX_REGISTRY_ENTRIES == 12U);

    session_before = session;
    CHECK(meiso_composition_session_v3_configure_session(&session) ==
          MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(meiso_profile_ref_equal(session.profile, *v3));
    CHECK(session.limits.max_frame_bytes == 8236U &&
          session.limits.max_runtime_payload_bytes == 8192U &&
          session.limits.max_registry_entries == 24U);
    CHECK(session.local_role == session_before.local_role &&
          session.local_incarnation == session_before.local_incarnation &&
          session.local_nonce.high == session_before.local_nonce.high &&
          session.local_nonce.low == session_before.local_nonce.low &&
          session.local_peer_id.high == session_before.local_peer_id.high &&
          session.expected_peer_id.low == session_before.expected_peer_id.low);
    session = base_session_config();
    session.local_role = 0U;
    session_before = session;
    CHECK(meiso_composition_session_v3_configure_session(&session) ==
          MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG);
    CHECK(memcmp(&session, &session_before, sizeof(session)) == 0);

    CHECK(meiso_composition_session_v3_build_catalog(
        &config, (MeisoMutableBuffer){capability, sizeof(capability)},
        endpoints, 6U, routes, 18U
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    expected_endpoint[0] = meiso_wifi_telemetry_endpoint();
    expected_endpoint[1] = meiso_composition_scene_presentation_endpoint();
    expected_endpoint[2] = meiso_composition_scene_scene_endpoint();
    expected_endpoint[3] = meiso_device_telemetry_endpoint();
    expected_endpoint[4] = meiso_composition_render_media_endpoint();
    expected_endpoint[5] = meiso_composition_camera_media_endpoint();
    for (i = 0U; i < 6U; ++i) {
        CHECK(endpoint_equal(endpoints[i].id, *expected_endpoint[i]));
        CHECK(endpoints[i].incarnation == expected_incarnation[i]);
        CHECK(endpoints[i].capability.data != NULL);
    }
    CHECK(endpoints[0].capability.data == capability &&
          endpoints[3].capability.data == capability +
              MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE &&
          endpoints[4].capability.data == capability +
              MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
              MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE &&
          endpoints[5].capability.data == capability +
              MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
              MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
              MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE);
    CHECK(endpoints[0].capability.size + endpoints[1].capability.size +
              endpoints[2].capability.size + endpoints[3].capability.size +
              endpoints[4].capability.size + endpoints[5].capability.size ==
          MEISO_COMPOSITION_SESSION_V3_CAPABILITY_BYTES);

    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    scene_resources = meiso_composition_scene_resources(&scene_count);
    device_resources = meiso_device_telemetry_resources(&device_count);
    render_resources = meiso_composition_render_media_resources(&render_count);
    camera_resources = meiso_composition_camera_media_resources(&camera_count);
    CHECK(wifi_count == 3U && scene_count == 6U && device_count == 3U &&
          render_count == 3U && camera_count == 3U);
    for (i = 0U; i < 3U; ++i) {
        expected_resources[i] = &wifi_resources[i];
        expected_resources[9U + i] = &device_resources[i];
        expected_resources[12U + i] = &render_resources[i];
        expected_resources[15U + i] = &camera_resources[i];
    }
    for (i = 0U; i < 6U; ++i) {
        expected_resources[3U + i] = &scene_resources[i];
    }
    for (i = 0U; i < 18U; ++i) {
        const size_t endpoint_index = i < 3U ? 0U : i < 5U ? 1U :
            i < 9U ? 2U : i < 12U ? 3U : i < 15U ? 4U : 5U;
        CHECK(endpoint_equal(routes[i].route.endpoint_id,
                             endpoints[endpoint_index].id));
        CHECK(routes[i].route.endpoint_incarnation ==
              expected_incarnation[endpoint_index]);
        CHECK(routes[i].route.facet == facets[i]);
        CHECK(meiso_resource_descriptor_equal(
            &routes[i].route.resource, expected_resources[i]
        ));
        CHECK(routes[i].delivery == (i == 14U || i == 17U ?
            MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA :
            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME));
    }

    CHECK(meiso_composition_session_v3_catalog_encode(
        7U, endpoints, 6U, routes, 18U,
        (MeisoMutableBuffer){catalog, sizeof(catalog)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(size == 2032U &&
          fnv1a64(catalog, size) == UINT64_C(0x534e6b158059f197));
    memset(catalog_alias, 0x6a, sizeof(catalog_alias));
    memcpy(catalog_alias, capability, sizeof(capability));
    memcpy(catalog_alias_before, catalog_alias, sizeof(catalog_alias));
    memcpy(catalog_alias_endpoints, endpoints, sizeof(endpoints));
    catalog_alias_endpoints[0].capability.data = catalog_alias;
    catalog_alias_endpoints[3].capability.data = catalog_alias +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    catalog_alias_endpoints[4].capability.data = catalog_alias +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE;
    catalog_alias_endpoints[5].capability.data = catalog_alias +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE;
    size = 44U;
    CHECK(meiso_composition_session_v3_catalog_encode(
        7U, catalog_alias_endpoints, 6U, routes, 18U,
        (MeisoMutableBuffer){catalog_alias, sizeof(catalog_alias)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(size == 44U &&
          memcmp(catalog_alias, catalog_alias_before,
                 sizeof(catalog_alias)) == 0);
    memset(&alias, 0x6d, sizeof(alias));
    memcpy(alias_before, alias.bytes, sizeof(alias.bytes));
    CHECK(meiso_composition_session_v3_catalog_encode(
        7U, endpoints, 6U, routes, 18U,
        (MeisoMutableBuffer){alias.bytes,
                             MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE},
        &alias.size
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(memcmp(alias.bytes, alias_before, sizeof(alias.bytes)) == 0);
    memset(catalog, 0xa5, sizeof(catalog));
    routes[0].delivery = MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA;
    size = 44U;
    CHECK(meiso_composition_session_v3_catalog_encode(
        7U, endpoints, 6U, routes, 18U,
        (MeisoMutableBuffer){catalog, sizeof(catalog)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG);
    CHECK(size == 0U && bytes_are(catalog, 0xa5, sizeof(catalog)));

    memset(capability, 0xa5, sizeof(capability));
    memset(endpoints, 0xa5, sizeof(endpoints));
    memset(routes, 0xa5, sizeof(routes));
    memcpy(capability_before, capability, sizeof(capability));
    memcpy(endpoints_before, endpoints, sizeof(endpoints));
    memcpy(routes_before, routes, sizeof(routes));
    CHECK(meiso_composition_session_v3_build_catalog(
        &config, (MeisoMutableBuffer){capability, sizeof(capability) - 1U},
        endpoints, 6U, routes, 18U
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(memcmp(capability, capability_before, sizeof(capability)) == 0 &&
          memcmp(endpoints, endpoints_before, sizeof(endpoints)) == 0 &&
          memcmp(routes, routes_before, sizeof(routes)) == 0);
    CHECK(meiso_composition_session_v3_build_catalog(
        &config, (MeisoMutableBuffer){capability, sizeof(capability)},
        endpoints, 5U, routes, 18U
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(memcmp(capability, capability_before, sizeof(capability)) == 0 &&
          memcmp(endpoints, endpoints_before, sizeof(endpoints)) == 0 &&
          memcmp(routes, routes_before, sizeof(routes)) == 0);
    CHECK(meiso_composition_session_v3_build_catalog(
        &config, (MeisoMutableBuffer){capability, sizeof(capability)},
        endpoints, 6U, routes, 17U
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(memcmp(capability, capability_before, sizeof(capability)) == 0 &&
          memcmp(endpoints, endpoints_before, sizeof(endpoints)) == 0 &&
          memcmp(routes, routes_before, sizeof(routes)) == 0);
    config.camera_media_incarnation = 0U;
    CHECK(meiso_composition_session_v3_build_catalog(
        &config, (MeisoMutableBuffer){capability, sizeof(capability)},
        endpoints, 6U, routes, 18U
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG);
    CHECK(memcmp(capability, capability_before, sizeof(capability)) == 0 &&
          memcmp(endpoints, endpoints_before, sizeof(endpoints)) == 0 &&
          memcmp(routes, routes_before, sizeof(routes)) == 0);

    config = catalog_config();
    memset(&alias, 0x5a, sizeof(alias));
    alias.config = config;
    memcpy(alias_before, alias.bytes, sizeof(alias.bytes));
    CHECK(meiso_composition_session_v3_build_catalog(
        &alias.config,
        (MeisoMutableBuffer){alias.bytes + 1U,
                             MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES},
        endpoints, 6U, routes, 18U
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(memcmp(alias.bytes, alias_before, sizeof(alias.bytes)) == 0 &&
          memcmp(endpoints, endpoints_before, sizeof(endpoints)) == 0 &&
          memcmp(routes, routes_before, sizeof(routes)) == 0);

    memset(&descriptor_alias, 0x3c, sizeof(descriptor_alias));
    memcpy(descriptor_alias_before, descriptor_alias.bytes,
           sizeof(descriptor_alias.bytes));
    CHECK(meiso_composition_session_v3_build_catalog(
        &config, (MeisoMutableBuffer){capability, sizeof(capability)},
        descriptor_alias.endpoints, 6U, descriptor_alias.routes,
        18U
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(memcmp(descriptor_alias.bytes, descriptor_alias_before,
                 sizeof(descriptor_alias.bytes)) == 0);
    return true;
}

static MeisoCompositionMediaBinding render_binding(void)
{
    MeisoCompositionMediaBinding value;
    size_t count = 0U;
    const MeisoResourceDescriptor *resources =
        meiso_composition_render_media_resources(&count);
    memset(&value, 0, sizeof(value));
    value.session_id.host_nonce = (MeisoSessionNonce){1U, 2U};
    value.session_id.edge_nonce = (MeisoSessionNonce){3U, 4U};
    value.catalog_generation = 7U;
    value.endpoint_id = *meiso_composition_render_media_endpoint();
    value.endpoint_incarnation = 15U;
    if (resources != NULL && count == 3U) {
        value.resource_id = resources[2].id;
    }
    value.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    value.production_epoch = 17U;
    value.media_stream_epoch = 19U;
    value.channel_nonce = (MeisoSessionNonce){23U, 29U};
    return value;
}

static MeisoCompositionMediaDesired render_desired(void)
{
    return (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 10000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
}

static MeisoCompositionMediaActual render_actual(void)
{
    MeisoCompositionMediaActual value;
    memset(&value, 0, sizeof(value));
    value.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    value.effective = render_desired();
    value.media_stream_epoch = 19U;
    value.channel_nonce = (MeisoSessionNonce){23U, 29U};
    value.producer_dropped_frames = 31U;
    value.queue_depth = 2U;
    value.recovery_generation = 37U;
    return value;
}

static bool test_control_codecs(void)
{
    const MeisoCompositionMediaCapability capability =
        catalog_config().render_media_capability;
    const MeisoCompositionMediaDesired desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 10000U, 0U, 1U, 0U
    };
    const MeisoCompositionMediaOpen open = {
        {{1U, 2U}, {3U, 4U}}, 7U
    };
    MeisoCompositionMediaBinding binding = render_binding();
    uint8_t bytes[MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE];
    MeisoCompositionMediaCapability cap_decoded;
    MeisoCompositionMediaDesired desired_decoded;
    MeisoCompositionMediaOpen open_decoded;
    MeisoCompositionMediaBinding binding_decoded;
    size_t size = 0U;

    CHECK(meiso_composition_media_desired_supported(&capability, &desired));
    CHECK(meiso_composition_media_capability_encode(
              &capability, (MeisoMutableBuffer){bytes, 64U}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK && size == 64U);
    CHECK(meiso_composition_media_capability_decode(
              (MeisoByteView){bytes, 64U}, &cap_decoded
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(cap_decoded.source == capability.source);
    CHECK(cap_decoded.pixel_formats == capability.pixel_formats);
    CHECK(cap_decoded.codecs == capability.codecs);
    CHECK(cap_decoded.min_width == capability.min_width);
    CHECK(cap_decoded.min_height == capability.min_height);
    CHECK(cap_decoded.max_width == capability.max_width);
    CHECK(cap_decoded.max_height == capability.max_height);
    CHECK(cap_decoded.max_fps_millihz == capability.max_fps_millihz);
    CHECK(cap_decoded.max_access_unit_bytes ==
          capability.max_access_unit_bytes);
    CHECK(cap_decoded.max_fragment_payload_bytes ==
          capability.max_fragment_payload_bytes);
    CHECK(cap_decoded.max_bitrate_bps == capability.max_bitrate_bps);
    CHECK(meiso_composition_media_desired_encode(
              &desired, (MeisoMutableBuffer){bytes, 32U}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK && size == 32U);
    CHECK(meiso_composition_media_desired_decode(
              (MeisoByteView){bytes, 32U}, &desired_decoded
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(desired_decoded.pixel_format == desired.pixel_format);
    CHECK(desired_decoded.codec == desired.codec);
    CHECK(desired_decoded.width == desired.width);
    CHECK(desired_decoded.height == desired.height);
    CHECK(desired_decoded.fps_millihz == desired.fps_millihz);
    CHECK(desired_decoded.bitrate_bps == desired.bitrate_bps);
    CHECK(desired_decoded.gop_frames == desired.gop_frames);
    CHECK(desired_decoded.recovery_flags == desired.recovery_flags);
    CHECK(meiso_composition_media_open_encode(
              &open, (MeisoMutableBuffer){bytes, 64U}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK && size == 64U);
    CHECK(meiso_composition_media_open_decode(
              (MeisoByteView){bytes, 64U}, &open_decoded
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(meiso_session_id_equal(open.session_id, open_decoded.session_id));
    CHECK(meiso_composition_media_binding_encode(
              &binding, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK && size == sizeof(bytes));
    CHECK(bytes[88] == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW);
    CHECK(bytes[89] == MEISO_RUNTIME_FACET_DATA);
    CHECK(bytes[90] == MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    CHECK(meiso_composition_media_binding_decode(
              (MeisoByteView){bytes, sizeof(bytes)}, &binding_decoded
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(binding_decoded.production_epoch == binding.production_epoch);
    bytes[91] = 1U;
    CHECK(meiso_composition_media_binding_decode(
              (MeisoByteView){bytes, sizeof(bytes)}, &binding_decoded
          ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    return true;
}

static bool test_media_codec_boundaries_and_aliases(void)
{
    const MeisoCompositionMediaCapability capability =
        catalog_config().render_media_capability;
    const MeisoCompositionMediaDesired desired = render_desired();
    const MeisoCompositionMediaActual actual = render_actual();
    const MeisoCompositionMediaOpen open = {
        {{1U, 2U}, {3U, 4U}}, 7U
    };
    const MeisoCompositionMediaBinding binding = render_binding();
    MeisoCompositionMediaDesired invalid_desired;
    uint8_t capability_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    uint8_t desired_wire[MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE];
    uint8_t actual_wire[MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE];
    uint8_t open_wire[MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE];
    uint8_t binding_wire[MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE];
    uint8_t binding_trailing[MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE + 1U];
    uint8_t output[MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE];
    uint8_t output_before[sizeof(output)];
    MeisoCompositionMediaCapability capability_out;
    MeisoCompositionMediaDesired desired_out;
    MeisoCompositionMediaActual actual_out;
    MeisoCompositionMediaOpen open_out;
    MeisoCompositionMediaBinding binding_out;
    uint8_t typed_before[sizeof(binding_out)];
    size_t size = 0U;

    CHECK(meiso_composition_media_capability_encode(
        &capability,
        (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(meiso_composition_media_desired_encode(
        &desired, (MeisoMutableBuffer){desired_wire, sizeof(desired_wire)},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(meiso_composition_media_actual_encode(
        &actual, (MeisoMutableBuffer){actual_wire, sizeof(actual_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(memcmp(actual_wire, "MACT\0\1\1\0", 8U) == 0 &&
          bytes_are(actual_wire + 72U, 0U, 8U));
    CHECK(meiso_composition_media_actual_decode(
        (MeisoByteView){actual_wire, sizeof(actual_wire)}, &actual_out
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(actual_out.source == actual.source &&
          actual_out.effective.pixel_format == actual.effective.pixel_format &&
          actual_out.effective.codec == actual.effective.codec &&
          actual_out.effective.width == actual.effective.width &&
          actual_out.effective.height == actual.effective.height &&
          actual_out.media_stream_epoch == actual.media_stream_epoch &&
          actual_out.channel_nonce.high == actual.channel_nonce.high &&
          actual_out.channel_nonce.low == actual.channel_nonce.low &&
          actual_out.producer_dropped_frames ==
              actual.producer_dropped_frames &&
          actual_out.queue_depth == actual.queue_depth &&
          actual_out.recovery_generation == actual.recovery_generation);
    CHECK(meiso_composition_media_open_encode(
        &open, (MeisoMutableBuffer){open_wire, sizeof(open_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(meiso_composition_media_binding_encode(
        &binding,
        (MeisoMutableBuffer){binding_wire, sizeof(binding_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);

    memcpy(output, capability_wire, sizeof(capability_wire));
    output[sizeof(capability_wire)] = 0U;
    memset(&capability_out, 0xa5, sizeof(capability_out));
    memcpy(typed_before, &capability_out, sizeof(capability_out));
    CHECK(meiso_composition_media_capability_decode(
        (MeisoByteView){output, sizeof(capability_wire) + 1U}, &capability_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&capability_out, typed_before, sizeof(capability_out)) == 0);
    memcpy(output, desired_wire, sizeof(desired_wire));
    output[sizeof(desired_wire)] = 0U;
    memset(&desired_out, 0xa5, sizeof(desired_out));
    memcpy(typed_before, &desired_out, sizeof(desired_out));
    CHECK(meiso_composition_media_desired_decode(
        (MeisoByteView){output, sizeof(desired_wire) + 1U}, &desired_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&desired_out, typed_before, sizeof(desired_out)) == 0);
    memcpy(output, actual_wire, sizeof(actual_wire));
    output[sizeof(actual_wire)] = 0U;
    memset(&actual_out, 0xa5, sizeof(actual_out));
    memcpy(typed_before, &actual_out, sizeof(actual_out));
    CHECK(meiso_composition_media_actual_decode(
        (MeisoByteView){output, sizeof(actual_wire) + 1U}, &actual_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&actual_out, typed_before, sizeof(actual_out)) == 0);
    memcpy(output, open_wire, sizeof(open_wire));
    output[sizeof(open_wire)] = 0U;
    memset(&open_out, 0xa5, sizeof(open_out));
    memcpy(typed_before, &open_out, sizeof(open_out));
    CHECK(meiso_composition_media_open_decode(
        (MeisoByteView){output, sizeof(open_wire) + 1U}, &open_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&open_out, typed_before, sizeof(open_out)) == 0);
    memcpy(binding_trailing, binding_wire, sizeof(binding_wire));
    binding_trailing[sizeof(binding_wire)] = 0U;
    memset(&binding_out, 0xa5, sizeof(binding_out));
    memcpy(typed_before, &binding_out, sizeof(binding_out));
    CHECK(meiso_composition_media_binding_decode(
        (MeisoByteView){binding_trailing, sizeof(binding_trailing)},
        &binding_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&binding_out, typed_before, sizeof(binding_out)) == 0);

    memset(output, 0xa5, sizeof(output));
    memcpy(output_before, output, sizeof(output));
    size = 99U;
    CHECK(meiso_composition_media_capability_encode(
        &capability,
        (MeisoMutableBuffer){output,
                             MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE - 1U},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && memcmp(output, output_before, sizeof(output)) == 0);
    size = 99U;
    CHECK(meiso_composition_media_desired_encode(
        &desired,
        (MeisoMutableBuffer){output,
                             MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE - 1U},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && memcmp(output, output_before, sizeof(output)) == 0);
    size = 99U;
    CHECK(meiso_composition_media_actual_encode(
        &actual,
        (MeisoMutableBuffer){output,
                             MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE - 1U},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && memcmp(output, output_before, sizeof(output)) == 0);
    size = 99U;
    CHECK(meiso_composition_media_open_encode(
        &open,
        (MeisoMutableBuffer){output,
                             MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE - 1U},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && memcmp(output, output_before, sizeof(output)) == 0);
    size = 99U;
    CHECK(meiso_composition_media_binding_encode(
        &binding,
        (MeisoMutableBuffer){output,
                             MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE - 1U},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && memcmp(output, output_before, sizeof(output)) == 0);
    invalid_desired = desired;
    invalid_desired.width = 0U;
    size = 99U;
    CHECK(meiso_composition_media_desired_encode(
        &invalid_desired,
        (MeisoMutableBuffer){output,
                             MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG);
    CHECK(size == 0U && memcmp(output, output_before, sizeof(output)) == 0);

    memset(&capability_out, 0xa5, sizeof(capability_out));
    memcpy(typed_before, &capability_out, sizeof(capability_out));
    CHECK(meiso_composition_media_capability_decode(
        (MeisoByteView){capability_wire, sizeof(capability_wire) - 1U},
        &capability_out
    ) == MEISO_COMPOSITION_SESSION_V3_TRUNCATED);
    CHECK(memcmp(&capability_out, typed_before, sizeof(capability_out)) == 0);
    capability_wire[44] = 1U;
    CHECK(meiso_composition_media_capability_decode(
        (MeisoByteView){capability_wire, sizeof(capability_wire)},
        &capability_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&capability_out, typed_before, sizeof(capability_out)) == 0);
    CHECK(meiso_composition_media_capability_encode(
        &capability,
        (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);

    memset(&desired_out, 0xa5, sizeof(desired_out));
    memcpy(typed_before, &desired_out, sizeof(desired_out));
    CHECK(meiso_composition_media_desired_decode(
        (MeisoByteView){desired_wire, sizeof(desired_wire) - 1U}, &desired_out
    ) == MEISO_COMPOSITION_SESSION_V3_TRUNCATED);
    CHECK(memcmp(&desired_out, typed_before, sizeof(desired_out)) == 0);
    desired_wire[6] = 1U;
    CHECK(meiso_composition_media_desired_decode(
        (MeisoByteView){desired_wire, sizeof(desired_wire)}, &desired_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&desired_out, typed_before, sizeof(desired_out)) == 0);
    CHECK(meiso_composition_media_desired_encode(
        &desired, (MeisoMutableBuffer){desired_wire, sizeof(desired_wire)},
        &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);

    memset(&actual_out, 0xa5, sizeof(actual_out));
    memcpy(typed_before, &actual_out, sizeof(actual_out));
    CHECK(meiso_composition_media_actual_decode(
        (MeisoByteView){actual_wire, sizeof(actual_wire) - 1U}, &actual_out
    ) == MEISO_COMPOSITION_SESSION_V3_TRUNCATED);
    CHECK(memcmp(&actual_out, typed_before, sizeof(actual_out)) == 0);
    actual_wire[72] = 1U;
    CHECK(meiso_composition_media_actual_decode(
        (MeisoByteView){actual_wire, sizeof(actual_wire)}, &actual_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&actual_out, typed_before, sizeof(actual_out)) == 0);
    CHECK(meiso_composition_media_actual_encode(
        &actual, (MeisoMutableBuffer){actual_wire, sizeof(actual_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);

    memset(&open_out, 0xa5, sizeof(open_out));
    memcpy(typed_before, &open_out, sizeof(open_out));
    CHECK(meiso_composition_media_open_decode(
        (MeisoByteView){open_wire, sizeof(open_wire) - 1U}, &open_out
    ) == MEISO_COMPOSITION_SESSION_V3_TRUNCATED);
    CHECK(memcmp(&open_out, typed_before, sizeof(open_out)) == 0);
    open_wire[48] = 1U;
    CHECK(meiso_composition_media_open_decode(
        (MeisoByteView){open_wire, sizeof(open_wire)}, &open_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&open_out, typed_before, sizeof(open_out)) == 0);
    CHECK(meiso_composition_media_open_encode(
        &open, (MeisoMutableBuffer){open_wire, sizeof(open_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);

    memset(&binding_out, 0xa5, sizeof(binding_out));
    memcpy(typed_before, &binding_out, sizeof(binding_out));
    CHECK(meiso_composition_media_binding_decode(
        (MeisoByteView){binding_wire, sizeof(binding_wire) - 1U}, &binding_out
    ) == MEISO_COMPOSITION_SESSION_V3_TRUNCATED);
    CHECK(memcmp(&binding_out, typed_before, sizeof(binding_out)) == 0);
    binding_wire[91] = 1U;
    CHECK(meiso_composition_media_binding_decode(
        (MeisoByteView){binding_wire, sizeof(binding_wire)}, &binding_out
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&binding_out, typed_before, sizeof(binding_out)) == 0);
    CHECK(meiso_composition_media_binding_encode(
        &binding,
        (MeisoMutableBuffer){binding_wire, sizeof(binding_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);

    CHECK_MEDIA_ALIAS(
        MeisoCompositionMediaCapability, capability,
        meiso_composition_media_capability_encode,
        meiso_composition_media_capability_decode,
        capability_wire, sizeof(capability_wire)
    );
    CHECK_MEDIA_ALIAS(
        MeisoCompositionMediaDesired, desired,
        meiso_composition_media_desired_encode,
        meiso_composition_media_desired_decode,
        desired_wire, sizeof(desired_wire)
    );
    CHECK_MEDIA_ALIAS(
        MeisoCompositionMediaActual, actual,
        meiso_composition_media_actual_encode,
        meiso_composition_media_actual_decode,
        actual_wire, sizeof(actual_wire)
    );
    CHECK_MEDIA_ALIAS(
        MeisoCompositionMediaOpen, open,
        meiso_composition_media_open_encode,
        meiso_composition_media_open_decode,
        open_wire, sizeof(open_wire)
    );
    CHECK_MEDIA_ALIAS(
        MeisoCompositionMediaBinding, binding,
        meiso_composition_media_binding_encode,
        meiso_composition_media_binding_decode,
        binding_wire, sizeof(binding_wire)
    );
    return true;
}

static bool test_frame_codec(void)
{
    const uint8_t fragment[4] = {0x10U, 0x20U, 0x30U, 0x40U};
    MeisoCompositionMediaFrame value;
    MeisoCompositionMediaFrame decoded;
    uint8_t bytes[MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + 4U];
    size_t size = 0U;
    memset(&value, 0, sizeof(value));
    value.binding = render_binding();
    value.frame_sequence = 1U;
    value.source_time = (MeisoTimePoint){5U, 100U};
    value.applied_scene_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    value.applied_scene_state_version = 37U;
    value.active_plan_generation = 41U;
    value.world_generation = 43U;
    value.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    value.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    value.width = 1U;
    value.height = 1U;
    value.access_unit_bytes = 4U;
    value.fragment = (MeisoByteView){fragment, sizeof(fragment)};
    value.flags = MEISO_COMPOSITION_MEDIA_FRAME_BEGIN |
        MEISO_COMPOSITION_MEDIA_FRAME_END |
        MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    CHECK(meiso_composition_media_frame_encode(
              &value, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &size
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(size == sizeof(bytes));
    CHECK(memcmp(bytes, "MFRM\0\x01\0\x07", 8U) == 0);
    CHECK(bytes[128] == MEISO_OWNER_ROLE_HOST);
    CHECK(bytes[200] == 0U && bytes[201] == 0U &&
          bytes[202] == 0U && bytes[203] == 4U);
    CHECK(memcmp(bytes + MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE,
                 fragment, sizeof(fragment)) == 0);
    CHECK(meiso_composition_media_frame_decode(
              (MeisoByteView){bytes, sizeof(bytes)}, &decoded
          ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(decoded.frame_sequence == 1U && decoded.fragment.size == 4U);
    CHECK(memcmp(decoded.fragment.data, fragment, 4U) == 0);
    CHECK(meiso_composition_media_frame_decode(
              (MeisoByteView){bytes, sizeof(bytes) - 1U}, &decoded
          ) == MEISO_COMPOSITION_SESSION_V3_TRUNCATED);
    bytes[224] = 1U;
    CHECK(meiso_composition_media_frame_decode(
              (MeisoByteView){bytes, sizeof(bytes)}, &decoded
          ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    return true;
}

static MeisoCompositionMediaFrame one_pixel_frame(const uint8_t *fragment)
{
    MeisoCompositionMediaFrame value;
    memset(&value, 0, sizeof(value));
    value.binding = render_binding();
    value.frame_sequence = 1U;
    value.source_time = (MeisoTimePoint){5U, 100U};
    value.applied_scene_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    value.applied_scene_state_version = 37U;
    value.active_plan_generation = 41U;
    value.world_generation = 43U;
    value.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    value.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    value.width = 1U;
    value.height = 1U;
    value.access_unit_bytes = 4U;
    value.fragment = (MeisoByteView){fragment, 4U};
    value.flags = MEISO_COMPOSITION_MEDIA_FRAME_BEGIN |
        MEISO_COMPOSITION_MEDIA_FRAME_END |
        MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    return value;
}

static bool test_frame_capacity_alias_and_borrow(void)
{
    const uint8_t fragment[4] = {1U, 2U, 3U, 4U};
    uint8_t max_fragment[MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE];
    MeisoCompositionMediaFrame value = one_pixel_frame(fragment);
    MeisoCompositionMediaFrame decoded;
    uint8_t wire[MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + 4U];
    uint8_t trailing_wire[sizeof(wire) + 1U];
    uint8_t max_wire[MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE +
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE];
    uint8_t before[sizeof(wire)];
    uint8_t decoded_before[sizeof(decoded)];
    size_t size = 0U;
    union {
        max_align_t alignment;
        size_t size;
        MeisoCompositionMediaFrame frame;
        uint8_t bytes[512];
    } overlap;
    uint8_t overlap_before[sizeof(overlap.bytes)];

    memset(wire, 0xa5, sizeof(wire));
    memcpy(before, wire, sizeof(before));
    size = 99U;
    CHECK(meiso_composition_media_frame_encode(
        &value, (MeisoMutableBuffer){wire, sizeof(wire) - 1U}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U && memcmp(wire, before, sizeof(wire)) == 0);
    CHECK(meiso_composition_media_frame_encode(
        &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    memset(&decoded, 0, sizeof(decoded));
    CHECK(meiso_composition_media_frame_decode(
        (MeisoByteView){wire, sizeof(wire)}, &decoded
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(decoded.fragment.data ==
              wire + MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE &&
          decoded.fragment.size == 4U &&
          memcmp(decoded.fragment.data, fragment, 4U) == 0);

    memcpy(trailing_wire, wire, sizeof(wire));
    trailing_wire[sizeof(wire)] = 0U;
    memset(&decoded, 0xa5, sizeof(decoded));
    memcpy(decoded_before, &decoded, sizeof(decoded));
    CHECK(meiso_composition_media_frame_decode(
        (MeisoByteView){trailing_wire, sizeof(trailing_wire)}, &decoded
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&decoded, decoded_before, sizeof(decoded)) == 0);

    memset(max_fragment, 0x7c, sizeof(max_fragment));
    value = one_pixel_frame(max_fragment);
    value.width = 32U;
    value.height = 32U;
    value.access_unit_bytes = sizeof(max_fragment);
    value.fragment.size = sizeof(max_fragment);
    memset(max_wire, 0xa5, sizeof(max_wire));
    memcpy(overlap_before, max_wire, sizeof(overlap_before));
    size = 99U;
    CHECK(meiso_composition_media_frame_encode(
        &value, (MeisoMutableBuffer){max_wire, sizeof(max_wire) - 1U}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_CAPACITY);
    CHECK(size == 0U &&
          memcmp(max_wire, overlap_before, sizeof(overlap_before)) == 0 &&
          bytes_are(max_wire + sizeof(overlap_before), 0xa5,
                    sizeof(max_wire) - sizeof(overlap_before)));
    CHECK(meiso_composition_media_frame_encode(
        &value, (MeisoMutableBuffer){max_wire, sizeof(max_wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(size == sizeof(max_wire));
    CHECK(meiso_composition_media_frame_decode(
        (MeisoByteView){max_wire, sizeof(max_wire)}, &decoded
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    CHECK(decoded.fragment.size == sizeof(max_fragment) &&
          decoded.fragment.data == max_wire +
              MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE &&
          memcmp(decoded.fragment.data, max_fragment,
                 sizeof(max_fragment)) == 0);

    memset(&decoded, 0xa5, sizeof(decoded));
    memcpy(decoded_before, &decoded, sizeof(decoded));
    wire[204] = 1U;
    CHECK(meiso_composition_media_frame_decode(
        (MeisoByteView){wire, sizeof(wire)}, &decoded
    ) == MEISO_COMPOSITION_SESSION_V3_MALFORMED);
    CHECK(memcmp(&decoded, decoded_before, sizeof(decoded)) == 0);
    wire[204] = 0U;

    value = one_pixel_frame(fragment);
    memset(&overlap, 0x3c, sizeof(overlap));
    memcpy(overlap.bytes, fragment, sizeof(fragment));
    memcpy(overlap_before, overlap.bytes, sizeof(overlap_before));
    value.fragment = (MeisoByteView){overlap.bytes, sizeof(fragment)};
    size = 99U;
    CHECK(meiso_composition_media_frame_encode(
        &value, (MeisoMutableBuffer){overlap.bytes, sizeof(wire)}, &size
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(size == 0U &&
          memcmp(overlap.bytes, overlap_before, sizeof(overlap_before)) == 0);

    value = one_pixel_frame(fragment);
    memset(&overlap, 0x5a, sizeof(overlap));
    memcpy(overlap_before, overlap.bytes, sizeof(overlap_before));
    CHECK(meiso_composition_media_frame_encode(
        &value, (MeisoMutableBuffer){overlap.bytes, sizeof(wire)},
        &overlap.size
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(memcmp(overlap.bytes, overlap_before, sizeof(overlap_before)) == 0);

    memset(&overlap, 0x6b, sizeof(overlap));
    memcpy(overlap.bytes, wire, sizeof(wire));
    memcpy(overlap_before, overlap.bytes, sizeof(overlap_before));
    CHECK(meiso_composition_media_frame_decode(
        (MeisoByteView){overlap.bytes, sizeof(wire)},
        &overlap.frame
    ) == MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT);
    CHECK(memcmp(overlap.bytes, overlap_before, sizeof(overlap_before)) == 0);
    return true;
}

int main(void)
{
    if (!test_device_literals() || !test_device_codec_boundaries() ||
        !test_profile_and_catalog() ||
        !test_profile_catalog_shape_and_failures() ||
        !test_control_codecs() ||
        !test_media_codec_boundaries_and_aliases() ||
        !test_frame_codec() || !test_frame_capacity_alias_and_borrow()) {
        return 1;
    }
    puts("composition media wire tests passed");
    return 0;
}
