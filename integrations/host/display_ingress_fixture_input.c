#include <meiso/host/display_ingress_fixture_input.h>

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

typedef struct {
    uint32_t state[8];
    uint8_t block[64];
    uint64_t total_bytes;
    size_t block_size;
} DifiSha256;

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

static uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static void sha256_transform(DifiSha256 *sha, const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = get_u32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
            rotate_right(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
            rotate_right(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 +
            words[index - 7U] + s1;
    }
    a = sha->state[0];
    b = sha->state[1];
    c = sha->state[2];
    d = sha->state[3];
    e = sha->state[4];
    f = sha->state[5];
    g = sha->state[6];
    h = sha->state[7];
    for (index = 0U; index < 64U; ++index) {
        const uint32_t s1 = rotate_right(e, 6U) ^
            rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + s1 + choice +
            constants[index] + words[index];
        const uint32_t s0 = rotate_right(a, 2U) ^
            rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void sha256_init(DifiSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memmove(sha->state, initial, sizeof(initial));
}

static void sha256_update(DifiSha256 *sha, const uint8_t *data, size_t size)
{
    while (size != 0U) {
        const size_t available = sizeof(sha->block) - sha->block_size;
        const size_t take = size < available ? size : available;

        memmove(sha->block + sha->block_size, data, take);
        sha->block_size += take;
        sha->total_bytes += take;
        data += take;
        size -= take;
        if (sha->block_size == sizeof(sha->block)) {
            sha256_transform(sha, sha->block);
            sha->block_size = 0U;
        }
    }
}

static void sha256_final(
    DifiSha256 *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    const uint64_t total_bits = sha->total_bytes * 8U;
    size_t index;

    sha->block[sha->block_size++] = 0x80U;
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        sha256_transform(sha, sha->block);
        sha->block_size = 0U;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (index = 0U; index < 8U; ++index) {
        sha->block[56U + index] =
            (uint8_t)(total_bits >> (56U - index * 8U));
    }
    sha256_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        put_u32(digest + index * 4U, sha->state[index]);
    }
}

bool meiso_display_ingress_host_fixture_input_sha256(
    MeisoByteView payload,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    DifiSha256 sha;
    uint8_t candidate[MEISO_SHA256_DIGEST_SIZE];

    if (!meiso_byte_view_valid(payload) || payload.data == NULL ||
        payload.size == 0U ||
        payload.size > MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES ||
        out_digest == NULL ||
        overlaps(payload.data, payload.size, out_digest,
                 MEISO_SHA256_DIGEST_SIZE)) {
        return false;
    }
    sha256_init(&sha);
    sha256_update(&sha, payload.data, payload.size);
    sha256_final(&sha, candidate);
    memmove(out_digest, candidate, sizeof(candidate));
    return true;
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
