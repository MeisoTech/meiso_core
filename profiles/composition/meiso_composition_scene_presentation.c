#include "meiso_composition_scene_private.h"

#include <string.h>

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_PRESENTATION_DESIRED_STATE_MAX_PAYLOAD_SIZE <=
            8192U,
    "PresentationPlan State frame must fit the Runtime payload limit"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE <=
            8192U,
    "PresentationStatus State frame must fit the Runtime payload limit"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE <= 8192U,
    "SceneStatus State frame must fit the Runtime payload limit"
);
enum {

    STATUS_MAGIC = 0x53535430U,

    CODEC_WIRE_VERSION = 1U,

    PRESENTATION_PLAN_KNOWN_FLAGS =

        MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME

};

/* Scene status and presentation desired/actual canonical codecs. */

static bool scene_status_valid(const MeisoCompositionSceneStatus *value)
{
    const uint64_t max_blob_bytes =
        (uint64_t)MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS *
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
    const uint64_t bytes_per_target_set =
        (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT * 4U * 2U;
    bool no_active_world;
    bool active_world;
    bool no_last_result;
    bool has_last_result;

    if (value == NULL) {
        return false;
    }
    no_active_world = value->active_plan_generation == 0U &&
        value->world_generation == 0U;
    active_world = value->active_plan_generation != 0U &&
        value->world_generation != 0U;
    no_last_result = value->last_result_stream_epoch == 0U &&
        value->last_result_sequence == 0U;
    has_last_result = value->last_result_stream_epoch != 0U &&
        value->last_result_sequence != 0U;

    if (!meiso_composition_scene_private_stable_reason_valid(value->reason) ||
        (!no_active_world && !active_world) ||
        (!no_last_result && !has_last_result) ||
        value->active_object_count >
            MEISO_COMPOSITION_SCENE_MAX_OBJECTS ||
        value->unique_mesh_count >
            MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES ||
        value->unique_mesh_count > value->active_object_count ||
        value->published_blob_count >
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS ||
        value->published_blob_bytes > max_blob_bytes ||
        value->resident_mesh_bytes >
            MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL ||
        value->target_slots_used >
            MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT ||
        value->target_bytes >
            bytes_per_target_set * value->target_slots_used ||
        value->render_jobs_in_flight >
            MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS ||
        value->render_jobs_in_flight > value->target_slots_used ||
        value->result_queue_depth >
            MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS) {
        return false;
    }
    if ((value->published_blob_count == 0U) !=
        (value->published_blob_bytes == 0U)) {
        return false;
    }
    if (no_active_world && (value->active_object_count != 0U ||
                            value->unique_mesh_count != 0U)) {
        return false;
    }
    return true;
}

size_t meiso_composition_scene_status_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_status_encode(
    const MeisoCompositionSceneStatus *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_encode(
        value, scene_status_valid(value), output,
        MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_ranges_overlap(value, sizeof(*value), output.data,
                       MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0, MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE);
    meiso_composition_scene_private_write_u32(output.data, STATUS_MAGIC);
    meiso_composition_scene_private_write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    meiso_composition_scene_private_write_u16(output.data + 6U, value->reason);
    meiso_composition_scene_private_write_u64(output.data + 8U, value->active_plan_generation);
    meiso_composition_scene_private_write_u64(output.data + 16U, value->world_generation);
    meiso_composition_scene_private_write_u32(output.data + 24U, value->active_object_count);
    meiso_composition_scene_private_write_u32(output.data + 28U, value->unique_mesh_count);
    meiso_composition_scene_private_write_u32(output.data + 32U, value->published_blob_count);
    meiso_composition_scene_private_write_u32(output.data + 36U, value->target_slots_used);
    meiso_composition_scene_private_write_u64(output.data + 40U, value->published_blob_bytes);
    meiso_composition_scene_private_write_u64(output.data + 48U, value->resident_mesh_bytes);
    meiso_composition_scene_private_write_u64(output.data + 56U, value->target_bytes);
    meiso_composition_scene_private_write_u32(output.data + 64U, value->render_jobs_in_flight);
    meiso_composition_scene_private_write_u32(output.data + 68U, value->result_queue_depth);
    meiso_composition_scene_private_write_u64(output.data + 72U, value->last_result_stream_epoch);
    meiso_composition_scene_private_write_u64(output.data + 80U, value->last_result_sequence);
    *out_size = MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_status_decode(
    MeisoByteView input,
    MeisoCompositionSceneStatus *out_value
)
{
    MeisoCompositionSceneStatus value;
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE, out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_read_u32(input.data) != STATUS_MAGIC ||
        meiso_composition_scene_private_read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        !meiso_composition_scene_private_bytes_zero(input.data + 88U, 24U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.reason = meiso_composition_scene_private_read_u16(input.data + 6U);
    value.active_plan_generation = meiso_composition_scene_private_read_u64(input.data + 8U);
    value.world_generation = meiso_composition_scene_private_read_u64(input.data + 16U);
    value.active_object_count = meiso_composition_scene_private_read_u32(input.data + 24U);
    value.unique_mesh_count = meiso_composition_scene_private_read_u32(input.data + 28U);
    value.published_blob_count = meiso_composition_scene_private_read_u32(input.data + 32U);
    value.target_slots_used = meiso_composition_scene_private_read_u32(input.data + 36U);
    value.published_blob_bytes = meiso_composition_scene_private_read_u64(input.data + 40U);
    value.resident_mesh_bytes = meiso_composition_scene_private_read_u64(input.data + 48U);
    value.target_bytes = meiso_composition_scene_private_read_u64(input.data + 56U);
    value.render_jobs_in_flight = meiso_composition_scene_private_read_u32(input.data + 64U);
    value.result_queue_depth = meiso_composition_scene_private_read_u32(input.data + 68U);
    value.last_result_stream_epoch = meiso_composition_scene_private_read_u64(input.data + 72U);
    value.last_result_sequence = meiso_composition_scene_private_read_u64(input.data + 80U);
    if (!scene_status_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}
static bool plan_valid(const MeisoCompositionScenePresentationPlan *value)
{
    if (value == NULL ||
        (value->flags & (MeisoCompositionScenePlanFlags)
            ~PRESENTATION_PLAN_KNOWN_FLAGS) != 0U ||
        value->plan_generation == 0U ||
        value->capability_generation == 0U ||
        !meiso_composition_scene_private_bytes_nonzero(value->candidate_digest,
                       MEISO_SHA256_DIGEST_SIZE) ||
        !meiso_composition_scene_private_endpoint_id_equal(value->scene_binding, meiso_composition_scene_private_scene_endpoint) ||
        value->source != MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE ||
        value->geometry != MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION ||
        value->target_width != MEISO_COMPOSITION_SCENE_TARGET_WIDTH ||
        value->target_height != MEISO_COMPOSITION_SCENE_TARGET_HEIGHT ||
        value->color_format != MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 ||
        value->depth_format !=
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 ||
        value->max_result_hz != MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ ||
        value->offline_lease_ms >
            MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS) {
        return false;
    }
    if (value->prepared_token_echo_present) {
        return meiso_composition_scene_private_bytes_nonzero(value->prepared_token_echo,
                             MEISO_SHA256_DIGEST_SIZE);
    }
    return meiso_composition_scene_private_bytes_zero(value->prepared_token_echo, MEISO_SHA256_DIGEST_SIZE);
}

static bool candidate_state_valid(MeisoCompositionSceneCandidateState value)
{
    return value >= MEISO_COMPOSITION_SCENE_CANDIDATE_NONE &&
        value <= MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED;
}

static bool active_state_valid(MeisoCompositionSceneActiveState value)
{
    return value >= MEISO_COMPOSITION_SCENE_ACTIVE_NONE &&
        value <= MEISO_COMPOSITION_SCENE_ACTIVE_SAFETY;
}

static bool presentation_reason_valid(
    MeisoCompositionScenePresentationReason value
)
{
    return value <=
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED;
}

static bool zero_generation_digest(
    uint64_t generation,
    const uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    return generation == 0U && meiso_composition_scene_private_bytes_zero(digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool nonzero_generation_digest(
    uint64_t generation,
    const uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    return generation != 0U &&
        meiso_composition_scene_private_bytes_nonzero(digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool presentation_status_valid(
    const MeisoCompositionScenePresentationStatus *value
)
{
    bool candidate_ref_valid;
    bool active_ref_valid;

    if (value == NULL || value->capability_generation == 0U ||
        !candidate_state_valid(value->candidate_state) ||
        !active_state_valid(value->active_state) ||
        !presentation_reason_valid(value->reason) ||
        !meiso_composition_scene_private_endpoint_id_equal(value->scene_binding, meiso_composition_scene_private_scene_endpoint) ||
        value->target_width != MEISO_COMPOSITION_SCENE_TARGET_WIDTH ||
        value->target_height != MEISO_COMPOSITION_SCENE_TARGET_HEIGHT ||
        value->color_format != MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 ||
        value->depth_format !=
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32) {
        return false;
    }

    candidate_ref_valid = value->candidate_state ==
        MEISO_COMPOSITION_SCENE_CANDIDATE_NONE ?
        zero_generation_digest(value->candidate_plan_generation,
                               value->candidate_digest) :
        nonzero_generation_digest(value->candidate_plan_generation,
                                  value->candidate_digest);
    active_ref_valid = value->active_state ==
        MEISO_COMPOSITION_SCENE_ACTIVE_NONE ?
        zero_generation_digest(value->active_plan_generation,
                               value->active_digest) :
        nonzero_generation_digest(value->active_plan_generation,
                                  value->active_digest);
    if (!candidate_ref_valid || !active_ref_valid) {
        return false;
    }

    if (value->candidate_state ==
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
        value->candidate_state ==
            MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) {
        if (!value->prepared_token_present ||
            !meiso_composition_scene_private_bytes_nonzero(value->prepared_token,
                           MEISO_SHA256_DIGEST_SIZE) ||
            value->reason !=
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE) {
            return false;
        }
    } else if (value->prepared_token_present ||
               !meiso_composition_scene_private_bytes_zero(value->prepared_token,
                           MEISO_SHA256_DIGEST_SIZE)) {
        return false;
    }

    if ((value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED ||
         value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED) &&
        value->reason == MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE) {
        return false;
    }
    if ((value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_NONE ||
         value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
         value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) &&
        value->reason != MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE) {
        return false;
    }
    return true;
}

size_t meiso_composition_scene_presentation_plan_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_plan_encode(
    const MeisoCompositionScenePresentationPlan *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_encode(
        value, plan_valid(value), output,
        MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_ranges_overlap(value, sizeof(*value), output.data,
                       MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0,
           MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE);
    meiso_composition_scene_private_write_u32(output.data, UINT32_C(0x504c4e30)); /* PLN0 */
    meiso_composition_scene_private_write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    meiso_composition_scene_private_write_u16(output.data + 6U, value->flags);
    meiso_composition_scene_private_write_u64(output.data + 8U, value->plan_generation);
    meiso_composition_scene_private_write_u64(output.data + 16U, value->capability_generation);
    memcpy(output.data + 24U, value->candidate_digest,
           MEISO_SHA256_DIGEST_SIZE);
    meiso_composition_scene_private_write_u64(output.data + 56U, value->scene_binding.high);
    meiso_composition_scene_private_write_u64(output.data + 64U, value->scene_binding.low);
    output.data[72U] = value->source;
    output.data[73U] = value->geometry;
    output.data[74U] = 1U; /* exactly one required layer */
    output.data[75U] = value->prepared_token_echo_present ? 1U : 0U;
    meiso_composition_scene_private_write_u16(output.data + 76U, value->target_width);
    meiso_composition_scene_private_write_u16(output.data + 78U, value->target_height);
    meiso_composition_scene_private_write_u16(output.data + 80U, value->color_format);
    meiso_composition_scene_private_write_u16(output.data + 82U, value->depth_format);
    meiso_composition_scene_private_write_u16(output.data + 84U, value->max_result_hz);
    meiso_composition_scene_private_write_u32(output.data + 88U, value->offline_lease_ms);
    memcpy(output.data + 96U, value->prepared_token_echo,
           MEISO_SHA256_DIGEST_SIZE);
    *out_size = MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_plan_decode(
    MeisoByteView input,
    MeisoCompositionScenePresentationPlan *out_value
)
{
    MeisoCompositionScenePresentationPlan value;
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE,
        out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_read_u32(input.data) != UINT32_C(0x504c4e30) ||
        meiso_composition_scene_private_read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        input.data[74U] != 1U || input.data[75U] > 1U ||
        !meiso_composition_scene_private_bytes_zero(input.data + 86U, 2U) ||
        !meiso_composition_scene_private_bytes_zero(input.data + 92U, 4U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    value.flags = meiso_composition_scene_private_read_u16(input.data + 6U);
    value.plan_generation = meiso_composition_scene_private_read_u64(input.data + 8U);
    value.capability_generation = meiso_composition_scene_private_read_u64(input.data + 16U);
    memcpy(value.candidate_digest, input.data + 24U,
           MEISO_SHA256_DIGEST_SIZE);
    value.scene_binding.high = meiso_composition_scene_private_read_u64(input.data + 56U);
    value.scene_binding.low = meiso_composition_scene_private_read_u64(input.data + 64U);
    value.source = input.data[72U];
    value.geometry = input.data[73U];
    value.prepared_token_echo_present = input.data[75U] != 0U;
    value.target_width = meiso_composition_scene_private_read_u16(input.data + 76U);
    value.target_height = meiso_composition_scene_private_read_u16(input.data + 78U);
    value.color_format = meiso_composition_scene_private_read_u16(input.data + 80U);
    value.depth_format = meiso_composition_scene_private_read_u16(input.data + 82U);
    value.max_result_hz = meiso_composition_scene_private_read_u16(input.data + 84U);
    value.offline_lease_ms = meiso_composition_scene_private_read_u32(input.data + 88U);
    memcpy(value.prepared_token_echo, input.data + 96U,
           MEISO_SHA256_DIGEST_SIZE);
    if (!plan_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

size_t meiso_composition_scene_presentation_status_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_status_encode(
    const MeisoCompositionScenePresentationStatus *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_encode(
        value, presentation_status_valid(value), output,
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_ranges_overlap(
            value, sizeof(*value), output.data,
            MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
        )) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0,
           MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE);
    meiso_composition_scene_private_write_u32(output.data, UINT32_C(0x50535430)); /* PST0 */
    meiso_composition_scene_private_write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    meiso_composition_scene_private_write_u64(output.data + 8U, value->capability_generation);
    meiso_composition_scene_private_write_u64(output.data + 16U, value->candidate_plan_generation);
    memcpy(output.data + 24U, value->candidate_digest,
           MEISO_SHA256_DIGEST_SIZE);
    meiso_composition_scene_private_write_u64(output.data + 56U, value->active_plan_generation);
    memcpy(output.data + 64U, value->active_digest,
           MEISO_SHA256_DIGEST_SIZE);
    output.data[96U] = value->candidate_state;
    output.data[97U] = value->active_state;
    meiso_composition_scene_private_write_u16(output.data + 98U, value->reason);
    output.data[100U] = value->prepared_token_present ? 1U : 0U;
    memcpy(output.data + 104U, value->prepared_token,
           MEISO_SHA256_DIGEST_SIZE);
    meiso_composition_scene_private_write_u16(output.data + 136U, value->target_width);
    meiso_composition_scene_private_write_u16(output.data + 138U, value->target_height);
    meiso_composition_scene_private_write_u16(output.data + 140U, value->color_format);
    meiso_composition_scene_private_write_u16(output.data + 142U, value->depth_format);
    meiso_composition_scene_private_write_u64(output.data + 144U, value->scene_binding.high);
    meiso_composition_scene_private_write_u64(output.data + 152U, value->scene_binding.low);
    *out_size = MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_status_decode(
    MeisoByteView input,
    MeisoCompositionScenePresentationStatus *out_value
)
{
    MeisoCompositionScenePresentationStatus value;
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE,
        out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_read_u32(input.data) != UINT32_C(0x50535430) ||
        meiso_composition_scene_private_read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        !meiso_composition_scene_private_bytes_zero(input.data + 6U, 2U) || input.data[100U] > 1U ||
        !meiso_composition_scene_private_bytes_zero(input.data + 101U, 3U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    value.capability_generation = meiso_composition_scene_private_read_u64(input.data + 8U);
    value.candidate_plan_generation = meiso_composition_scene_private_read_u64(input.data + 16U);
    memcpy(value.candidate_digest, input.data + 24U,
           MEISO_SHA256_DIGEST_SIZE);
    value.active_plan_generation = meiso_composition_scene_private_read_u64(input.data + 56U);
    memcpy(value.active_digest, input.data + 64U,
           MEISO_SHA256_DIGEST_SIZE);
    value.candidate_state = input.data[96U];
    value.active_state = input.data[97U];
    value.reason = meiso_composition_scene_private_read_u16(input.data + 98U);
    value.prepared_token_present = input.data[100U] != 0U;
    memcpy(value.prepared_token, input.data + 104U,
           MEISO_SHA256_DIGEST_SIZE);
    value.target_width = meiso_composition_scene_private_read_u16(input.data + 136U);
    value.target_height = meiso_composition_scene_private_read_u16(input.data + 138U);
    value.color_format = meiso_composition_scene_private_read_u16(input.data + 140U);
    value.depth_format = meiso_composition_scene_private_read_u16(input.data + 142U);
    value.scene_binding.high = meiso_composition_scene_private_read_u64(input.data + 144U);
    value.scene_binding.low = meiso_composition_scene_private_read_u64(input.data + 152U);
    if (!presentation_status_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}
