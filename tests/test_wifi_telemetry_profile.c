#include "wifi_telemetry.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                         \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

static const uint8_t CAPABILITY_GOLDEN[
    MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE
] = {
    0x00U, 0x00U, 0x03U, 0xffU,
    0x00U, 0x00U, 0x00U, 0xc8U,
    0x00U, 0x00U, 0x03U, 0xe8U,
    0x00U, 0x00U, 0x00U, 0xc8U
};

static const uint8_t DESIRED_GOLDEN[
    MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
] = {
    0x00U, 0x00U, 0x00U, 0x0fU,
    0x00U, 0x00U, 0x02U, 0x58U
};

static const uint8_t EFFECTIVE_GOLDEN[
    MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
] = {
    0x00U, 0x00U, 0x03U, 0xffU,
    0x00U, 0x00U, 0x03U, 0xe8U
};

static const uint8_t SAMPLE_GOLDEN[
    MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE
] = {
    0x00U, 0x00U, 0x03U, 0xffU,
    0x00U, 0x00U, 0x02U, 0xffU,
    0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0xffU, 0xffU, 0xeaU, 0x84U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U
};

static MeisoWifiTelemetryCapability example_capability(void)
{
    const MeisoWifiTelemetryCapability value = {
        MEISO_WIFI_TELEMETRY_FIELD_ALL,
        200U,
        1000U,
        200U
    };
    return value;
}

static MeisoWifiTelemetryDesiredParameters example_desired(void)
{
    const MeisoWifiTelemetryDesiredParameters value = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM |
            MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES |
            MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES,
        600U
    };
    return value;
}

static MeisoWifiTelemetryEffectiveParameters example_effective(void)
{
    const MeisoWifiTelemetryEffectiveParameters value = {
        MEISO_WIFI_TELEMETRY_FIELD_ALL,
        1000U
    };
    return value;
}

static MeisoWifiTelemetrySample example_sample(void)
{
    MeisoWifiTelemetrySample value;

    memset(&value, 0, sizeof(value));
    value.selected_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    value.present_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL &
        (MeisoWifiTelemetryFieldMask)
            ~MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS;
    value.link_state = true;
    value.signal_mbm = -5500;
    value.rx_bytes = UINT64_C(0x0102030405060708);
    value.tx_bytes = UINT64_C(0x1112131415161718);
    value.rx_packets = UINT64_C(0x2122232425262728);
    value.tx_packets = UINT64_C(0x3132333435363738);
    value.rx_dropped = UINT64_C(0x4142434445464748);
    value.tx_dropped = UINT64_C(0x5152535455565758);
    value.rx_errors = 0U;
    value.tx_errors = UINT64_C(0x6162636465666768);
    return value;
}

static bool capability_equal(
    const MeisoWifiTelemetryCapability *lhs,
    const MeisoWifiTelemetryCapability *rhs
)
{
    return lhs->supported_fields == rhs->supported_fields &&
        lhs->min_period_ms == rhs->min_period_ms &&
        lhs->max_period_ms == rhs->max_period_ms &&
        lhs->period_step_ms == rhs->period_step_ms;
}

static bool desired_equal(
    const MeisoWifiTelemetryDesiredParameters *lhs,
    const MeisoWifiTelemetryDesiredParameters *rhs
)
{
    return lhs->selected_fields == rhs->selected_fields &&
        lhs->sample_period_ms == rhs->sample_period_ms;
}

static bool effective_equal(
    const MeisoWifiTelemetryEffectiveParameters *lhs,
    const MeisoWifiTelemetryEffectiveParameters *rhs
)
{
    return lhs->selected_fields == rhs->selected_fields &&
        lhs->sample_period_ms == rhs->sample_period_ms;
}

static bool sample_equal(
    const MeisoWifiTelemetrySample *lhs,
    const MeisoWifiTelemetrySample *rhs
)
{
    return lhs->selected_fields == rhs->selected_fields &&
        lhs->present_fields == rhs->present_fields &&
        lhs->link_state == rhs->link_state &&
        lhs->signal_mbm == rhs->signal_mbm &&
        lhs->rx_bytes == rhs->rx_bytes &&
        lhs->tx_bytes == rhs->tx_bytes &&
        lhs->rx_packets == rhs->rx_packets &&
        lhs->tx_packets == rhs->tx_packets &&
        lhs->rx_dropped == rhs->rx_dropped &&
        lhs->tx_dropped == rhs->tx_dropped &&
        lhs->rx_errors == rhs->rx_errors &&
        lhs->tx_errors == rhs->tx_errors;
}

static bool test_profile_contract(void)
{
    const MeisoProfileRef *profile = meiso_wifi_telemetry_profile();
    const MeisoEndpointId *endpoint = meiso_wifi_telemetry_endpoint();
    const MeisoSchemaRef *capability_schema =
        meiso_wifi_telemetry_capability_schema();
    const MeisoResourceDescriptor *resources;
    MeisoWifiTelemetryCapability capability = example_capability();
    MeisoWifiTelemetryDesiredParameters desired = example_desired();
    size_t count = 0U;

    CHECK(profile != NULL);
    CHECK(profile->id.high == UINT64_C(0x574946495f544c4d));
    CHECK(profile->id.low == UINT64_C(0x5f50524f46494c45));
    CHECK(profile->version == 1U);
    CHECK(endpoint != NULL);
    CHECK(endpoint->high == UINT64_C(0x574946495f544c4d));
    CHECK(endpoint->low == UINT64_C(0x454e44504f494e54));
    CHECK(capability_schema != NULL);
    CHECK(capability_schema->id.high == UINT64_C(0x574946495f534348));
    CHECK(capability_schema->id.low == UINT64_C(0x4341505f56303030));
    CHECK(capability_schema->version == 1U);

    resources = meiso_wifi_telemetry_resources(&count);
    CHECK(resources != NULL);
    CHECK(count == MEISO_WIFI_TELEMETRY_RESOURCE_COUNT);
    CHECK(resources[0].kind == MEISO_RESOURCE_KIND_STATE);
    CHECK(resources[0].owner == MEISO_OWNER_ROLE_HOST);
    CHECK(resources[0].id.high == UINT64_C(0x574946495f544c4d));
    CHECK(resources[0].id.low == UINT64_C(0x4445535f53544154));
    CHECK(resources[0].schema.id.high == UINT64_C(0x574946495f534348));
    CHECK(resources[0].schema.id.low == UINT64_C(0x4445535f56303030));
    CHECK(resources[0].schema.version == 1U);
    CHECK(resources[0].max_payload_size ==
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE);
    CHECK(resources[1].kind == MEISO_RESOURCE_KIND_STATE);
    CHECK(resources[1].owner == MEISO_OWNER_ROLE_EDGE);
    CHECK(resources[1].id.high == UINT64_C(0x574946495f544c4d));
    CHECK(resources[1].id.low == UINT64_C(0x4143545f53544154));
    CHECK(resources[1].schema.id.high == UINT64_C(0x574946495f534348));
    CHECK(resources[1].schema.id.low == UINT64_C(0x4143545f56303030));
    CHECK(resources[1].schema.version == 1U);
    CHECK(resources[1].max_payload_size ==
        MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE);
    CHECK(resources[2].kind == MEISO_RESOURCE_KIND_STREAM);
    CHECK(resources[2].owner == MEISO_OWNER_ROLE_EDGE);
    CHECK(resources[2].id.high == UINT64_C(0x574946495f544c4d));
    CHECK(resources[2].id.low == UINT64_C(0x444154415f535452));
    CHECK(resources[2].schema.id.high == UINT64_C(0x574946495f534348));
    CHECK(resources[2].schema.id.low == UINT64_C(0x534d505f56303030));
    CHECK(resources[2].schema.version == 1U);
    CHECK(resources[2].max_payload_size ==
        MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE);
    CHECK(meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_DESIRED
    ) == &resources[0]);
    CHECK(meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_ACTUAL
    ) == &resources[1]);
    CHECK(meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_DATA
    ) == &resources[2]);
    CHECK(meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_CAPABILITY
    ) == NULL);
    CHECK(meiso_wifi_telemetry_resource_for_facet(0U) == NULL);

    CHECK(MEISO_WIFI_TELEMETRY_FIELD_ALL == UINT32_C(0x000003ff));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE == UINT32_C(0x00000001));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM == UINT32_C(0x00000002));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES == UINT32_C(0x00000004));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES == UINT32_C(0x00000008));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS == UINT32_C(0x00000010));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS == UINT32_C(0x00000020));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED == UINT32_C(0x00000040));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED == UINT32_C(0x00000080));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS == UINT32_C(0x00000100));
    CHECK(MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS == UINT32_C(0x00000200));
    CHECK(meiso_wifi_telemetry_parameters_supported(
        &capability, &desired
    ));
    desired.selected_fields |= UINT32_C(0x00000400);
    CHECK(!meiso_wifi_telemetry_parameters_supported(
        &capability, &desired
    ));
    desired = example_desired();
    desired.sample_period_ms = 700U;
    CHECK(!meiso_wifi_telemetry_parameters_supported(
        &capability, &desired
    ));
    desired.sample_period_ms = 100U;
    CHECK(!meiso_wifi_telemetry_parameters_supported(
        &capability, &desired
    ));
    desired = example_desired();
    capability.supported_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE;
    CHECK(!meiso_wifi_telemetry_parameters_supported(
        &capability, &desired
    ));
    capability = example_capability();
    capability.max_period_ms = 900U;
    CHECK(!meiso_wifi_telemetry_parameters_supported(
        &capability, &desired
    ));
    return true;
}

static bool test_capability_codec(void)
{
    uint8_t wire[MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE + 1U];
    MeisoWifiTelemetryCapability value = example_capability();
    MeisoWifiTelemetryCapability decoded;
    size_t size = 999U;
    size_t index;

    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_wifi_telemetry_capability_encode(
        &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(CAPABILITY_GOLDEN));
    CHECK(memcmp(wire, CAPABILITY_GOLDEN, size) == 0);
    CHECK(wire[size] == 0xa5U);
    CHECK(meiso_wifi_telemetry_capability_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(capability_equal(&value, &decoded));
    for (index = 0U; index < size; ++index) {
        CHECK(meiso_wifi_telemetry_capability_decode(
            (MeisoByteView){wire, index}, &decoded
        ) == MEISO_WIFI_TELEMETRY_CODEC_TRUNCATED);
    }
    CHECK(meiso_wifi_telemetry_capability_decode(
        (MeisoByteView){wire, size + 1U}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);

    memmove(wire, CAPABILITY_GOLDEN, sizeof(CAPABILITY_GOLDEN));
    wire[2] = 0x07U;
    CHECK(meiso_wifi_telemetry_capability_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, CAPABILITY_GOLDEN, sizeof(CAPABILITY_GOLDEN));
    wire[7] = 0x65U;
    CHECK(meiso_wifi_telemetry_capability_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, CAPABILITY_GOLDEN, sizeof(CAPABILITY_GOLDEN));
    wire[11] = 0x84U;
    CHECK(meiso_wifi_telemetry_capability_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    return true;
}

static bool test_parameter_codecs(void)
{
    uint8_t wire[MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE + 1U];
    MeisoWifiTelemetryDesiredParameters desired = example_desired();
    MeisoWifiTelemetryDesiredParameters decoded_desired;
    MeisoWifiTelemetryEffectiveParameters effective = example_effective();
    MeisoWifiTelemetryEffectiveParameters decoded_effective;
    size_t size = 999U;
    size_t index;

    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_wifi_telemetry_desired_parameters_encode(
        &desired, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(DESIRED_GOLDEN));
    CHECK(memcmp(wire, DESIRED_GOLDEN, size) == 0);
    CHECK(meiso_wifi_telemetry_desired_parameters_decode(
        (MeisoByteView){wire, size}, &decoded_desired
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(desired_equal(&desired, &decoded_desired));
    for (index = 0U; index < size; ++index) {
        CHECK(meiso_wifi_telemetry_desired_parameters_decode(
            (MeisoByteView){wire, index}, &decoded_desired
        ) == MEISO_WIFI_TELEMETRY_CODEC_TRUNCATED);
    }
    CHECK(meiso_wifi_telemetry_desired_parameters_decode(
        (MeisoByteView){wire, size + 1U}, &decoded_desired
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    wire[2] = 0x04U;
    CHECK(meiso_wifi_telemetry_desired_parameters_decode(
        (MeisoByteView){wire, size}, &decoded_desired
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, DESIRED_GOLDEN, sizeof(DESIRED_GOLDEN));
    wire[7] = 0x59U;
    CHECK(meiso_wifi_telemetry_desired_parameters_decode(
        (MeisoByteView){wire, size}, &decoded_desired
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);

    memset(wire, 0xa5, sizeof(wire));
    size = 999U;
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &effective, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(EFFECTIVE_GOLDEN));
    CHECK(memcmp(wire, EFFECTIVE_GOLDEN, size) == 0);
    CHECK(meiso_wifi_telemetry_effective_parameters_decode(
        (MeisoByteView){wire, size}, &decoded_effective
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(effective_equal(&effective, &decoded_effective));
    for (index = 0U; index < size; ++index) {
        CHECK(meiso_wifi_telemetry_effective_parameters_decode(
            (MeisoByteView){wire, index}, &decoded_effective
        ) == MEISO_WIFI_TELEMETRY_CODEC_TRUNCATED);
    }
    CHECK(meiso_wifi_telemetry_effective_parameters_decode(
        (MeisoByteView){wire, size + 1U}, &decoded_effective
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);

    memset(wire, 0, sizeof(wire));
    CHECK(meiso_wifi_telemetry_effective_parameters_decode(
        (MeisoByteView){wire, sizeof(EFFECTIVE_GOLDEN)},
        &decoded_effective
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded_effective.selected_fields == 0U);
    CHECK(decoded_effective.sample_period_ms == 0U);
    effective = decoded_effective;
    size = 999U;
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &effective, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(EFFECTIVE_GOLDEN));
    CHECK(memcmp(wire, "\0\0\0\0\0\0\0\0", size) == 0);
    wire[7] = 100U;
    CHECK(meiso_wifi_telemetry_effective_parameters_decode(
        (MeisoByteView){wire, sizeof(EFFECTIVE_GOLDEN)},
        &decoded_effective
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    return true;
}

static bool test_sample_codec(void)
{
    uint8_t wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE + 1U];
    MeisoWifiTelemetrySample value = example_sample();
    MeisoWifiTelemetrySample decoded;
    uint8_t decoded_before[sizeof(decoded)];
    size_t size = 999U;
    size_t index;

    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_wifi_telemetry_sample_encode(
        &value, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == sizeof(SAMPLE_GOLDEN));
    CHECK(memcmp(wire, SAMPLE_GOLDEN, size) == 0);
    CHECK(wire[size] == 0xa5U);
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(sample_equal(&value, &decoded));
    for (index = 0U; index < size; ++index) {
        CHECK(meiso_wifi_telemetry_sample_decode(
            (MeisoByteView){wire, index}, &decoded
        ) == MEISO_WIFI_TELEMETRY_CODEC_TRUNCATED);
    }
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size + 1U}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);

    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    memset(&decoded, 0xa5, sizeof(decoded));
    memmove(decoded_before, &decoded, sizeof(decoded));
    wire[2] = 0x07U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    CHECK(memcmp(&decoded, decoded_before, sizeof(decoded)) == 0);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[2] = 0x01U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[4] = 0x04U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[8] = 2U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[9] = 1U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[20] = 1U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[7] = (uint8_t)(wire[7] & (uint8_t)~0x02U);
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[79] = 1U;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    memmove(wire, SAMPLE_GOLDEN, sizeof(SAMPLE_GOLDEN));
    wire[16] = 0xffU;
    wire[17] = 0xffU;
    wire[18] = 0xb1U;
    wire[19] = 0xdfU;
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, size}, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_MALFORMED);
    return true;
}

static bool test_encode_failures(void)
{
    uint8_t wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoWifiTelemetryCapability capability = example_capability();
    MeisoWifiTelemetryDesiredParameters desired = example_desired();
    MeisoWifiTelemetryEffectiveParameters effective = example_effective();
    MeisoWifiTelemetrySample sample = example_sample();
    MeisoWifiTelemetrySample sparse_sample;
    size_t size = 999U;

    memset(wire, 0xa5, sizeof(wire));
    CHECK(meiso_wifi_telemetry_capability_encode(
        NULL, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT);
    CHECK(size == 0U && wire[0] == 0xa5U);
    size = 999U;
    CHECK(meiso_wifi_telemetry_capability_encode(
        &capability, (MeisoMutableBuffer){NULL, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT);
    CHECK(size == 0U);
    size = 999U;
    CHECK(meiso_wifi_telemetry_capability_encode(
        &capability,
        (MeisoMutableBuffer){
            wire, MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE - 1U
        },
        &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_CAPACITY);
    CHECK(size == 0U && wire[0] == 0xa5U);
    CHECK(meiso_wifi_telemetry_capability_encode(
        &capability, (MeisoMutableBuffer){wire, sizeof(wire)}, NULL
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT);

    capability.period_step_ms = 300U;
    size = 999U;
    CHECK(meiso_wifi_telemetry_capability_encode(
        &capability, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_VALUE);
    desired.sample_period_ms = 101U;
    CHECK(meiso_wifi_telemetry_desired_parameters_encode(
        &desired, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_VALUE);
    effective.selected_fields = 0U;
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &effective, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_VALUE);
    sample.present_fields &=
        (MeisoWifiTelemetryFieldMask)~MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &sample, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_VALUE);

    memset(&sparse_sample, 0, sizeof(sparse_sample));
    sparse_sample.selected_fields = MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE;
    sparse_sample.present_fields = MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE;
    sparse_sample.link_state = false;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &sparse_sample, (MeisoMutableBuffer){wire, sizeof(wire)}, &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE);

    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){NULL, 0U}, &sample
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT);
    CHECK(meiso_wifi_telemetry_sample_decode(
        (MeisoByteView){wire, sizeof(wire)}, NULL
    ) == MEISO_WIFI_TELEMETRY_CODEC_INVALID_ARGUMENT);
    return true;
}

int main(void)
{
    if (!test_profile_contract() || !test_capability_codec() ||
        !test_parameter_codecs() || !test_sample_codec() ||
        !test_encode_failures()) {
        return 1;
    }
    (void)puts("wifi telemetry profile tests passed");
    return 0;
}
