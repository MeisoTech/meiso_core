#include <meiso/profiles/composition/scene.h>
#include "composition_sha256_private.h"

#include <stdint.h>
#include <string.h>

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

bool meiso_composition_scene_plan_digest(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionScenePresentationPlan canonical;
    MeisoCompositionSha256 sha;
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    size_t wire_size = 0U;

    if (plan == NULL || out_digest == NULL ||
        ranges_overlap(plan, sizeof(*plan), out_digest,
                       MEISO_SHA256_DIGEST_SIZE)) {
        return false;
    }
    canonical = *plan;
    memset(canonical.candidate_digest, 0x01,
           sizeof(canonical.candidate_digest));
    canonical.prepared_token_echo_present = false;
    memset(canonical.prepared_token_echo, 0,
           sizeof(canonical.prepared_token_echo));
    if (meiso_composition_scene_presentation_plan_encode(
            &canonical, (MeisoMutableBuffer){wire, sizeof(wire)},
            &wire_size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        wire_size != sizeof(wire)) {
        return false;
    }
    memset(wire + 24U, 0, MEISO_SHA256_DIGEST_SIZE);
    wire[75U] = 0U;
    memset(wire + 96U, 0, MEISO_SHA256_DIGEST_SIZE);
    meiso_composition_sha256_init(&sha);
    meiso_composition_sha256_update(&sha, wire, sizeof(wire));
    meiso_composition_sha256_final(&sha, out_digest);
    return true;
}

bool meiso_composition_scene_mesh_content_id(
    MeisoByteView content,
    MeisoContentId *out_content_id
)
{
    MeisoCompositionSceneMeshView mesh;
    MeisoContentId content_id;
    MeisoCompositionSha256 sha;

    if (out_content_id == NULL || content.data == NULL ||
        ranges_overlap(content.data, content.size,
                       out_content_id, sizeof(*out_content_id)) ||
        content.size > MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE ||
        meiso_composition_scene_mesh_decode(content, &mesh) !=
            MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return false;
    }
    memset(&content_id, 0, sizeof(content_id));
    content_id.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    content_id.digest_size = MEISO_SHA256_DIGEST_SIZE;
    meiso_composition_sha256_init(&sha);
    meiso_composition_sha256_update(&sha, content.data, content.size);
    meiso_composition_sha256_final(&sha, content_id.digest);
    *out_content_id = content_id;
    return true;
}

bool meiso_composition_scene_mesh_blob_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    MeisoContentId calculated;

    (void)context;
    return meiso_content_id_valid(expected_content_id) &&
        meiso_composition_scene_mesh_content_id(
            staged_content, &calculated) &&
        meiso_content_id_equal(expected_content_id, &calculated);
}
