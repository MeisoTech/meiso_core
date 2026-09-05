#include "display_ingress_profile_private.h"

#include <limits.h>
#include <string.h>

/* Canonical Profile-local suffix and evidence codecs. */

bool meiso_display_ingress_private_ranges_overlap(
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

bool meiso_display_ingress_private_bytes_zero(
    const uint8_t *bytes,
    size_t size
)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

void meiso_display_ingress_private_put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

void meiso_display_ingress_private_put_u64(uint8_t *out, uint64_t value)
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

uint32_t meiso_display_ingress_private_get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
        ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

uint64_t meiso_display_ingress_private_get_u64(const uint8_t *in)
{
    return ((uint64_t)in[0] << 56U) | ((uint64_t)in[1] << 48U) |
        ((uint64_t)in[2] << 40U) | ((uint64_t)in[3] << 32U) |
        ((uint64_t)in[4] << 24U) | ((uint64_t)in[5] << 16U) |
        ((uint64_t)in[6] << 8U) | (uint64_t)in[7];
}

bool meiso_display_ingress_private_capability_valid(
    const MeisoDisplayIngressCapability *value
)
{
    return value != NULL && value->max_input_width >= 1U &&
        value->max_input_width <= MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION &&
        value->max_input_height >= 1U &&
        value->max_input_height <= MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION &&
        value->max_frame_rate_millihz >= 1U &&
        value->max_frame_rate_millihz <=
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_RATE_MILLIHZ &&
        value->max_frame_bytes >= 4U &&
        value->max_frame_bytes <= MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES &&
        value->max_fragment_payload_bytes >= 1U &&
        value->max_fragment_payload_bytes <= value->max_frame_bytes &&
        value->evidence_classes != 0U &&
        (value->evidence_classes &
         (MeisoDisplayIngressEvidenceClassMask)
             ~MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_ALL) == 0U &&
        (value->deadline_policy == MEISO_STREAM_DEADLINE_UNSUPPORTED ||
         value->deadline_policy == MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE);
}

bool meiso_display_ingress_private_desired_valid(
    const MeisoDisplayIngressDesired *value
)
{
    return value != NULL && value->input_width >= 1U &&
        value->input_width <= MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION &&
        value->input_height >= 1U &&
        value->input_height <= MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION &&
        value->maximum_frame_rate_millihz >= 1U &&
        value->maximum_frame_rate_millihz <=
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_RATE_MILLIHZ;
}

bool meiso_display_ingress_private_actual_valid(
    const MeisoDisplayIngressActual *value
)
{
    return value != NULL && value->effective_input_width >= 1U &&
        value->effective_input_width <=
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION &&
        value->effective_input_height >= 1U &&
        value->effective_input_height <=
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION &&
        value->effective_maximum_frame_rate_millihz >= 1U &&
        value->effective_maximum_frame_rate_millihz <=
            MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_RATE_MILLIHZ &&
        meiso_session_nonce_valid(value->channel_nonce);
}

bool meiso_display_ingress_private_evidence_valid(
    const MeisoDisplayIngressPresentationEvidence *value
)
{
    if (value == NULL || value->output_width < 1U ||
        value->output_width > MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION ||
        value->output_height < 1U ||
        value->output_height > MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION ||
        value->media_stream_epoch == 0U ||
        value->displayed_frame_sequence == 0U) {
        return false;
    }
    if (value->evidence_class == MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED) {
        return value->connector_kind ==
            MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED;
    }
    return value->evidence_class ==
            MEISO_DISPLAY_INGRESS_EVIDENCE_DRM_KMS_PAGE_FLIP &&
        (value->connector_kind == MEISO_DISPLAY_INGRESS_CONNECTOR_HDMI ||
         value->connector_kind == MEISO_DISPLAY_INGRESS_CONNECTOR_DSI);
}

bool meiso_display_ingress_desired_supported(
    const MeisoDisplayIngressCapability *capability,
    const MeisoDisplayIngressDesired *desired
)
{
    uint64_t frame_bytes;

    if (!meiso_display_ingress_private_capability_valid(capability) ||
        !meiso_display_ingress_private_desired_valid(desired) ||
        desired->input_width > capability->max_input_width ||
        desired->input_height > capability->max_input_height ||
        desired->maximum_frame_rate_millihz >
            capability->max_frame_rate_millihz) {
        return false;
    }
    frame_bytes = (uint64_t)desired->input_width * desired->input_height * 4U;
    return frame_bytes <= capability->max_frame_bytes;
}

bool meiso_display_ingress_actual_matches_desired(
    const MeisoDisplayIngressCapability *capability,
    const MeisoDisplayIngressDesired *desired,
    const MeisoDisplayIngressActual *actual
)
{
    return meiso_display_ingress_desired_supported(capability, desired) &&
        meiso_display_ingress_private_actual_valid(actual) &&
        actual->effective_input_width == desired->input_width &&
        actual->effective_input_height == desired->input_height &&
        actual->effective_maximum_frame_rate_millihz ==
            desired->maximum_frame_rate_millihz;
}

bool meiso_display_ingress_evidence_supported(
    const MeisoDisplayIngressCapability *capability,
    const MeisoDisplayIngressPresentationEvidence *evidence,
    uint64_t containing_production_epoch
)
{
    uint32_t bit;

    if (!meiso_display_ingress_private_capability_valid(capability) ||
        !meiso_display_ingress_private_evidence_valid(evidence) ||
        evidence->media_stream_epoch != containing_production_epoch) {
        return false;
    }
    bit = evidence->evidence_class ==
            MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED ?
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_SIMULATED :
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP;
    return (capability->evidence_classes & bit) != 0U;
}

static bool encode_arguments_alias(
    const void *value,
    size_t value_size,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    return meiso_display_ingress_private_ranges_overlap(
            value, value_size, output.data, output.capacity
        ) || meiso_display_ingress_private_ranges_overlap(
            value, value_size, out_size, sizeof(*out_size)
        ) || meiso_display_ingress_private_ranges_overlap(
            output.data, output.capacity, out_size, sizeof(*out_size)
        );
}

static MeisoDisplayIngressProfileStatus decode_arguments_status(
    MeisoByteView input,
    void *out_value,
    size_t out_value_size,
    size_t expected_size
)
{
    if (out_value == NULL || (input.data == NULL && input.size != 0U)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_display_ingress_private_ranges_overlap(
            input.data, input.size, out_value, out_value_size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (input.size < expected_size) {
        return MEISO_DISPLAY_INGRESS_PROFILE_TRUNCATED;
    }
    if (input.size > expected_size) {
        return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
    }
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_capability_encode(
    const MeisoDisplayIngressCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t candidate[MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE];

    if (out_size == NULL || encode_arguments_alias(
            value, value == NULL ? 0U : sizeof(*value), output, out_size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (value == NULL || output.data == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_display_ingress_private_capability_valid(value)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    }
    memset(candidate, 0, sizeof(candidate));
    meiso_display_ingress_private_put_u32(candidate, value->max_input_width);
    meiso_display_ingress_private_put_u32(
        candidate + 4U, value->max_input_height
    );
    meiso_display_ingress_private_put_u32(
        candidate + 8U, value->max_frame_rate_millihz
    );
    meiso_display_ingress_private_put_u32(
        candidate + 12U, value->max_frame_bytes
    );
    meiso_display_ingress_private_put_u32(
        candidate + 16U, value->max_fragment_payload_bytes
    );
    meiso_display_ingress_private_put_u32(
        candidate + 20U, value->evidence_classes
    );
    candidate[24U] = value->deadline_policy;
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_capability_decode(
    MeisoByteView input,
    MeisoDisplayIngressCapability *out_value
)
{
    MeisoDisplayIngressCapability candidate;
    const MeisoDisplayIngressProfileStatus status = decode_arguments_status(
        input, out_value, out_value == NULL ? 0U : sizeof(*out_value),
        MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE
    );

    if (status != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        return status;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.max_input_width =
        meiso_display_ingress_private_get_u32(input.data);
    candidate.max_input_height =
        meiso_display_ingress_private_get_u32(input.data + 4U);
    candidate.max_frame_rate_millihz =
        meiso_display_ingress_private_get_u32(input.data + 8U);
    candidate.max_frame_bytes =
        meiso_display_ingress_private_get_u32(input.data + 12U);
    candidate.max_fragment_payload_bytes =
        meiso_display_ingress_private_get_u32(input.data + 16U);
    candidate.evidence_classes =
        meiso_display_ingress_private_get_u32(input.data + 20U);
    candidate.deadline_policy = input.data[24U];
    if (!meiso_display_ingress_private_bytes_zero(input.data + 25U, 7U) ||
        !meiso_display_ingress_private_capability_valid(&candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
    }
    *out_value = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_desired_encode(
    const MeisoDisplayIngressDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t candidate[MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE];

    if (out_size == NULL || encode_arguments_alias(
            value, value == NULL ? 0U : sizeof(*value), output, out_size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (value == NULL || output.data == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_display_ingress_private_desired_valid(value)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    }
    meiso_display_ingress_private_put_u32(candidate, value->input_width);
    meiso_display_ingress_private_put_u32(candidate + 4U, value->input_height);
    meiso_display_ingress_private_put_u32(
        candidate + 8U, value->maximum_frame_rate_millihz
    );
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_desired_decode(
    MeisoByteView input,
    MeisoDisplayIngressDesired *out_value
)
{
    MeisoDisplayIngressDesired candidate;
    const MeisoDisplayIngressProfileStatus status = decode_arguments_status(
        input, out_value, out_value == NULL ? 0U : sizeof(*out_value),
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE
    );

    if (status != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        return status;
    }
    candidate.input_width =
        meiso_display_ingress_private_get_u32(input.data);
    candidate.input_height =
        meiso_display_ingress_private_get_u32(input.data + 4U);
    candidate.maximum_frame_rate_millihz =
        meiso_display_ingress_private_get_u32(input.data + 8U);
    if (!meiso_display_ingress_private_desired_valid(&candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
    }
    *out_value = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_actual_encode(
    const MeisoDisplayIngressActual *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t candidate[MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE];

    if (out_size == NULL || encode_arguments_alias(
            value, value == NULL ? 0U : sizeof(*value), output, out_size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (value == NULL || output.data == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_display_ingress_private_actual_valid(value)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    }
    memset(candidate, 0, sizeof(candidate));
    meiso_display_ingress_private_put_u32(
        candidate, value->effective_input_width
    );
    meiso_display_ingress_private_put_u32(
        candidate + 4U, value->effective_input_height
    );
    meiso_display_ingress_private_put_u32(
        candidate + 8U, value->effective_maximum_frame_rate_millihz
    );
    meiso_display_ingress_private_put_u64(
        candidate + 16U, value->channel_nonce.high
    );
    meiso_display_ingress_private_put_u64(
        candidate + 24U, value->channel_nonce.low
    );
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_actual_decode(
    MeisoByteView input,
    MeisoDisplayIngressActual *out_value
)
{
    MeisoDisplayIngressActual candidate;
    const MeisoDisplayIngressProfileStatus status = decode_arguments_status(
        input, out_value, out_value == NULL ? 0U : sizeof(*out_value),
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE
    );

    if (status != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        return status;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.effective_input_width =
        meiso_display_ingress_private_get_u32(input.data);
    candidate.effective_input_height =
        meiso_display_ingress_private_get_u32(input.data + 4U);
    candidate.effective_maximum_frame_rate_millihz =
        meiso_display_ingress_private_get_u32(input.data + 8U);
    candidate.channel_nonce.high =
        meiso_display_ingress_private_get_u64(input.data + 16U);
    candidate.channel_nonce.low =
        meiso_display_ingress_private_get_u64(input.data + 24U);
    if (!meiso_display_ingress_private_bytes_zero(input.data + 12U, 4U) ||
        !meiso_display_ingress_private_actual_valid(&candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
    }
    *out_value = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_evidence_encode(
    const MeisoDisplayIngressPresentationEvidence *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t candidate[MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE];

    if (out_size == NULL || encode_arguments_alias(
            value, value == NULL ? 0U : sizeof(*value), output, out_size
        )) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (value == NULL || output.data == NULL) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    }
    if (!meiso_display_ingress_private_evidence_valid(value)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY;
    }
    memset(candidate, 0, sizeof(candidate));
    candidate[0] = value->evidence_class;
    candidate[1] = value->connector_kind;
    meiso_display_ingress_private_put_u32(candidate + 4U, value->output_width);
    meiso_display_ingress_private_put_u32(candidate + 8U, value->output_height);
    meiso_display_ingress_private_put_u64(
        candidate + 16U, value->media_stream_epoch
    );
    meiso_display_ingress_private_put_u64(
        candidate + 24U, value->displayed_frame_sequence
    );
    memmove(candidate + 32U, value->input_rgba_sha256,
            sizeof(value->input_rgba_sha256));
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}

MeisoDisplayIngressProfileStatus meiso_display_ingress_evidence_decode(
    MeisoByteView input,
    MeisoDisplayIngressPresentationEvidence *out_value
)
{
    MeisoDisplayIngressPresentationEvidence candidate;
    const MeisoDisplayIngressProfileStatus status = decode_arguments_status(
        input, out_value, out_value == NULL ? 0U : sizeof(*out_value),
        MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE
    );

    if (status != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        return status;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.evidence_class = input.data[0];
    candidate.connector_kind = input.data[1];
    candidate.output_width =
        meiso_display_ingress_private_get_u32(input.data + 4U);
    candidate.output_height =
        meiso_display_ingress_private_get_u32(input.data + 8U);
    candidate.media_stream_epoch =
        meiso_display_ingress_private_get_u64(input.data + 16U);
    candidate.displayed_frame_sequence =
        meiso_display_ingress_private_get_u64(input.data + 24U);
    memmove(candidate.input_rgba_sha256, input.data + 32U,
            sizeof(candidate.input_rgba_sha256));
    if (!meiso_display_ingress_private_bytes_zero(input.data + 2U, 2U) ||
        !meiso_display_ingress_private_bytes_zero(input.data + 12U, 4U) ||
        !meiso_display_ingress_private_evidence_valid(&candidate)) {
        return MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED;
    }
    *out_value = candidate;
    return MEISO_DISPLAY_INGRESS_PROFILE_OK;
}
