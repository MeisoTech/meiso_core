#ifndef MEISO_HOST_DISPLAY_INGRESS_FIXTURE_INPUT_H
#define MEISO_HOST_DISPLAY_INGRESS_FIXTURE_INPUT_H

#include <meiso/profiles/display_ingress/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One machine-local, one-shot Host fixture input. DIFI/v1 is not a Session,
 * Runtime, DIFR media, CMFB preview, receipt, or network format.
 */
#define MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MAGIC UINT32_C(0x44494649)
#define MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_VERSION 1U
#define MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE 128U
#define MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DEADLINE_PRESENT            \
    (UINT32_C(1) << 0U)
#define MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MAX_RECORD_BYTES            \
    (MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_HEADER_WIRE_SIZE +             \
     MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES)

typedef struct {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t row_pitch_bytes;
    MeisoDisplayIngressMediaFrame frame;
} MeisoDisplayIngressHostFixtureInput;

typedef struct {
    uint32_t record_bytes;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t row_pitch_bytes;
    uint32_t rgba_bytes;
    uint64_t frame_sequence;
    /* Caller-supplied producer/source monotonic timestamp; never local now. */
    MeisoTimePoint source_time;
    MeisoDeadline sender_deadline;
    uint64_t producer_dropped_frames;
    uint8_t rgba_sha256[MEISO_SHA256_DIGEST_SIZE];
} MeisoDisplayIngressHostFixtureInputHeader;

typedef uint8_t MeisoDisplayIngressHostFixtureInputStatus;
enum {
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_OK = 0,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_ARGUMENT = 1,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_INVALID_VALUE = 2,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_CAPACITY = 3,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_TRUNCATED = 4,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_MALFORMED = 5,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_UNSUPPORTED_VERSION = 6,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_FRAME_TOO_LARGE = 7,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_EXTRA_BYTES = 8,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_DIGEST_MISMATCH = 9,
    MEISO_DISPLAY_INGRESS_HOST_FIXTURE_INPUT_NON_OPAQUE_ALPHA = 10
};

/*
 * Encodes exactly the 128-byte canonical big-endian header. The SHA-256 at
 * bytes 80..111 is calculated from input->frame.rgba_frame by this codec; the
 * caller does not supply a second digest truth. No payload byte is copied.
 * Typed input, borrowed RGBA bytes, output and out_size must all be mutually
 * disjoint. The complete typed value, metadata, payload length and alpha are
 * validated before any output byte changes. Failure sets a safely disjoint
 * out_size to zero and leaves output bytes unchanged.
 */
MeisoDisplayIngressHostFixtureInputStatus
meiso_display_ingress_host_fixture_input_header_encode(
    const MeisoDisplayIngressHostFixtureInput *input,
    MeisoMutableBuffer output,
    size_t *out_size
);

/*
 * Input must be exactly one 128-byte header. Failure leaves a disjoint output
 * byte-exact unchanged. The returned header owns no payload bytes.
 */
MeisoDisplayIngressHostFixtureInputStatus
meiso_display_ingress_host_fixture_input_header_decode(
    MeisoByteView input,
    MeisoDisplayIngressHostFixtureInputHeader *out_header
);

/*
 * Revalidates the complete caller-supplied header, then validates exact
 * payload length, SHA-256 and opaque RGBA alpha. The returned frame borrows
 * payload, whose owner must outlive every use. Header, payload and out_frame
 * must be mutually disjoint. Failure leaves a disjoint out_frame unchanged.
 */
MeisoDisplayIngressHostFixtureInputStatus
meiso_display_ingress_host_fixture_input_validate_payload(
    const MeisoDisplayIngressHostFixtureInputHeader *header,
    MeisoByteView payload,
    MeisoDisplayIngressMediaFrame *out_frame
);

/*
 * Fixture identity helper. Payload must be nonempty, no larger than the v1
 * 64 MiB payload limit, and disjoint from out_digest. Success writes one
 * SHA-256; failure writes none.
 */
bool meiso_display_ingress_host_fixture_input_sha256(
    MeisoByteView payload,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
);

#ifdef __cplusplus
}
#endif

#endif
