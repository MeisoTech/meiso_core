#include "meiso_composition_scene_store_private.h"

#include "composition_sha256_private.h"

/* Fake rendering owns deterministic pixels and published-result bookkeeping. */

#include <string.h>

static uint64_t fnv1a_update(uint64_t hash, const uint8_t *data, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static void fake_pixel(uint64_t scene_seed, uint32_t x, uint32_t y,
                       uint8_t rgba[4])
{
    const uint64_t mixed =
        mix64(scene_seed ^ ((uint64_t)x * UINT64_C(0x9e3779b185ebca87)) ^
              ((uint64_t)y * UINT64_C(0xc2b2ae3d27d4eb4f)));

    rgba[0] = (uint8_t)(mixed >> 8U);
    rgba[1] = (uint8_t)(mixed >> 24U);
    rgba[2] = (uint8_t)(mixed >> 40U);
    rgba[3] = 0xffU;
}

static bool active_scene_seed(const MeisoCompositionSceneStore *store,
                              const MeisoCompositionSceneStoreWorld *world,
                              uint64_t *out_seed)
{
    static const uint8_t domain[] = {
        'M', 'E', 'I', 'S', 'O', '_', 'S', 'C', 'E', 'N', 'E', '_', 'F', 'A',
        'K', 'E', '_', 'R', 'E', 'N', 'D', 'E', 'R', '_', 'V', '0', 0U};
    uint64_t seed = UINT64_C(14695981039346656037);
    uint32_t index;

    seed = fnv1a_update(seed, domain, sizeof(domain));
    seed = fnv1a_update(seed, world->canonical, world->canonical_size);
    for (index = 0U; index < world->unique_mesh_count; ++index) {
        MeisoBlobView blob;
        uint8_t size_be[8];
        uint64_t size = world->unique_mesh_sizes[index];
        size_t byte_index;

        if (!meiso_blob_store_lookup(store->blob_store,
                                     &world->unique_meshes[index], &blob) ||
            blob.content.size != world->unique_mesh_sizes[index] ||
            !meiso_composition_scene_mesh_blob_verify(
                &world->unique_meshes[index], blob.content, NULL)) {
            return false;
        }
        for (byte_index = 0U; byte_index < sizeof(size_be); ++byte_index) {
            size_be[byte_index] = (uint8_t)(size >> (56U - byte_index * 8U));
        }
        seed = fnv1a_update(seed, size_be, sizeof(size_be));
        seed = fnv1a_update(seed, blob.content.data, blob.content.size);
    }
    *out_seed = seed;
    return true;
}

MeisoCompositionSceneRenderStatus meiso_composition_scene_store_fake_render(
    MeisoCompositionSceneStore *store, uint64_t source_clock_epoch,
    uint64_t render_start_monotonic_ns, uint64_t render_end_monotonic_ns,
    MeisoCompositionSceneRenderResult *out_result)
{
    const MeisoCompositionSceneStoreWorld *world;
    MeisoCompositionSha256 sha;
    uint64_t seed;
    uint32_t y;

    if (!meiso_composition_scene_store_private_valid(store) ||
        out_result == NULL ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_result, sizeof(*out_result))) {
        return MEISO_COMPOSITION_SCENE_RENDER_INVALID_ARGUMENT;
    }
    if (source_clock_epoch == 0U ||
        render_end_monotonic_ns < render_start_monotonic_ns) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.render_failures);
        return MEISO_COMPOSITION_SCENE_RENDER_INVALID_TIME;
    }
    world = meiso_composition_scene_store_private_const_active_world(store);
    if (world == NULL) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.render_failures);
        return MEISO_COMPOSITION_SCENE_RENDER_NO_ACTIVE_WORLD;
    }
    if (!active_scene_seed(store, world, &seed)) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.render_failures);
        return MEISO_COMPOSITION_SCENE_RENDER_MISSING_BLOB;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->applied_scene_owner_incarnation =
        world->ref.desired_owner_incarnation;
    out_result->applied_scene_state_version = world->ref.desired_state_version;
    out_result->active_plan_generation = world->ref.plan_generation;
    out_result->world_generation = world->ref.world_generation;
    out_result->result_code = MEISO_COMPOSITION_SCENE_RESULT_COMPLETE;
    out_result->evidence_flags =
        MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |
        MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED;
    out_result->object_count = world->desired.object_count;
    out_result->unique_mesh_count = world->unique_mesh_count;
    out_result->source_clock_epoch = source_clock_epoch;
    out_result->render_start_monotonic_ns = render_start_monotonic_ns;
    out_result->render_end_monotonic_ns = render_end_monotonic_ns;
    out_result->target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    out_result->target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    out_result->color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    out_result->depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    out_result->logical_color_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    out_result->logical_depth_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    out_result->thumbnail_width = MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    out_result->thumbnail_height = MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    out_result->thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    out_result->thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH * 2U;
    out_result->thumbnail_byte_size =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;

    meiso_composition_sha256_init(&sha);
    for (y = 0U; y < MEISO_COMPOSITION_SCENE_TARGET_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_TARGET_WIDTH; ++x) {
            uint8_t rgba[4];
            fake_pixel(seed, x, y, rgba);
            meiso_composition_sha256_update(&sha, rgba, sizeof(rgba));
        }
    }
    meiso_composition_sha256_final(&sha, out_result->full_target_sha256);

    for (y = 0U; y < MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH; ++x) {
            uint8_t rgba[4];
            uint16_t rgb565;
            const size_t offset =
                ((size_t)y * MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH + x) * 2U;
            fake_pixel(seed, x * 5U + 2U, y * 5U + 2U, rgba);
            rgb565 = (uint16_t)(((uint16_t)(rgba[0] >> 3U) << 11U) |
                                ((uint16_t)(rgba[1] >> 2U) << 5U) |
                                (uint16_t)(rgba[2] >> 3U));
            out_result->thumbnail_rgb565[offset] = (uint8_t)(rgb565 >> 8U);
            out_result->thumbnail_rgb565[offset + 1U] = (uint8_t)rgb565;
        }
    }
    meiso_composition_scene_store_private_increment_saturated(
        &store->stats.renders);
    return MEISO_COMPOSITION_SCENE_RENDER_COMPLETE;
}

bool meiso_composition_scene_store_record_result_published(
    MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneWorldRef *rendered_world, uint64_t stream_epoch,
    uint64_t sequence)
{
    const MeisoCompositionSceneStoreWorld *active;

    if (!meiso_composition_scene_store_private_valid(store) ||
        rendered_world == NULL || stream_epoch == 0U || sequence == 0U) {
        return false;
    }
    active = meiso_composition_scene_store_private_const_active_world(store);
    if (active == NULL ||
        !meiso_composition_scene_store_private_world_ref_equal(
            &active->ref, rendered_world)) {
        return false;
    }
    if (store->last_result_stream_epoch == stream_epoch &&
        sequence <= store->last_result_sequence) {
        return false;
    }
    store->last_result_stream_epoch = stream_epoch;
    store->last_result_sequence = sequence;
    return true;
}
