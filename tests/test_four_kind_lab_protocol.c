#include "lab_protocol.h"

#include <stdio.h>
#include <string.h>

typedef union {
    max_align_t alignment;
    uint8_t bytes[MEISO_LAB_MAX_MESSAGE_SIZE];
} WireStorage;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                      \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

static bool content_id_equal(
    const MeisoContentId *lhs,
    const MeisoContentId *rhs
)
{
    return meiso_content_id_equal(lhs, rhs);
}

static bool edge_state_equal(
    const MeisoLabEdgeState *lhs,
    const MeisoLabEdgeState *rhs
)
{
    return lhs->window_id == rhs->window_id &&
        lhs->interface_present == rhs->interface_present &&
        lhs->carrier_present == rhs->carrier_present &&
        lhs->temperature_present == rhs->temperature_present &&
        lhs->corrected_millic == rhs->corrected_millic &&
        lhs->uncertainty_millic == rhs->uncertainty_millic;
}

static bool sample_equal(
    const MeisoLabTelemetrySample *lhs,
    const MeisoLabTelemetrySample *rhs
)
{
    return lhs->window_id == rhs->window_id &&
        lhs->elapsed_ns == rhs->elapsed_ns &&
        lhs->rx_bytes == rhs->rx_bytes &&
        lhs->tx_bytes == rhs->tx_bytes &&
        lhs->rx_packets == rhs->rx_packets &&
        lhs->tx_packets == rhs->tx_packets &&
        lhs->rx_dropped == rhs->rx_dropped &&
        lhs->tx_dropped == rhs->tx_dropped &&
        lhs->signal_mbm == rhs->signal_mbm &&
        lhs->signal_present == rhs->signal_present &&
        lhs->corrected_millic == rhs->corrected_millic &&
        lhs->uncertainty_millic == rhs->uncertainty_millic &&
        lhs->temperature_present == rhs->temperature_present;
}

static MeisoLabEdgeState example_state(void)
{
    MeisoLabEdgeState value;
    memset(&value, 0, sizeof(value));
    value.window_id = UINT64_C(0x0102030405060708);
    value.interface_present = true;
    value.carrier_present = true;
    value.temperature_present = true;
    value.corrected_millic = 50500;
    value.uncertainty_millic = MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
    return value;
}

static MeisoLabTelemetrySample example_sample(void)
{
    MeisoLabTelemetrySample value;
    memset(&value, 0, sizeof(value));
    value.window_id = UINT64_C(0x0102030405060708);
    value.elapsed_ns = UINT64_C(0x1112131415161718);
    value.rx_bytes = UINT64_C(0x2122232425262728);
    value.tx_bytes = UINT64_C(0x3132333435363738);
    value.rx_packets = UINT64_C(0x4142434445464748);
    value.tx_packets = UINT64_C(0x5152535455565758);
    value.rx_dropped = UINT64_C(0x6162636465666768);
    value.tx_dropped = UINT64_C(0x7172737475767778);
    value.signal_mbm = -5500;
    value.signal_present = true;
    value.corrected_millic = 50500;
    value.uncertainty_millic = MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC;
    value.temperature_present = true;
    return value;
}

static MeisoOwnerScope edge_scope(void)
{
    const MeisoOwnerScope value = {
        MEISO_OWNER_ROLE_EDGE, UINT64_C(0x8182838485868788)
    };
    return value;
}

static MeisoActionId action_id(void)
{
    const MeisoActionId value = {
        UINT64_C(0x9192939495969798),
        UINT64_C(0xa1a2a3a4a5a6a7a8)
    };
    return value;
}

static MeisoLabMessage message_for(MeisoLabMessageType type)
{
    MeisoLabMessage message;
    size_t manifest_size;
    const uint8_t *manifest;

    memset(&message, 0, sizeof(message));
    message.type = type;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    switch (type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        message.body.registry.descriptor_count = MEISO_LAB_DESCRIPTOR_COUNT;
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        message.body.registry_accept.descriptor_count =
            MEISO_LAB_DESCRIPTOR_COUNT;
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        message.body.state_actual.owner = edge_scope();
        message.body.state_actual.version = 1U;
        message.body.state_actual.value = example_state();
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        message.body.state_accept.owner = edge_scope();
        message.body.state_accept.version = 1U;
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        message.body.stream_metrics.producer = edge_scope();
        message.body.stream_metrics.epoch = 7U;
        message.body.stream_metrics.sequence = 1U;
        message.body.stream_metrics.source_time =
            (MeisoTimePoint){9U, UINT64_C(0x0102030405060708)};
        message.body.stream_metrics.value = example_sample();
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        message.body.action_reset.id = action_id();
        message.body.action_reset.executor = edge_scope();
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        message.body.action_delivery_accept.id = action_id();
        message.body.action_delivery_accept.executor = edge_scope();
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        message.body.action_result.id = action_id();
        message.body.action_result.executor = edge_scope();
        message.body.action_result.state = MEISO_ACTION_STATE_ACCEPTED;
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        manifest = meiso_lab_capability_manifest(&manifest_size);
        message.body.blob_manifest.content_id =
            *meiso_lab_capability_manifest_content_id();
        message.body.blob_manifest.content = (MeisoByteView){
            manifest, manifest_size
        };
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        message.body.blob_accept.content_id =
            *meiso_lab_capability_manifest_content_id();
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        message.body.close.status = MEISO_LAB_CLOSE_PASS;
        break;
    default:
        break;
    }
    return message;
}

static bool test_profile_constants(void)
{
    static const uint8_t expected_digest[MEISO_SHA256_DIGEST_SIZE] = {
        0x79U, 0x32U, 0xcdU, 0x21U, 0x13U, 0x1bU, 0x0eU, 0xecU,
        0x09U, 0x53U, 0x08U, 0x2cU, 0xd5U, 0x37U, 0x91U, 0x5cU,
        0x89U, 0xc2U, 0x32U, 0x7eU, 0xbbU, 0x1dU, 0x6cU, 0x70U,
        0x77U, 0xabU, 0x88U, 0xacU, 0x6aU, 0x6fU, 0xc9U, 0xe8U
    };
    const MeisoResourceDescriptor *descriptors;
    const MeisoContentId *content_id;
    const uint8_t *manifest;
    size_t descriptor_count = 0U;
    size_t manifest_size = 0U;
    size_t index;

    descriptors = meiso_lab_descriptors(&descriptor_count);
    CHECK(descriptors != NULL && descriptor_count == 4U);
    for (index = 0U; index < descriptor_count; ++index) {
        CHECK(meiso_resource_descriptor_validate(&descriptors[index]) ==
            MEISO_DESCRIPTOR_VALID);
        CHECK(descriptors[index].kind == (MeisoResourceKind)(index + 1U));
        CHECK(descriptors[index].owner == MEISO_OWNER_ROLE_EDGE);
        CHECK(meiso_lab_descriptor(descriptors[index].kind) ==
            &descriptors[index]);
    }
    CHECK(meiso_lab_descriptor(0U) == NULL);
    CHECK(meiso_lab_descriptor(5U) == NULL);
    CHECK(descriptors[0].max_payload_size == MEISO_LAB_EDGE_STATE_WIRE_SIZE);
    CHECK(descriptors[1].max_payload_size ==
        MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE);
    CHECK(descriptors[2].max_payload_size == 8U);

    manifest = meiso_lab_capability_manifest(&manifest_size);
    content_id = meiso_lab_capability_manifest_content_id();
    CHECK(manifest != NULL && manifest_size == 401U);
    CHECK(descriptors[3].max_payload_size == manifest_size);
    CHECK(content_id != NULL && meiso_content_id_valid(content_id));
    CHECK(memcmp(
        content_id->digest, expected_digest, sizeof(expected_digest)
    ) == 0);
    CHECK(memcmp(manifest, "profile=four-kind-lab-v0\n", 25U) == 0);
    CHECK(manifest[manifest_size - 1U] == (uint8_t)'\n');

    CHECK(MEISO_LAB_TCP_PORT == 27640U);
    CHECK(MEISO_LAB_HOST_PEER_ID_HIGH != MEISO_LAB_EDGE_PEER_ID_HIGH);
    CHECK(MEISO_LAB_SESSION_MAX_RUNTIME_PAYLOAD_BYTES ==
        MEISO_LAB_MAX_MESSAGE_SIZE);
    return true;
}

static bool test_typed_payload_golden(void)
{
    static const uint8_t state_golden[MEISO_LAB_EDGE_STATE_WIRE_SIZE] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x0bU, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0xc5U, 0x44U,
        0x00U, 0x00U, 0x0bU, 0xb8U,
        0x00U, 0x00U, 0x00U, 0x00U
    };
    static const uint8_t sample_golden[
        MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE
    ] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
        0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
        0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U,
        0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U,
        0xffU, 0xffU, 0xeaU, 0x84U,
        0x00U, 0x00U, 0xc5U, 0x44U,
        0x00U, 0x00U, 0x0bU, 0xb8U,
        0x03U, 0x00U, 0x00U, 0x00U
    };
    WireStorage wire;
    MeisoLabEdgeState state = example_state();
    MeisoLabEdgeState decoded_state;
    MeisoLabTelemetrySample sample = example_sample();
    MeisoLabTelemetrySample decoded_sample;
    size_t size = 999U;
    size_t index;

    memset(&wire, 0xa5, sizeof(wire));
    CHECK(meiso_lab_edge_state_encode(
        &state,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_OK);
    CHECK(size == sizeof(state_golden));
    CHECK(memcmp(wire.bytes, state_golden, sizeof(state_golden)) == 0);
    CHECK(meiso_lab_edge_state_decode(
        (MeisoByteView){wire.bytes, size}, &decoded_state
    ) == MEISO_LAB_CODEC_OK);
    CHECK(edge_state_equal(&state, &decoded_state));
    for (index = 0U; index < size; ++index) {
        CHECK(meiso_lab_edge_state_decode(
            (MeisoByteView){wire.bytes, index}, &decoded_state
        ) == MEISO_LAB_CODEC_TRUNCATED);
    }
    wire.bytes[9] = 1U;
    CHECK(meiso_lab_edge_state_decode(
        (MeisoByteView){wire.bytes, size}, &decoded_state
    ) == MEISO_LAB_CODEC_MALFORMED);
    wire.bytes[9] = 0U;
    wire.bytes[8] |= 4U;
    CHECK(meiso_lab_edge_state_decode(
        (MeisoByteView){wire.bytes, size}, &decoded_state
    ) == MEISO_LAB_CODEC_MALFORMED);

    memset(&wire, 0xa5, sizeof(wire));
    size = 999U;
    CHECK(meiso_lab_telemetry_sample_encode(
        &sample,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_OK);
    CHECK(size == sizeof(sample_golden));
    CHECK(memcmp(wire.bytes, sample_golden, sizeof(sample_golden)) == 0);
    CHECK(meiso_lab_telemetry_sample_decode(
        (MeisoByteView){wire.bytes, size}, &decoded_sample
    ) == MEISO_LAB_CODEC_OK);
    CHECK(sample_equal(&sample, &decoded_sample));
    wire.bytes[77] = 1U;
    CHECK(meiso_lab_telemetry_sample_decode(
        (MeisoByteView){wire.bytes, size}, &decoded_sample
    ) == MEISO_LAB_CODEC_MALFORMED);

    state.corrected_millic = 125001;
    CHECK(meiso_lab_edge_state_encode(
        &state,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_INVALID_VALUE);
    state = example_state();
    state.uncertainty_millic = 2999U;
    CHECK(meiso_lab_edge_state_encode(
        &state,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_INVALID_VALUE);
    state = example_state();
    state.temperature_present = false;
    CHECK(meiso_lab_edge_state_encode(
        &state,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_INVALID_VALUE);

    sample = example_sample();
    sample.signal_present = false;
    CHECK(meiso_lab_telemetry_sample_encode(
        &sample,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_INVALID_VALUE);
    sample = example_sample();
    sample.temperature_present = false;
    sample.corrected_millic = 0;
    sample.uncertainty_millic = 0U;
    sample.signal_present = false;
    sample.signal_mbm = 0;
    CHECK(meiso_lab_telemetry_sample_encode(
        &sample,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_OK);
    return true;
}

static bool message_roundtrip(
    const MeisoLabMessage *message,
    size_t expected_size,
    MeisoLabMessage *out_decoded,
    WireStorage *wire
)
{
    size_t size = 0U;
    CHECK(meiso_lab_message_encode(
        message,
        (MeisoMutableBuffer){wire->bytes, sizeof(wire->bytes)},
        &size
    ) == MEISO_LAB_CODEC_OK);
    CHECK(size == expected_size);
    CHECK(wire->bytes[0] == (uint8_t)'M');
    CHECK(wire->bytes[1] == (uint8_t)'4');
    CHECK(wire->bytes[2] == (uint8_t)'L');
    CHECK(wire->bytes[3] == (uint8_t)'1');
    CHECK(wire->bytes[4] == MEISO_LAB_WIRE_VERSION);
    CHECK(wire->bytes[5] == message->type);
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire->bytes, size}, out_decoded
    ) == MEISO_LAB_CODEC_OK);
    CHECK(out_decoded->type == message->type);
    CHECK(out_decoded->registry_generation ==
        MEISO_LAB_REGISTRY_GENERATION);
    return true;
}

static bool test_message_golden_and_roundtrip(void)
{
    static const uint8_t state_prefix_golden[] = {
        0x4dU, 0x34U, 0x4cU, 0x31U, 0x01U, 0x03U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x50U,
        0x00U, 0x00U, 0x00U, 0x01U,
        0x4dU, 0x34U, 0x4cU, 0x31U, 0x5fU, 0x52U, 0x45U, 0x53U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U, 0x88U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
    };
    WireStorage wire;
    MeisoLabMessage message;
    MeisoLabMessage decoded;
    const uint8_t *manifest;
    size_t manifest_size;
    size_t index;
    const size_t expected_sizes[11] = {
        196U, 20U, 80U, 56U, 160U, 104U,
        64U, 80U, 473U, 68U, 20U
    };

    for (index = 0U; index < 11U; ++index) {
        message = message_for((MeisoLabMessageType)(index + 1U));
        CHECK(message_roundtrip(
            &message, expected_sizes[index], &decoded, &wire
        ));
        if (message.type == MEISO_LAB_MESSAGE_REGISTRY) {
            CHECK(memcmp(wire.bytes + 16U, "\x00\x00\x00\x04", 4U) == 0);
            CHECK(memcmp(wire.bytes + 20U, "M4L1_RES", 8U) == 0);
            CHECK(wire.bytes[36] == MEISO_RESOURCE_KIND_STATE);
            CHECK(wire.bytes[37] == MEISO_OWNER_ROLE_EDGE);
            CHECK(memcmp(wire.bytes + 40U, "M4L1_SCH", 8U) == 0);
            CHECK(decoded.body.registry.descriptor_count == 4U);
        }
        if (message.type == MEISO_LAB_MESSAGE_STATE_ACTUAL) {
            CHECK(memcmp(
                wire.bytes, state_prefix_golden,
                sizeof(state_prefix_golden)
            ) == 0);
            CHECK(edge_state_equal(
                &decoded.body.state_actual.value,
                &message.body.state_actual.value
            ));
        }
        if (message.type == MEISO_LAB_MESSAGE_STREAM_METRICS) {
            CHECK(sample_equal(
                &decoded.body.stream_metrics.value,
                &message.body.stream_metrics.value
            ));
        }
        if (message.type == MEISO_LAB_MESSAGE_ACTION_RESET) {
            CHECK(memcmp(wire.bytes + 64U, "M4L1_RES", 8U) == 0);
            CHECK(wire.bytes[79] == 2U);
            CHECK(decoded.body.action_reset.id.high == action_id().high);
            CHECK(decoded.body.action_reset.executor.incarnation ==
                edge_scope().incarnation);
        }
        if (message.type == MEISO_LAB_MESSAGE_BLOB_MANIFEST) {
            manifest = meiso_lab_capability_manifest(&manifest_size);
            CHECK(manifest_size == 401U);
            CHECK(memcmp(
                wire.bytes + 72U, manifest, manifest_size
            ) == 0);
            CHECK(content_id_equal(
                &decoded.body.blob_manifest.content_id,
                meiso_lab_capability_manifest_content_id()
            ));
            CHECK(decoded.body.blob_manifest.content.size == manifest_size);
            CHECK(memcmp(
                decoded.body.blob_manifest.content.data,
                manifest,
                manifest_size
            ) == 0);
        }
    }
    return true;
}

static bool test_message_malformed_and_capacity(void)
{
    WireStorage wire;
    WireStorage copy;
    MeisoLabMessage message = message_for(MEISO_LAB_MESSAGE_REGISTRY);
    MeisoLabMessage decoded;
    size_t size = 0U;
    size_t index;

    memset(&wire, 0xa5, sizeof(wire));
    CHECK(meiso_lab_message_encode(
        &message,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_OK);
    copy = wire;
    for (index = 0U; index < size; ++index) {
        CHECK(meiso_lab_message_decode(
            (MeisoByteView){wire.bytes, index}, &decoded
        ) == MEISO_LAB_CODEC_TRUNCATED);
    }
    wire = copy;
    wire.bytes[0] = (uint8_t)'X';
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);
    wire = copy;
    wire.bytes[4] = 2U;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_UNSUPPORTED);
    wire = copy;
    wire.bytes[5] = 99U;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_UNSUPPORTED);
    wire = copy;
    wire.bytes[6] = 1U;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);
    wire = copy;
    wire.bytes[11] = (uint8_t)(size - 1U);
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);
    wire = copy;
    wire.bytes[15] = 2U;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);
    wire = copy;
    wire.bytes[19] = 3U;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);
    wire = copy;
    wire.bytes[36] = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);

    memset(&wire, 0xa5, sizeof(wire));
    size = 777U;
    CHECK(meiso_lab_message_encode(
        &message,
        (MeisoMutableBuffer){wire.bytes, 195U},
        &size
    ) == MEISO_LAB_CODEC_CAPACITY);
    CHECK(size == 777U && wire.bytes[0] == 0xa5U);
    message.registry_generation = 2U;
    CHECK(meiso_lab_message_encode(
        &message,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_INVALID_VALUE);

    message = message_for(MEISO_LAB_MESSAGE_BLOB_MANIFEST);
    message.body.blob_manifest.content_id.digest[0] ^= 1U;
    CHECK(meiso_lab_message_encode(
        &message,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_INVALID_VALUE);
    message = message_for(MEISO_LAB_MESSAGE_BLOB_MANIFEST);
    CHECK(meiso_lab_message_encode(
        &message,
        (MeisoMutableBuffer){wire.bytes, sizeof(wire.bytes)},
        &size
    ) == MEISO_LAB_CODEC_OK);
    wire.bytes[48] ^= 1U;
    CHECK(meiso_lab_message_decode(
        (MeisoByteView){wire.bytes, size}, &decoded
    ) == MEISO_LAB_CODEC_MALFORMED);
    return true;
}

static bool test_order(void)
{
    const MeisoLabSender senders[12] = {
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_HOST,
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_HOST,
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_HOST,
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_EDGE,
        MEISO_LAB_SENDER_HOST,
        MEISO_LAB_SENDER_HOST
    };
    const MeisoLabMessageType types[12] = {
        MEISO_LAB_MESSAGE_REGISTRY,
        MEISO_LAB_MESSAGE_REGISTRY_ACCEPT,
        MEISO_LAB_MESSAGE_STATE_ACTUAL,
        MEISO_LAB_MESSAGE_STATE_ACCEPT,
        MEISO_LAB_MESSAGE_STREAM_METRICS,
        MEISO_LAB_MESSAGE_ACTION_RESET,
        MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT,
        MEISO_LAB_MESSAGE_ACTION_RESULT,
        MEISO_LAB_MESSAGE_ACTION_RESULT,
        MEISO_LAB_MESSAGE_BLOB_MANIFEST,
        MEISO_LAB_MESSAGE_BLOB_ACCEPT,
        MEISO_LAB_MESSAGE_CLOSE
    };
    MeisoLabOrder order;
    MeisoLabMessage message;
    size_t index;

    meiso_lab_order_init(&order);
    for (index = 0U; index < 12U; ++index) {
        message = message_for(types[index]);
        if (index == 8U) {
            message.body.action_result.state = MEISO_ACTION_STATE_SUCCEEDED;
            message.body.action_result.new_window_id =
                UINT64_C(0x1112131415161718);
        }
        CHECK(meiso_lab_order_advance(
            &order, senders[index], &message
        ) == (index == 11U ?
            MEISO_LAB_ORDER_COMPLETE : MEISO_LAB_ORDER_ADVANCED));
    }
    CHECK(order.step == 12U && order.terminal == 1U);
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_HOST, &message
    ) == MEISO_LAB_ORDER_ALREADY_TERMINAL);

    meiso_lab_order_init(&order);
    message = message_for(MEISO_LAB_MESSAGE_REGISTRY);
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_HOST, &message
    ) == MEISO_LAB_ORDER_UNEXPECTED);
    CHECK(order.terminal == 2U);

    meiso_lab_order_init(&order);
    message = message_for(MEISO_LAB_MESSAGE_REGISTRY);
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_EDGE, &message
    ) == MEISO_LAB_ORDER_ADVANCED);
    message = message_for(MEISO_LAB_MESSAGE_REGISTRY_ACCEPT);
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_HOST, &message
    ) == MEISO_LAB_ORDER_ADVANCED);
    message = message_for(MEISO_LAB_MESSAGE_CLOSE);
    message.body.close.status = MEISO_LAB_CLOSE_FAIL;
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_HOST, &message
    ) == MEISO_LAB_ORDER_UNEXPECTED);
    CHECK(order.terminal == 2U);
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_HOST, &message
    ) == MEISO_LAB_ORDER_ALREADY_TERMINAL);

    meiso_lab_order_init(&order);
    for (index = 0U; index < 5U; ++index) {
        message = message_for(types[index]);
        CHECK(meiso_lab_order_advance(
            &order, senders[index], &message
        ) == MEISO_LAB_ORDER_ADVANCED);
    }
    message = message_for(MEISO_LAB_MESSAGE_ACTION_RESET);
    message.body.action_reset.executor.incarnation += 1U;
    CHECK(meiso_lab_order_advance(
        &order, MEISO_LAB_SENDER_HOST, &message
    ) == MEISO_LAB_ORDER_UNEXPECTED);
    CHECK(order.terminal == 2U);
    return true;
}

int main(void)
{
    if (!test_profile_constants() || !test_typed_payload_golden() ||
        !test_message_golden_and_roundtrip() ||
        !test_message_malformed_and_capacity() || !test_order()) {
        return 1;
    }
    (void)puts("four-kind lab protocol passed");
    return 0;
}
