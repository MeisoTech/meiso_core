#include <meiso/host/display_ingress_fixture_input.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(                                                   \
                stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition                               \
            );                                                               \
            return false;                                                    \
        }                                                                    \
    } while (0)

static const uint8_t PIXEL[4U] = {0x11U, 0x22U, 0x33U, 0xffU};
static const uint8_t PIXEL_SHA256[32U] = {
    0x9aU, 0xadU, 0x15U, 0x2cU, 0xe3U, 0xaeU, 0x5cU, 0x9bU,
    0xa6U, 0xdcU, 0x6aU, 0x41U, 0x5dU, 0x01U, 0xeaU, 0x8cU,
    0xceU, 0x54U, 0x8fU, 0x33U, 0x24U, 0xc7U, 0x42U, 0xa8U,
    0xadU, 0x2cU, 0x97U, 0xbbU, 0x7fU, 0x13U, 0x31U, 0xb9U
};
static const uint8_t ABC_SHA256[32U] = {
    0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
    0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
    0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
    0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
};
static const uint8_t GOLDEN_HEADER[128U] = {
    0x44U, 0x49U, 0x46U, 0x49U, 0x00U, 0x01U, 0x00U, 0x80U,
    0x00U, 0x00U, 0x00U, 0x84U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x9aU, 0xadU, 0x15U, 0x2cU, 0xe3U, 0xaeU, 0x5cU, 0x9bU,
    0xa6U, 0xdcU, 0x6aU, 0x41U, 0x5dU, 0x01U, 0xeaU, 0x8cU,
    0xceU, 0x54U, 0x8fU, 0x33U, 0x24U, 0xc7U, 0x42U, 0xa8U,
    0xadU, 0x2cU, 0x97U, 0xbbU, 0x7fU, 0x13U, 0x31U, 0xb9U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static MeisoDisplayIngressHostFixtureInput fixture_input(void)
{
    MeisoDisplayIngressHostFixtureInput value;

    memset(&value, 0, sizeof(value));
    value.input_width = 1U;
    value.input_height = 1U;
    value.row_pitch_bytes = 4U;
    value.frame.frame_sequence = UINT64_C(0x0102030405060708);
    value.frame.source_time = (MeisoTimePoint){
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728)
    };
    value.frame.producer_dropped_frames =
        UINT64_C(0x3132333435363738);
    value.frame.rgba_frame = (MeisoByteView){PIXEL, sizeof(PIXEL)};
    return value;
}

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static bool header_equal(
    const MeisoDisplayIngressHostFixtureInputHeader *value
)
{
    return value->record_bytes == 132U && value->input_width == 1U &&
        value->input_height == 1U && value->row_pitch_bytes == 4U &&
        value->rgba_bytes == 4U &&
        value->frame_sequence == UINT64_C(0x0102030405060708) &&
        value->source_time.clock_epoch ==
            UINT64_C(0x1112131415161718) &&
        value->source_time.monotonic_ns ==
            UINT64_C(0x2122232425262728) &&
        !value->sender_deadline.present &&
        value->sender_deadline.value.clock_epoch == 0U &&
        value->sender_deadline.value.monotonic_ns == 0U &&
        value->producer_dropped_frames ==
            UINT64_C(0x3132333435363738) &&
        memcmp(value->rgba_sha256, PIXEL_SHA256, sizeof(PIXEL_SHA256)) == 0;
}

static bool test_known_sha_and_golden_header(void)
{
    static const uint8_t ABC[3U] = {'a', 'b', 'c'};
    MeisoDisplayIngressHostFixtureInput input = fixture_input();
    MeisoDisplayIngressHostFixtureInputHeader decoded;
    MeisoDisplayIngressMediaFrame frame;
    uint8_t digest[32U];
    uint8_t encoded[128U];
    size_t encoded_size = 99U;

    memset(&decoded, 0, sizeof(decoded));
    memset(&frame, 0, sizeof(frame));
    memset(digest, 0, sizeof(digest));
    memset(encoded, 0xa5, sizeof(encoded));
    CHECK(meiso_display_ingress_host_fixture_input_sha256(
        (MeisoByteView){ABC, sizeof(ABC)}, digest
    ));
    CHECK(memcmp(digest, ABC_SHA256, sizeof(digest)) == 0);
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){encoded, sizeof(encoded)}, &encoded_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    CHECK(encoded_size == sizeof(encoded));
    CHECK(memcmp(encoded, GOLDEN_HEADER, sizeof(encoded)) == 0);
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &decoded
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    CHECK(header_equal(&decoded));
    CHECK(meiso_display_ingress_host_fixture_input_validate_payload(
        &decoded, (MeisoByteView){PIXEL, sizeof(PIXEL)}, &frame
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    CHECK(frame.frame_sequence == input.frame.frame_sequence);
    CHECK(frame.source_time.clock_epoch == input.frame.source_time.clock_epoch);
    CHECK(frame.source_time.monotonic_ns == input.frame.source_time.monotonic_ns);
    CHECK(!frame.sender_deadline.present);
    CHECK(frame.producer_dropped_frames ==
          input.frame.producer_dropped_frames);
    CHECK(frame.rgba_frame.data == PIXEL && frame.rgba_frame.size == 4U);
    return true;
}

static bool test_present_deadline_round_trip(void)
{
    MeisoDisplayIngressHostFixtureInput input = fixture_input();
    MeisoDisplayIngressHostFixtureInputHeader decoded;
    MeisoDisplayIngressMediaFrame frame;
    uint8_t encoded[128U];
    size_t encoded_size = 0U;

    input.frame.sender_deadline.present = true;
    input.frame.sender_deadline.value = (MeisoTimePoint){
        input.frame.source_time.clock_epoch, 0U
    };
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){encoded, sizeof(encoded)}, &encoded_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    CHECK(encoded_size == sizeof(encoded) && encoded[15U] == 1U);
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){encoded, sizeof(encoded)}, &decoded
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    CHECK(decoded.sender_deadline.present &&
          decoded.sender_deadline.value.clock_epoch ==
              decoded.source_time.clock_epoch &&
          decoded.sender_deadline.value.monotonic_ns == 0U);
    CHECK(meiso_display_ingress_host_fixture_input_validate_payload(
        &decoded, (MeisoByteView){PIXEL, sizeof(PIXEL)}, &frame
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    CHECK(frame.sender_deadline.present);
    return true;
}

static bool test_encode_failures_do_not_write(void)
{
    union {
        MeisoDisplayIngressHostFixtureInput input;
        uint8_t bytes[256U];
    } aliased_input_output;
    union {
        size_t marker;
        uint8_t bytes[16U];
    } payload_out_size_alias;
    MeisoDisplayIngressHostFixtureInput input = fixture_input();
    MeisoDisplayIngressHostFixtureInput before_input;
    uint8_t aliased_input_output_before[sizeof(aliased_input_output)];
    uint8_t payload_out_size_alias_before[sizeof(payload_out_size_alias)];
    uint8_t output[128U];
    uint8_t mutable_pixel[4U] = {1U, 2U, 3U, 0U};
    uint8_t *oversize_payload;
    MeisoDisplayIngressHostFixtureInputStatus status;
    size_t output_size;

    memset(output, 0xa5, sizeof(output));
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        NULL, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){NULL, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input.frame.rgba_frame = (MeisoByteView){NULL, sizeof(PIXEL)};
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    memset(&aliased_input_output, 0, sizeof(aliased_input_output));
    aliased_input_output.input = fixture_input();
    memmove(aliased_input_output_before, aliased_input_output.bytes,
            sizeof(aliased_input_output_before));
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &aliased_input_output.input,
        (MeisoMutableBuffer){
            aliased_input_output.bytes,
            sizeof(aliased_input_output.bytes)
        },
        &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 0U);
    CHECK(memcmp(aliased_input_output.bytes, aliased_input_output_before,
                 sizeof(aliased_input_output_before)) == 0);

    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){
        (const uint8_t *)(const void *)&input, 4U
    };
    before_input = input;
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 0U && memcmp(
        &input, &before_input, sizeof(input)
    ) == 0 && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    before_input = input;
    memset(output, 0xa5, sizeof(output));
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input,
        (MeisoMutableBuffer){output, sizeof(output)},
        (size_t *)(void *)&input.frame.frame_sequence
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(&input, &before_input, sizeof(input)) == 0 &&
          bytes_are(output, 0xa5U, sizeof(output)));

    memset(&payload_out_size_alias, 0, sizeof(payload_out_size_alias));
    payload_out_size_alias.bytes[0U] = 1U;
    payload_out_size_alias.bytes[1U] = 2U;
    payload_out_size_alias.bytes[2U] = 3U;
    payload_out_size_alias.bytes[3U] = UINT8_MAX;
    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){
        payload_out_size_alias.bytes, 4U
    };
    before_input = input;
    memmove(payload_out_size_alias_before, payload_out_size_alias.bytes,
            sizeof(payload_out_size_alias_before));
    memset(output, 0xa5, sizeof(output));
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input,
        (MeisoMutableBuffer){output, sizeof(output)},
        &payload_out_size_alias.marker
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(&input, &before_input, sizeof(input)) == 0 &&
          memcmp(payload_out_size_alias.bytes,
                 payload_out_size_alias_before,
                 sizeof(payload_out_size_alias_before)) == 0 &&
          bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    memset(output, 0xa5, sizeof(output));
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output) - 1U}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_CAPACITY);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    oversize_payload = (uint8_t *)malloc(
        (size_t)MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES + 1U
    );
    CHECK(oversize_payload != NULL);
    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){
        oversize_payload,
        (size_t)MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES + 1U
    };
    output_size = 77U;
    status = meiso_display_ingress_host_fixture_input_header_encode(
        &input,
        (MeisoMutableBuffer){output, sizeof(output) - 1U},
        &output_size
    );
    CHECK(status == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_CAPACITY &&
          output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));
    input.frame.frame_sequence = 0U;
    output_size = 77U;
    status = meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    );
    CHECK(status ==
              MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE &&
          output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));
    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){
        oversize_payload,
        (size_t)MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES + 1U
    };
    input.input_width = 0U;
    output_size = 77U;
    status = meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    );
    CHECK(status ==
              MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE &&
          output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));
    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){
        oversize_payload,
        (size_t)MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES + 1U
    };
    input.row_pitch_bytes = 8U;
    output_size = 77U;
    status = meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    );
    CHECK(status ==
              MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE &&
          output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));
    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){
        oversize_payload,
        (size_t)MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES + 1U
    };
    output_size = 77U;
    status = meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    );
    free(oversize_payload);
    CHECK(status ==
          MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    input.frame.frame_sequence = 0U;
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    input.input_width = MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION + 1U;
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){mutable_pixel,
                                             sizeof(mutable_pixel)};
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_NON_OPAQUE_ALPHA);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    input.frame.rgba_frame = (MeisoByteView){output, 4U};
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input, (MeisoMutableBuffer){output, sizeof(output)}, &output_size
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 0U && bytes_are(output, 0xa5U, sizeof(output)));

    input = fixture_input();
    output_size = 77U;
    CHECK(meiso_display_ingress_host_fixture_input_header_encode(
        &input,
        (MeisoMutableBuffer){output, sizeof(output)},
        (size_t *)(void *)output
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(output_size == 77U && bytes_are(output, 0xa5U, sizeof(output)));
    return true;
}

static bool decode_failure(
    const uint8_t *bytes,
    size_t size,
    MeisoDisplayIngressHostFixtureInputStatus expected
)
{
    MeisoDisplayIngressHostFixtureInputHeader output;
    MeisoDisplayIngressHostFixtureInputHeader before;

    memset(&output, 0xa5, sizeof(output));
    before = output;
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){bytes, size}, &output
    ) == expected);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    return true;
}

static bool test_decode_failure_matrix(void)
{
    union {
        MeisoDisplayIngressHostFixtureInputHeader header;
        uint8_t bytes[128U];
    } alias;
    uint8_t alias_before[128U];
    uint8_t wire[129U];

    memmove(alias.bytes, GOLDEN_HEADER, sizeof(alias.bytes));
    memmove(alias_before, alias.bytes, sizeof(alias_before));
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){alias.bytes, sizeof(alias.bytes)}, &alias.header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(alias.bytes, alias_before, sizeof(alias.bytes)) == 0);

    CHECK(decode_failure(
        NULL, 0U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED
    ));
    CHECK(decode_failure(
        NULL, 1U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT
    ));

    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    CHECK(decode_failure(
        wire, 127U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED
    ));
    wire[128U] = 0U;
    CHECK(decode_failure(
        wire, sizeof(wire),
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_EXTRA_BYTES
    ));

    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[0U] ^= 1U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[5U] = 2U;
    CHECK(decode_failure(
        wire, 128U,
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_UNSUPPORTED_VERSION
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[5U] = 2U;
    wire[7U] = 127U;
    wire[15U] = 2U;
    CHECK(decode_failure(
        wire, 128U,
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_UNSUPPORTED_VERSION
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[7U] = 127U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[15U] = 2U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[127U] = 1U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[19U] = 0U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[11U] ^= 1U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    memset(wire + 28U, UINT8_MAX, 4U);
    CHECK(decode_failure(
        wire, 128U,
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    memset(wire + 32U, 0, 8U);
    memset(wire + 28U, UINT8_MAX, 4U);
    CHECK(decode_failure(
        wire, 128U,
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    memset(wire + 8U, UINT8_MAX, 4U);
    CHECK(decode_failure(
        wire, 128U,
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE
    ));
    memmove(wire, GOLDEN_HEADER, sizeof(GOLDEN_HEADER));
    wire[15U] = 1U;
    CHECK(decode_failure(
        wire, 128U, MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE
    ));
    return true;
}

static bool validate_failure(
    const MeisoDisplayIngressHostFixtureInputHeader *header,
    MeisoByteView payload,
    MeisoDisplayIngressHostFixtureInputStatus expected
)
{
    MeisoDisplayIngressMediaFrame output;
    MeisoDisplayIngressMediaFrame before;

    memset(&output, 0xa5, sizeof(output));
    before = output;
    CHECK(meiso_display_ingress_host_fixture_input_validate_payload(
        header, payload, &output
    ) == expected);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    return true;
}

static bool test_payload_failure_matrix(void)
{
    union {
        MeisoDisplayIngressHostFixtureInputHeader header;
        MeisoDisplayIngressMediaFrame frame;
        uint8_t bytes[sizeof(MeisoDisplayIngressHostFixtureInputHeader)];
    } header_output_alias;
    union {
        MeisoDisplayIngressMediaFrame frame;
        uint8_t bytes[sizeof(MeisoDisplayIngressMediaFrame)];
    } payload_output_alias;
    MeisoDisplayIngressHostFixtureInputHeader header;
    uint8_t alias_before[
        sizeof(MeisoDisplayIngressHostFixtureInputHeader)
    ];
    uint8_t changed[5U];

    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    header_output_alias.header = header;
    memmove(alias_before, header_output_alias.bytes, sizeof(alias_before));
    CHECK(meiso_display_ingress_host_fixture_input_validate_payload(
        &header_output_alias.header,
        (MeisoByteView){PIXEL, sizeof(PIXEL)},
        &header_output_alias.frame
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(
        header_output_alias.bytes, alias_before, sizeof(alias_before)
    ) == 0);
    memset(&header_output_alias, 0, sizeof(header_output_alias));
    header_output_alias.header = header;
    header_output_alias.header.rgba_bytes = UINT32_MAX;
    memmove(alias_before, header_output_alias.bytes, sizeof(alias_before));
    CHECK(meiso_display_ingress_host_fixture_input_validate_payload(
        &header_output_alias.header,
        (MeisoByteView){PIXEL, sizeof(PIXEL)},
        &header_output_alias.frame
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(
        header_output_alias.bytes, alias_before, sizeof(alias_before)
    ) == 0);

    memset(payload_output_alias.bytes, 0xa5,
           sizeof(payload_output_alias.bytes));
    memmove(payload_output_alias.bytes, PIXEL, sizeof(PIXEL));
    memmove(alias_before, payload_output_alias.bytes,
            sizeof(payload_output_alias.bytes));
    CHECK(meiso_display_ingress_host_fixture_input_validate_payload(
        &header,
        (MeisoByteView){payload_output_alias.bytes, sizeof(PIXEL)},
        &payload_output_alias.frame
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT);
    CHECK(memcmp(
        payload_output_alias.bytes,
        alias_before,
        sizeof(payload_output_alias.bytes)
    ) == 0);
    memset(&header_output_alias, 0, sizeof(header_output_alias));
    header_output_alias.header = header;
    memmove(alias_before, header_output_alias.bytes, sizeof(alias_before));
    CHECK(validate_failure(
        &header_output_alias.header,
        (MeisoByteView){header_output_alias.bytes, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT
    ));
    CHECK(memcmp(
        header_output_alias.bytes, alias_before, sizeof(alias_before)
    ) == 0);
    CHECK(validate_failure(
        &header, (MeisoByteView){NULL, 0U},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED
    ));
    CHECK(validate_failure(
        &header, (MeisoByteView){NULL, 1U},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT
    ));
    CHECK(validate_failure(
        &header, (MeisoByteView){PIXEL, 3U},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED
    ));
    header.frame_sequence = 0U;
    CHECK(validate_failure(
        &header, (MeisoByteView){PIXEL, 3U},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE
    ));
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    memmove(changed, PIXEL, sizeof(PIXEL));
    changed[4U] = 0U;
    CHECK(validate_failure(
        &header, (MeisoByteView){changed, sizeof(changed)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_EXTRA_BYTES
    ));
    changed[0U] ^= 1U;
    CHECK(validate_failure(
        &header, (MeisoByteView){changed, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DIGEST_MISMATCH
    ));
    memmove(changed, PIXEL, sizeof(PIXEL));
    changed[3U] = 0U;
    CHECK(validate_failure(
        &header, (MeisoByteView){changed, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DIGEST_MISMATCH
    ));
    memmove(changed, PIXEL, sizeof(PIXEL));
    changed[3U] = 0U;
    memmove(header.rgba_sha256, PIXEL_SHA256, sizeof(header.rgba_sha256));
    CHECK(meiso_display_ingress_host_fixture_input_sha256(
        (MeisoByteView){changed, sizeof(PIXEL)}, header.rgba_sha256
    ));
    CHECK(validate_failure(
        &header, (MeisoByteView){changed, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_NON_OPAQUE_ALPHA
    ));
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    header.frame_sequence = 0U;
    CHECK(validate_failure(
        &header, (MeisoByteView){PIXEL, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE
    ));
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    header.rgba_bytes = UINT32_MAX;
    CHECK(validate_failure(
        &header, (MeisoByteView){PIXEL, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE
    ));
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    header.frame_sequence = 0U;
    header.rgba_bytes = UINT32_MAX;
    CHECK(validate_failure(
        &header, (MeisoByteView){PIXEL, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE
    ));
    CHECK(meiso_display_ingress_host_fixture_input_header_decode(
        (MeisoByteView){GOLDEN_HEADER, sizeof(GOLDEN_HEADER)}, &header
    ) == MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK);
    header.record_bytes = UINT32_MAX;
    CHECK(validate_failure(
        &header, (MeisoByteView){PIXEL, sizeof(PIXEL)},
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE
    ));
    return true;
}

static bool test_sha_alias_and_bounds(void)
{
    uint8_t storage[64U];
    uint8_t before[64U];

    memset(storage, 0xa5, sizeof(storage));
    memmove(before, storage, sizeof(before));
    CHECK(!meiso_display_ingress_host_fixture_input_sha256(
        (MeisoByteView){storage, sizeof(storage)}, storage
    ));
    CHECK(memcmp(storage, before, sizeof(storage)) == 0);
    CHECK(!meiso_display_ingress_host_fixture_input_sha256(
        (MeisoByteView){storage,
            (size_t)MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES + 1U},
        storage + 32U
    ));
    CHECK(memcmp(storage, before, sizeof(storage)) == 0);
    return true;
}

int main(void)
{
    if (!test_known_sha_and_golden_header() ||
        !test_present_deadline_round_trip() ||
        !test_encode_failures_do_not_write() ||
        !test_decode_failure_matrix() ||
        !test_payload_failure_matrix() ||
        !test_sha_alias_and_bounds()) {
        return 1;
    }
    puts("DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TEST_PASSED");
    return 0;
}
