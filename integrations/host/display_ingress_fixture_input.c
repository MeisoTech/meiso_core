#include <meiso/host/display_ingress_fixture_input.h>
#include "../../profiles/display_ingress/display_ingress_profile_private.h"

#include <limits.h>
#include <string.h>

enum {
    DIFI_MAGIC_OFFSET = 0U,
    DIFI_VERSION_OFFSET = 4U,
    DIFI_HEADER_BYTES_OFFSET = 6U,
    DIFI_RECORD_BYTES_OFFSET = 8U,
    DIFI_FLAGS_OFFSET = 12U,
    DIFI_WIDTH_OFFSET = 16U,
    DIFI_HEIGHT_OFFSET = 20U,
    DIFI_ROW_PITCH_OFFSET = 24U,
    DIFI_RGBA_BYTES_OFFSET = 28U,
    DIFI_FRAME_SEQUENCE_OFFSET = 32U,
    DIFI_SOURCE_EPOCH_OFFSET = 40U,
    DIFI_SOURCE_TIME_OFFSET = 48U,
    DIFI_DEADLINE_EPOCH_OFFSET = 56U,
    DIFI_DEADLINE_TIME_OFFSET = 64U,
    DIFI_PRODUCER_DROPS_OFFSET = 72U,
    DIFI_SHA256_OFFSET = 80U,
    DIFI_RESERVED_OFFSET = 112U,
    DIFI_RESERVED_BYTES = 16U
};

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
}

static uint32_t get_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
        ((uint32_t)bytes[1] << 16U) |
        ((uint32_t)bytes[2] << 8U) |
        (uint32_t)bytes[3];
}

static uint64_t get_u64(const uint8_t *bytes)
{
    return ((uint64_t)get_u32(bytes) << 32U) | get_u32(bytes + 4U);
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    put_u32(bytes, (uint32_t)(value >> 32U));
    put_u32(bytes + 4U, (uint32_t)value);
}

static bool range_valid(const void *data, size_t size)
{
    const uintptr_t start = (uintptr_t)data;

    return size == 0U ||
        (data != NULL && size - 1U <= UINTPTR_MAX - start);
}

static bool overlaps(
    const void *left,
    size_t left_size,
    const void *right,
    size_t right_size
)
{
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;

    if (left_size == 0U || right_size == 0U) {
        return false;
    }
    if (!range_valid(left, left_size) || !range_valid(right, right_size)) {
        return true;
    }
    return left_start <= right_start + right_size - 1U &&
        right_start <= left_start + left_size - 1U;
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

static bool rgba_opaque(MeisoByteView payload)
{
    size_t offset;

    for (offset = 3U; offset < payload.size; offset += 4U) {
        if (payload.data[offset] != UINT8_MAX) {
            return false;
        }
    }
    return true;
}

bool meiso_display_ingress_host_fixture_input_sha256(
    MeisoByteView payload,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    return meiso_display_ingress_private_payload_sha256(payload, out_digest);
}

static MeisoDisplayIngressHostFixtureInputStatus validate_header_value(
    const MeisoDisplayIngressHostFixtureInputHeader *header
)
{
    uint64_t row_pitch;
    uint64_t rgba_bytes;
    uint64_t record_bytes;

    if (header->input_width > MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION ||
        header->input_height > MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION ||
        header->rgba_bytes >
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES ||
        header->record_bytes >
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MAX_RECORD_BYTES) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE;
    }
    row_pitch = (uint64_t)header->input_width * UINT64_C(4);
    rgba_bytes = row_pitch * (uint64_t)header->input_height;
    record_bytes = rgba_bytes +
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE;
    if (rgba_bytes > MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES ||
        record_bytes >
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MAX_RECORD_BYTES) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE;
    }
    if (header->input_width == 0U || header->input_height == 0U ||
        header->frame_sequence == 0U ||
        !meiso_time_point_valid(header->source_time) ||
        !meiso_deadline_valid(header->sender_deadline) ||
        (!header->sender_deadline.present &&
         (header->sender_deadline.value.clock_epoch != 0U ||
          header->sender_deadline.value.monotonic_ns != 0U)) ||
        (header->sender_deadline.present &&
         header->sender_deadline.value.clock_epoch !=
             header->source_time.clock_epoch) ||
        header->row_pitch_bytes != (uint32_t)row_pitch ||
        header->rgba_bytes != (uint32_t)rgba_bytes ||
        header->record_bytes != (uint32_t)record_bytes) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE;
    }
    return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK;
}

MeisoDisplayIngressHostFixtureInputStatus
meiso_display_ingress_host_fixture_input_header_encode(
    const MeisoDisplayIngressHostFixtureInput *input,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoDisplayIngressHostFixtureInputHeader header;
    MeisoDisplayIngressHostFixtureInputStatus status;
    uint8_t candidate[MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE];
    uint32_t flags = 0U;

    if (out_size == NULL || !range_valid(out_size, sizeof(*out_size)) ||
        (output.data != NULL &&
         overlaps(output.data, output.capacity, out_size,
                  sizeof(*out_size))) ||
        (input != NULL &&
         overlaps(input, sizeof(*input), out_size, sizeof(*out_size))) ||
        (input != NULL && input->frame.rgba_frame.data != NULL &&
         overlaps(input->frame.rgba_frame.data,
                  input->frame.rgba_frame.size,
                  out_size,
                  sizeof(*out_size)))) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (input == NULL || !meiso_mutable_buffer_valid(output) ||
        output.data == NULL ||
        !meiso_byte_view_valid(input->frame.rgba_frame) ||
        input->frame.rgba_frame.data == NULL ||
        overlaps(input, sizeof(*input), output.data, output.capacity) ||
        overlaps(input->frame.rgba_frame.data, input->frame.rgba_frame.size,
                 input, sizeof(*input)) ||
        overlaps(input->frame.rgba_frame.data, input->frame.rgba_frame.size,
                 output.data, output.capacity)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_CAPACITY;
    }
    if (input->input_width > MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION ||
        input->input_height > MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION ||
        input->frame.rgba_frame.size >
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE;
    }
    memset(&header, 0, sizeof(header));
    header.input_width = input->input_width;
    header.input_height = input->input_height;
    header.row_pitch_bytes = input->row_pitch_bytes;
    header.frame_sequence = input->frame.frame_sequence;
    header.source_time = input->frame.source_time;
    header.sender_deadline = input->frame.sender_deadline;
    header.producer_dropped_frames = input->frame.producer_dropped_frames;
    header.rgba_bytes = (uint32_t)input->frame.rgba_frame.size;
    header.record_bytes =
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE +
        header.rgba_bytes;
    status = validate_header_value(&header);
    if (status != MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK) {
        return status;
    }
    if (!rgba_opaque(input->frame.rgba_frame)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_NON_OPAQUE_ALPHA;
    }
    if (!meiso_display_ingress_host_fixture_input_sha256(
            input->frame.rgba_frame, header.rgba_sha256)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT;
    }

    memset(candidate, 0, sizeof(candidate));
    put_u32(candidate + DIFI_MAGIC_OFFSET,
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MAGIC);
    put_u16(candidate + DIFI_VERSION_OFFSET,
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_VERSION);
    put_u16(candidate + DIFI_HEADER_BYTES_OFFSET,
            MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE);
    put_u32(candidate + DIFI_RECORD_BYTES_OFFSET, header.record_bytes);
    if (header.sender_deadline.present) {
        flags = MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DEADLINE_PRESENT;
    }
    put_u32(candidate + DIFI_FLAGS_OFFSET, flags);
    put_u32(candidate + DIFI_WIDTH_OFFSET, header.input_width);
    put_u32(candidate + DIFI_HEIGHT_OFFSET, header.input_height);
    put_u32(candidate + DIFI_ROW_PITCH_OFFSET, header.row_pitch_bytes);
    put_u32(candidate + DIFI_RGBA_BYTES_OFFSET, header.rgba_bytes);
    put_u64(candidate + DIFI_FRAME_SEQUENCE_OFFSET, header.frame_sequence);
    put_u64(candidate + DIFI_SOURCE_EPOCH_OFFSET,
            header.source_time.clock_epoch);
    put_u64(candidate + DIFI_SOURCE_TIME_OFFSET,
            header.source_time.monotonic_ns);
    put_u64(candidate + DIFI_DEADLINE_EPOCH_OFFSET,
            header.sender_deadline.value.clock_epoch);
    put_u64(candidate + DIFI_DEADLINE_TIME_OFFSET,
            header.sender_deadline.value.monotonic_ns);
    put_u64(candidate + DIFI_PRODUCER_DROPS_OFFSET,
            header.producer_dropped_frames);
    memmove(candidate + DIFI_SHA256_OFFSET, header.rgba_sha256,
            sizeof(header.rgba_sha256));
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK;
}

MeisoDisplayIngressHostFixtureInputStatus
meiso_display_ingress_host_fixture_input_header_decode(
    MeisoByteView input,
    MeisoDisplayIngressHostFixtureInputHeader *out_header
)
{
    MeisoDisplayIngressHostFixtureInputHeader candidate;
    MeisoDisplayIngressHostFixtureInputStatus status;
    uint32_t flags;

    if (!meiso_byte_view_valid(input) || out_header == NULL ||
        overlaps(input.data, input.size, out_header, sizeof(*out_header))) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT;
    }
    if (input.size <
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED;
    }
    if (input.size >
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_EXTRA_BYTES;
    }
    if (get_u32(input.data + DIFI_MAGIC_OFFSET) !=
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MAGIC) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED;
    }
    if (get_u16(input.data + DIFI_VERSION_OFFSET) !=
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_VERSION) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_UNSUPPORTED_VERSION;
    }
    if (get_u16(input.data + DIFI_HEADER_BYTES_OFFSET) !=
        MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED;
    }
    flags = get_u32(input.data + DIFI_FLAGS_OFFSET);
    if ((flags &
         ~MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DEADLINE_PRESENT) != 0U ||
        !bytes_zero(input.data + DIFI_RESERVED_OFFSET,
                    DIFI_RESERVED_BYTES)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.record_bytes = get_u32(input.data + DIFI_RECORD_BYTES_OFFSET);
    candidate.input_width = get_u32(input.data + DIFI_WIDTH_OFFSET);
    candidate.input_height = get_u32(input.data + DIFI_HEIGHT_OFFSET);
    candidate.row_pitch_bytes = get_u32(input.data + DIFI_ROW_PITCH_OFFSET);
    candidate.rgba_bytes = get_u32(input.data + DIFI_RGBA_BYTES_OFFSET);
    candidate.frame_sequence = get_u64(input.data + DIFI_FRAME_SEQUENCE_OFFSET);
    candidate.source_time.clock_epoch =
        get_u64(input.data + DIFI_SOURCE_EPOCH_OFFSET);
    candidate.source_time.monotonic_ns =
        get_u64(input.data + DIFI_SOURCE_TIME_OFFSET);
    candidate.sender_deadline.present =
        (flags & MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DEADLINE_PRESENT) !=
        0U;
    candidate.sender_deadline.value.clock_epoch =
        get_u64(input.data + DIFI_DEADLINE_EPOCH_OFFSET);
    candidate.sender_deadline.value.monotonic_ns =
        get_u64(input.data + DIFI_DEADLINE_TIME_OFFSET);
    candidate.producer_dropped_frames =
        get_u64(input.data + DIFI_PRODUCER_DROPS_OFFSET);
    memmove(candidate.rgba_sha256, input.data + DIFI_SHA256_OFFSET,
            sizeof(candidate.rgba_sha256));
    status = validate_header_value(&candidate);
    if (status != MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK) {
        return status;
    }
    *out_header = candidate;
    return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK;
}

MeisoDisplayIngressHostFixtureInputStatus
meiso_display_ingress_host_fixture_input_validate_payload(
    const MeisoDisplayIngressHostFixtureInputHeader *header,
    MeisoByteView payload,
    MeisoDisplayIngressMediaFrame *out_frame
)
{
    MeisoDisplayIngressHostFixtureInputStatus status;
    MeisoDisplayIngressMediaFrame candidate;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    if (header == NULL || !meiso_byte_view_valid(payload) ||
        out_frame == NULL ||
        overlaps(header, sizeof(*header), payload.data, payload.size) ||
        overlaps(header, sizeof(*header), out_frame, sizeof(*out_frame)) ||
        overlaps(payload.data, payload.size, out_frame, sizeof(*out_frame))) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT;
    }
    status = validate_header_value(header);
    if (status != MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK) {
        return status;
    }
    if (payload.size < header->rgba_bytes) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED;
    }
    if (payload.size > header->rgba_bytes) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_EXTRA_BYTES;
    }
    if (!meiso_display_ingress_host_fixture_input_sha256(payload, digest)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT;
    }
    if (memcmp(digest, header->rgba_sha256, sizeof(digest)) != 0) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DIGEST_MISMATCH;
    }
    if (!rgba_opaque(payload)) {
        return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_NON_OPAQUE_ALPHA;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.frame_sequence = header->frame_sequence;
    candidate.source_time = header->source_time;
    candidate.sender_deadline = header->sender_deadline;
    candidate.producer_dropped_frames = header->producer_dropped_frames;
    candidate.rgba_frame = payload;
    *out_frame = candidate;
    return MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK;
}
