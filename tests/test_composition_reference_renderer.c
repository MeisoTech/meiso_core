#include "composition_reference_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    void *blob_arena;
    size_t blob_arena_size;
    void *scene_arena;
    size_t scene_arena_size;
    void *renderer_arena;
    size_t renderer_arena_size;
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionReferenceRenderer *renderer;
    MeisoOwnerScope host_owner;
    MeisoCompositionSceneControlScope scope;
    MeisoContentId cube_id;
    uint8_t cube_storage[512];
    MeisoByteView cube;
} Fixture;

typedef struct {
    uint8_t color_sha256[MEISO_SHA256_DIGEST_SIZE];
    uint8_t depth_sha256[MEISO_SHA256_DIGEST_SIZE];
    uint8_t thumbnail_sha256[MEISO_SHA256_DIGEST_SIZE];
    uint8_t depth_selected_be[3U][4U];
} RenderGolden;

static const RenderGolden RENDER_GOLDENS[3] = {
    {
        {0x3f, 0xd3, 0x97, 0xc5, 0xe2, 0x59, 0xc4, 0x3d,
         0x5e, 0xc4, 0x80, 0xf9, 0xb8, 0x6a, 0x7c, 0x86,
         0xe9, 0x4f, 0xb2, 0x39, 0xd0, 0x34, 0x04, 0xf3,
         0x30, 0xb8, 0x8e, 0x69, 0x0c, 0x0a, 0xee, 0x65},
        {0x6d, 0x9c, 0x95, 0x7a, 0xf7, 0xb2, 0x50, 0x0b,
         0x30, 0x9a, 0x42, 0x5d, 0x8c, 0xd4, 0x31, 0x4a,
         0xea, 0x3f, 0x99, 0x34, 0x8f, 0x5d, 0x16, 0x99,
         0xb9, 0x63, 0x15, 0xa1, 0x77, 0x2f, 0xaa, 0xae},
        {0x07, 0xf0, 0x56, 0x87, 0x2d, 0x92, 0xa2, 0x33,
         0x5e, 0x0d, 0x3f, 0x1d, 0xa6, 0x75, 0xbf, 0x02,
         0x89, 0x22, 0x38, 0xc2, 0xf8, 0xb7, 0xe2, 0x7c,
         0x24, 0xe8, 0x5d, 0xf5, 0xa4, 0xcd, 0xdb, 0x91},
        {{0xc2, 0x29, 0x38, 0x27},
         {0x1e, 0x37, 0x69, 0xc2},
         {0x3e, 0xde, 0x4a, 0xfc}}
    },
    {
        {0xf5, 0x3f, 0x4e, 0x06, 0x29, 0xcf, 0xe1, 0x97,
         0x64, 0x9a, 0x32, 0x42, 0x6a, 0xb6, 0x2b, 0xd7,
         0xe7, 0xd6, 0x86, 0xd6, 0x2f, 0x48, 0xbf, 0xa8,
         0xe7, 0x44, 0x66, 0x91, 0x2e, 0xf8, 0xdb, 0xd7},
        {0x99, 0x23, 0x23, 0x21, 0xae, 0x85, 0xcc, 0x89,
         0x5b, 0x1a, 0x8d, 0x8c, 0xc0, 0x09, 0x11, 0x5b,
         0x70, 0x83, 0x11, 0x1f, 0xde, 0x7f, 0x12, 0x66,
         0x44, 0x9d, 0xca, 0xb1, 0xdd, 0xb4, 0x0f, 0x1a},
        {0x5d, 0x8f, 0x19, 0x8d, 0xbd, 0xbf, 0xe2, 0xba,
         0x20, 0xb1, 0xb7, 0xf1, 0x53, 0x8e, 0x00, 0xf4,
         0x25, 0x32, 0x22, 0x82, 0x53, 0x5d, 0x67, 0x21,
         0x98, 0x8b, 0xa8, 0xf6, 0xb0, 0xdd, 0x78, 0x13},
        {{0xea, 0x30, 0x9e, 0x85},
         {0x11, 0xbc, 0x7c, 0x63},
         {0xe7, 0x30, 0xb7, 0xd9}}
    },
    {
        {0x31, 0xae, 0xde, 0x8a, 0xb8, 0xb9, 0x8d, 0xdc,
         0xfa, 0xeb, 0x22, 0x60, 0x1f, 0xdb, 0x63, 0xaa,
         0x6c, 0x7e, 0x76, 0x0c, 0x3a, 0x12, 0xb2, 0xc6,
         0x18, 0x8d, 0x80, 0xea, 0x8f, 0xb3, 0xfa, 0xcd},
        {0x63, 0xca, 0xeb, 0x44, 0x2b, 0x08, 0x7e, 0x69,
         0x69, 0x7d, 0x18, 0xa2, 0x88, 0xe3, 0x08, 0xf2,
         0x64, 0xd0, 0x6e, 0xea, 0xd6, 0x8b, 0x41, 0x34,
         0x65, 0xc8, 0xf5, 0x7d, 0xa9, 0x97, 0xec, 0xc8},
        {0x83, 0xe9, 0xc6, 0x21, 0xa3, 0x93, 0x9e, 0xdf,
         0x1b, 0x44, 0xe8, 0x82, 0x74, 0x96, 0xce, 0x68,
         0x3c, 0xd5, 0x46, 0x55, 0x57, 0xc2, 0xd1, 0xf9,
         0xe3, 0xe8, 0x82, 0xb1, 0x29, 0xb7, 0x86, 0xda},
        {{0x0b, 0xd6, 0x12, 0xdc},
         {0x6c, 0xb6, 0xa0, 0x60},
         {0x35, 0x17, 0x8f, 0xf2}}
    }
};

static bool sha256_matches(
    MeisoByteView bytes,
    const uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoContentId expected;

    memset(&expected, 0, sizeof(expected));
    expected.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    expected.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memcpy(expected.digest, digest, MEISO_SHA256_DIGEST_SIZE);
    return meiso_runtime_blob_sha256_verify(&expected, bytes, NULL);
}

static bool control_scope_equal(
    MeisoCompositionSceneControlScope lhs,
    MeisoCompositionSceneControlScope rhs
)
{
    return lhs.desired_owner_incarnation ==
            rhs.desired_owner_incarnation &&
        lhs.control_scope_generation == rhs.control_scope_generation;
}

static bool world_ref_equal(
    MeisoCompositionSceneWorldRef lhs,
    MeisoCompositionSceneWorldRef rhs
)
{
    return lhs.desired_owner_incarnation ==
            rhs.desired_owner_incarnation &&
        lhs.desired_state_version == rhs.desired_state_version &&
        lhs.plan_generation == rhs.plan_generation &&
        lhs.world_generation == rhs.world_generation;
}

static bool job_ref_equal(
    MeisoCompositionReferenceRendererJobRef lhs,
    MeisoCompositionReferenceRendererJobRef rhs
)
{
    return lhs.renderer_generation == rhs.renderer_generation &&
        lhs.job_id == rhs.job_id &&
        control_scope_equal(lhs.control_scope, rhs.control_scope) &&
        world_ref_equal(lhs.world, rhs.world);
}

static bool bytes_are_zero(MeisoByteView bytes)
{
    size_t index;

    for (index = 0U; index < bytes.size; ++index) {
        if (bytes.data[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool encode_cube(Fixture *fixture)
{
    size_t encoded_size = 0U;

    CHECK(meiso_composition_scene_mesh_encode(
        meiso_composition_scene_canonical_cube(),
        (MeisoMutableBuffer){
            fixture->cube_storage, sizeof(fixture->cube_storage)
        },
        &encoded_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    fixture->cube = (MeisoByteView){
        fixture->cube_storage, encoded_size
    };
    CHECK(meiso_composition_scene_mesh_content_id(
        fixture->cube, &fixture->cube_id
    ));
    return true;
}

static bool publish_cube(Fixture *fixture, bool duplicate)
{
    MeisoMutableBuffer staging;

    CHECK(meiso_blob_store_stage_begin(
        fixture->blob_store,
        &fixture->cube_id,
        fixture->cube.size
    ) == MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(
        fixture->blob_store, &staging
    ));
    CHECK(staging.capacity == fixture->cube.size);
    memcpy(staging.data, fixture->cube.data, fixture->cube.size);
    CHECK(meiso_blob_store_commit_verified(
        fixture->blob_store,
        fixture->cube.size,
        meiso_composition_scene_mesh_blob_verify,
        NULL
    ) == (duplicate ? MEISO_BLOB_COMMIT_DUPLICATE :
                       MEISO_BLOB_COMMIT_PUBLISHED));
    CHECK(meiso_composition_scene_store_mark_blob_visible(
        fixture->scene_store,
        fixture->host_owner.incarnation,
        &fixture->cube_id,
        fixture->cube.size
    ) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE);
    return true;
}

static bool publish_mesh(
    Fixture *fixture,
    MeisoContentId id,
    MeisoByteView bytes
)
{
    MeisoMutableBuffer staging;

    CHECK(meiso_blob_store_stage_begin(
        fixture->blob_store, &id, bytes.size
    ) == MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(
        fixture->blob_store, &staging
    ) && staging.capacity == bytes.size);
    memcpy(staging.data, bytes.data, bytes.size);
    CHECK(meiso_blob_store_commit_verified(
        fixture->blob_store,
        bytes.size,
        meiso_composition_scene_mesh_blob_verify,
        NULL
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(meiso_composition_scene_store_mark_blob_visible(
        fixture->scene_store,
        fixture->host_owner.incarnation,
        &id,
        bytes.size
    ) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE);
    return true;
}

static bool encode_triangle(
    uint8_t *storage,
    size_t capacity,
    MeisoByteView *out_bytes,
    MeisoContentId *out_id
)
{
    MeisoCompositionSceneMeshVertex vertices[3];
    const uint32_t indices[3] = {0U, 1U, 2U};
    MeisoCompositionSceneMesh mesh;
    size_t encoded_size = 0U;

    memset(vertices, 0, sizeof(vertices));
    vertices[0].position[0] = -1.0F;
    vertices[1].position[0] = 2.0F;
    vertices[2].position[1] = 1.5F;
    memset(&mesh, 0, sizeof(mesh));
    mesh.vertex_count = 3U;
    mesh.vertices = vertices;
    mesh.index_count = 3U;
    mesh.indices = indices;
    CHECK(meiso_composition_scene_mesh_encode(
        &mesh,
        (MeisoMutableBuffer){storage, capacity},
        &encoded_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    *out_bytes = (MeisoByteView){storage, encoded_size};
    CHECK(meiso_composition_scene_mesh_content_id(*out_bytes, out_id));
    return true;
}

static bool verify_thumbnail_samples(
    const MeisoCompositionReferenceRendererCompleteView *complete
)
{
    uint32_t y;

    CHECK(complete->result.thumbnail_width ==
              MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH &&
          complete->result.thumbnail_height ==
              MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT &&
          complete->result.thumbnail_byte_size ==
              MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE &&
          complete->result.thumbnail_stride_bytes ==
              MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH * 2U);
    for (y = 0U; y < MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT; ++y) {
        uint32_t x;

        for (x = 0U; x < MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH; ++x) {
            const uint32_t source_x = x * 5U + 2U;
            const uint32_t source_y = y * 5U + 2U;
            const size_t source =
                ((size_t)source_y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
                 source_x) * 4U;
            const size_t target =
                ((size_t)y * MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH + x) *
                2U;
            const uint8_t *rgba = complete->color_rgba8888.data + source;
            const uint16_t rgb565 = (uint16_t)(
                ((uint16_t)(rgba[0] >> 3U) << 11U) |
                ((uint16_t)(rgba[1] >> 2U) << 5U) |
                (uint16_t)(rgba[2] >> 3U)
            );

            CHECK(complete->result.thumbnail_rgb565[target] ==
                      (uint8_t)(rgb565 >> 8U) &&
                  complete->result.thumbnail_rgb565[target + 1U] ==
                      (uint8_t)rgb565);
        }
    }
    return true;
}

static bool verify_result_codec(
    const MeisoCompositionSceneRenderResult *result
)
{
    uint8_t first[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    uint8_t second[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    MeisoCompositionSceneRenderResult decoded;
    size_t first_size = 0U;
    size_t second_size = 0U;

    CHECK(meiso_composition_scene_render_result_encode(
        result, (MeisoMutableBuffer){first, sizeof(first)}, &first_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(first_size == sizeof(first));
    CHECK(meiso_composition_scene_render_result_decode(
        (MeisoByteView){first, first_size}, &decoded
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_render_result_encode(
        &decoded, (MeisoMutableBuffer){second, sizeof(second)}, &second_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(second_size == first_size &&
          memcmp(first, second, first_size) == 0);
    return true;
}

static bool verify_render_golden(
    const MeisoCompositionReferenceRendererCompleteView *complete,
    const RenderGolden *golden
)
{
    static const size_t selected_offsets[3] = {
        0U,
        MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE / 2U,
        MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE - 4U
    };
    const MeisoByteView thumbnail = {
        complete->result.thumbnail_rgb565,
        complete->result.thumbnail_byte_size
    };
    uint32_t index;

    CHECK(complete->color_rgba8888.size ==
              MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE &&
          complete->depth_implementation_32.size ==
              MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE &&
          complete->result.evidence_flags == UINT32_C(0x00000003) &&
          sha256_matches(complete->color_rgba8888,
                         golden->color_sha256) &&
          memcmp(complete->result.full_target_sha256,
                 golden->color_sha256,
                 MEISO_SHA256_DIGEST_SIZE) == 0 &&
          sha256_matches(complete->depth_implementation_32,
                         golden->depth_sha256) &&
          sha256_matches(thumbnail, golden->thumbnail_sha256) &&
          verify_thumbnail_samples(complete) &&
          verify_result_codec(&complete->result));
    for (index = 0U; index < 3U; ++index) {
        CHECK(memcmp(
            complete->depth_implementation_32.data +
                selected_offsets[index],
            golden->depth_selected_be[index],
            4U
        ) == 0);
    }
    return true;
}

static bool render_active(
    Fixture *fixture,
    const MeisoCompositionSceneWorldView *active,
    MeisoCompositionReferenceRendererCompleteView *out_complete
)
{
    MeisoCompositionReferenceRendererJobRef job;

    CHECK(meiso_composition_reference_renderer_submit(
        fixture->renderer, &fixture->scope, active, 55U, &job
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture->renderer, &job, 1000U, 1010U, out_complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE);
    return true;
}

static MeisoCompositionSceneDesired scene_value(MeisoContentId mesh_id)
{
    MeisoCompositionSceneDesired value;
    MeisoCompositionSceneObject *object;

    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = 9U;
    value.camera.source_time = (MeisoTimePoint){7U, 100U};
    value.camera.position[2] = -3.0F;
    value.camera.orientation_xyzw[3] = 1.0F;
    value.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    value.camera.vertical_fov_degrees = 60.0F;
    value.camera.near_z = 0.1F;
    value.camera.far_z = 100.0F;
    value.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.object_count = 1U;
    object = &value.objects[0];
    object->id = (MeisoCompositionSceneObjectId){1U, 1U};
    object->mesh_content_id = mesh_id;
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    object->space_epoch = value.camera.space_epoch;
    object->transform.orientation_xyzw[3] = 1.0F;
    object->transform.scale[0] = 1.0F;
    object->transform.scale[1] = 1.0F;
    object->transform.scale[2] = 1.0F;
    object->flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
    return value;
}

static bool activate_scene(
    Fixture *fixture,
    const MeisoCompositionSceneDesired *desired,
    uint64_t desired_version,
    uint64_t plan_generation,
    MeisoCompositionSceneWorldView *out_active
)
{
    MeisoCompositionSceneWorldRef ref;

    CHECK(meiso_composition_scene_store_prepare(
        fixture->scene_store,
        fixture->host_owner.incarnation,
        desired_version,
        plan_generation,
        desired
    ) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(meiso_composition_scene_store_activate(
        fixture->scene_store,
        fixture->host_owner.incarnation,
        desired_version,
        plan_generation,
        &ref
    ) == MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED);
    CHECK(ref.world_generation != 0U &&
          meiso_composition_scene_store_active(
              fixture->scene_store, out_active
          ) && world_ref_equal(ref, out_active->ref));
    return true;
}

static void fixture_destroy(Fixture *fixture)
{
    free(fixture->renderer_arena);
    free(fixture->scene_arena);
    free(fixture->blob_arena);
    memset(fixture, 0, sizeof(*fixture));
}

static bool fixture_init(Fixture *fixture)
{
    const MeisoResourceDescriptor *mesh =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig blob_config = {
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    };

    memset(fixture, 0, sizeof(*fixture));
    fixture->host_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, UINT64_C(0x7001)
    };
    CHECK(mesh != NULL && encode_cube(fixture));
    fixture->blob_arena_size = meiso_blob_store_memory_required(
        mesh, &blob_config
    );
    CHECK(fixture->blob_arena_size != 0U);
    fixture->blob_arena = calloc(1U, fixture->blob_arena_size);
    CHECK(fixture->blob_arena != NULL);
    CHECK(meiso_blob_store_init(
        fixture->blob_arena,
        fixture->blob_arena_size,
        mesh,
        &blob_config,
        &fixture->blob_store
    ) == MEISO_BLOB_INIT_OK);
    fixture->scene_arena_size =
        meiso_composition_scene_store_memory_required();
    CHECK(fixture->scene_arena_size != 0U);
    fixture->scene_arena = calloc(1U, fixture->scene_arena_size);
    CHECK(fixture->scene_arena != NULL);
    CHECK(meiso_composition_scene_store_init(
        fixture->scene_arena,
        fixture->scene_arena_size,
        fixture->blob_store,
        &fixture->scene_store
    ) == MEISO_COMPOSITION_SCENE_STORE_INIT_OK);
    CHECK(meiso_composition_scene_store_begin_owner(
        fixture->scene_store, fixture->host_owner.incarnation
    ) == MEISO_COMPOSITION_SCENE_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_control_scope(
        fixture->scene_store, &fixture->scope
    ));
    fixture->renderer_arena_size =
        meiso_composition_reference_renderer_memory_required();
    CHECK(fixture->renderer_arena_size >
          MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES);
    fixture->renderer_arena = calloc(1U, fixture->renderer_arena_size);
    CHECK(fixture->renderer_arena != NULL);
    CHECK(meiso_composition_reference_renderer_init(
        fixture->renderer_arena,
        fixture->renderer_arena_size,
        fixture->blob_store,
        &fixture->renderer
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_OK);
    CHECK(meiso_composition_reference_renderer_begin_control_scope(
        fixture->renderer, &fixture->scope
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_STARTED);
    CHECK(publish_cube(fixture, false));
    return true;
}

static bool check_failed_init_is_atomic(
    MeisoBlobStore *blob_store,
    size_t required,
    size_t alignment,
    uint32_t variant
)
{
    MeisoCompositionReferenceRenderer *renderer =
        (MeisoCompositionReferenceRenderer *)(uintptr_t)UINTPTR_MAX;
    uint8_t *storage = NULL;
    uint8_t *before = NULL;
    uint8_t *arena;
    size_t storage_size;
    size_t arena_size;
    MeisoCompositionReferenceRendererInitStatus expected;
    bool okay = false;

    if (variant == 0U) {
        storage_size = required;
        arena_size = required - 1U;
        expected = MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_ARENA_TOO_SMALL;
    } else if (variant == 1U) {
        storage_size = required + alignment;
        arena_size = required;
        expected = MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_MISALIGNED_ARENA;
    } else {
        storage_size = required;
        arena_size = required;
        expected = MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_ARGUMENT;
    }
    storage = (uint8_t *)malloc(storage_size);
    before = (uint8_t *)malloc(storage_size);
    if (storage == NULL || before == NULL) {
        goto cleanup;
    }
    memset(storage, variant == 1U ? 0x5a : 0xa5, storage_size);
    memcpy(before, storage, storage_size);
    arena = variant == 1U ? storage + 1U : storage;
    if (meiso_composition_reference_renderer_init(
            arena,
            arena_size,
            blob_store,
            variant == 2U ?
                (MeisoCompositionReferenceRenderer **)(void *)arena :
                &renderer
        ) != expected) {
        goto cleanup;
    }
    okay = memcmp(storage, before, storage_size) == 0 &&
        (variant == 2U || renderer ==
            (MeisoCompositionReferenceRenderer *)(uintptr_t)UINTPTR_MAX);

cleanup:
    free(before);
    free(storage);
    return okay;
}

static bool check_extra_arena_metadata(
    MeisoBlobStore *blob_store,
    size_t required,
    size_t extra
)
{
    MeisoCompositionReferenceRenderer *renderer = NULL;
    MeisoCompositionReferenceRendererSnapshot snapshot;
    uint8_t *arena = (uint8_t *)calloc(1U, required + extra);
    bool okay = false;

    if (arena == NULL) {
        return false;
    }
    if (meiso_composition_reference_renderer_init(
            arena, required + extra, blob_store, &renderer
        ) == MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_OK &&
        meiso_composition_reference_renderer_snapshot(renderer, &snapshot) &&
        snapshot.target_slots_allocated == 2U &&
        snapshot.target_bytes ==
            MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES &&
        snapshot.renderer_metadata_bytes == required + extra -
            MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES) {
        okay = true;
    }
    free(arena);
    return okay;
}

static bool test_memory_alignment_and_alias_contract(void)
{
    Fixture fixture;
    MeisoCompositionReferenceRendererSnapshot snapshot;
    size_t required;
    size_t alignment;

    CHECK(fixture_init(&fixture));
    required = meiso_composition_reference_renderer_memory_required();
    alignment = meiso_composition_reference_renderer_alignment_required();
    CHECK(required == fixture.renderer_arena_size && required >
              MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES &&
          alignment != 0U &&
          MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES ==
              UINT64_C(1228800));
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &snapshot
    ));
    CHECK(snapshot.control_scope_bound &&
          control_scope_equal(snapshot.control_scope, fixture.scope) &&
          snapshot.target_slots_allocated ==
              MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT &&
          snapshot.target_bytes ==
              MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES &&
          snapshot.renderer_metadata_bytes ==
              required -
                  MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES &&
          snapshot.queued_jobs == 0U && !snapshot.complete_available);

    CHECK(required > 1U &&
          check_failed_init_is_atomic(
              fixture.blob_store, required, alignment, 0U
          ) &&
          check_failed_init_is_atomic(
              fixture.blob_store, required, alignment, 1U
          ) &&
          check_failed_init_is_atomic(
              fixture.blob_store, required, alignment, 2U
          ) &&
          check_extra_arena_metadata(
              fixture.blob_store, required, alignment
          ));
    fixture_destroy(&fixture);
    return true;
}

static bool test_two_job_capacity_and_atomic_complete(void)
{
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionReferenceRendererJobRef first;
    MeisoCompositionReferenceRendererJobRef second;
    MeisoCompositionReferenceRendererJobRef unchanged;
    MeisoCompositionReferenceRendererSnapshot snapshot;
    MeisoCompositionReferenceRendererCompleteView complete;
    MeisoCompositionReferenceRendererCompleteView latest;
    MeisoCompositionReferenceRendererCompleteView output;
    MeisoCompositionReferenceRendererStats stats;
    MeisoByteView old_color;
    MeisoByteView old_depth;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &first
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &second
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(first.job_id != 0U && second.job_id == first.job_id + 1U &&
          first.renderer_generation == second.renderer_generation &&
          control_scope_equal(first.control_scope, fixture.scope));
    memset(&unchanged, 0xa5, sizeof(unchanged));
    {
        const MeisoCompositionReferenceRendererJobRef before = unchanged;

        CHECK(meiso_composition_reference_renderer_submit(
            fixture.renderer, &fixture.scope, &active, 55U, &unchanged
        ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUE_FULL);
        CHECK(memcmp(&before, &unchanged, sizeof(before)) == 0);
    }
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &snapshot
    ));
    CHECK(snapshot.queued_jobs == 2U &&
          job_ref_equal(snapshot.head_job, first) &&
          !snapshot.complete_available);

    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCompositionReferenceRendererCompleteView before = output;

        CHECK(meiso_composition_reference_renderer_render_next(
            fixture.renderer, &second, 1000U, 1010U, &output
        ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_JOB_MISMATCH);
        CHECK(memcmp(&before, &output, sizeof(before)) == 0);
    }
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCompositionReferenceRendererCompleteView before = output;

        CHECK(meiso_composition_reference_renderer_render_next(
            fixture.renderer, &first, 1010U, 1000U, &output
        ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_TIME);
        CHECK(memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE);
    CHECK(job_ref_equal(complete.job, first) &&
          complete.result.evidence_flags ==
              MEISO_COMPOSITION_REFERENCE_RENDERER_EVIDENCE_FLAGS &&
          complete.result.evidence_flags == UINT32_C(0x00000003) &&
          complete.color_rgba8888.size ==
              MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE &&
          complete.depth_implementation_32.size ==
              MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE &&
          meiso_composition_reference_renderer_latest_complete(
              fixture.renderer, &latest
          ) && job_ref_equal(latest.job, first) &&
          latest.target_slot == complete.target_slot &&
          latest.color_rgba8888.data == complete.color_rgba8888.data &&
          latest.depth_implementation_32.data ==
              complete.depth_implementation_32.data);
    old_color = complete.color_rgba8888;
    old_depth = complete.depth_implementation_32;
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[0]));

    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCompositionReferenceRendererCompleteView before = output;

        CHECK(meiso_composition_reference_renderer_render_next(
            fixture.renderer, &second, 1020U, 1010U, &output
        ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_TIME);
        CHECK(memcmp(&before, &output, sizeof(before)) == 0 &&
              sha256_matches(old_color, RENDER_GOLDENS[0].color_sha256) &&
              sha256_matches(old_depth, RENDER_GOLDENS[0].depth_sha256) &&
              meiso_composition_reference_renderer_latest_complete(
                  fixture.renderer, &latest
              ) && job_ref_equal(latest.job, first));
    }
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &second, 1020U, 1030U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &snapshot
    ));
    CHECK(meiso_composition_reference_renderer_stats(
        fixture.renderer, &stats
    ));
    CHECK(job_ref_equal(complete.job, second) &&
          complete.target_slot != latest.target_slot &&
          snapshot.queued_jobs == 0U &&
          snapshot.complete_available &&
          job_ref_equal(snapshot.latest_complete_job, second) &&
          stats.jobs_submitted == 2U &&
          stats.queue_capacity_rejections == 1U &&
          stats.renders_completed == 2U &&
          stats.render_failures == 2U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_fixed_render_goldens_and_mesh_dependency(void)
{
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionReferenceRendererCompleteView complete;
    MeisoCompositionReferenceRendererCompleteView repeated;
    MeisoCompositionSceneRenderResult first_result;
    MeisoCompositionSceneRenderResult camera_result;
    MeisoCompositionSceneWorldRef retired_world;
    uint8_t first_result_wire[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    uint8_t repeated_result_wire[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    uint8_t triangle_storage[256];
    MeisoByteView triangle;
    MeisoContentId triangle_id;
    size_t first_result_size = 0U;
    size_t repeated_result_size = 0U;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[0]));
    first_result = complete.result;
    CHECK(meiso_composition_scene_render_result_encode(
        &first_result,
        (MeisoMutableBuffer){first_result_wire, sizeof(first_result_wire)},
        &first_result_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);

    CHECK(render_active(&fixture, &active, &repeated));
    CHECK(verify_render_golden(&repeated, &RENDER_GOLDENS[0]) &&
          repeated.target_slot != complete.target_slot);
    CHECK(meiso_composition_scene_render_result_encode(
        &repeated.result,
        (MeisoMutableBuffer){
            repeated_result_wire, sizeof(repeated_result_wire)
        },
        &repeated_result_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(first_result_size == repeated_result_size &&
          memcmp(first_result_wire, repeated_result_wire,
                 first_result_size) == 0);

    retired_world = active.ref;
    desired.objects[0].transform.position[0] = 1.0F;
    CHECK(activate_scene(&fixture, &desired, 2U, 71U, &active));
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired_world
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    CHECK(!meiso_composition_reference_renderer_latest_complete(
        fixture.renderer, &complete
    ));
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[1]) &&
          memcmp(first_result.full_target_sha256,
                 complete.result.full_target_sha256,
                 MEISO_SHA256_DIGEST_SIZE) != 0 &&
          memcmp(first_result.thumbnail_rgb565,
                 complete.result.thumbnail_rgb565,
                 MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) != 0);

    retired_world = active.ref;
    desired.camera.position[1] = 1.0F;
    CHECK(activate_scene(&fixture, &desired, 3U, 71U, &active));
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired_world
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[2]));
    camera_result = complete.result;

    CHECK(encode_triangle(
        triangle_storage, sizeof(triangle_storage), &triangle, &triangle_id
    ));
    CHECK(publish_mesh(&fixture, triangle_id, triangle));
    retired_world = active.ref;
    desired.object_count = 2U;
    desired.objects[1] = desired.objects[0];
    desired.objects[1].id = (MeisoCompositionSceneObjectId){1U, 2U};
    desired.objects[1].mesh_content_id = triangle_id;
    desired.objects[1].transform.position[0] = 2.0F;
    CHECK(activate_scene(&fixture, &desired, 4U, 71U, &active));
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired_world
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(complete.result.object_count == 2U &&
          complete.result.unique_mesh_count == 2U &&
          sha256_matches(
              complete.color_rgba8888,
              complete.result.full_target_sha256
          ) && verify_thumbnail_samples(&complete) &&
          verify_result_codec(&complete.result) &&
          memcmp(camera_result.full_target_sha256,
                 complete.result.full_target_sha256,
                 MEISO_SHA256_DIGEST_SIZE) != 0 &&
          memcmp(camera_result.thumbnail_rgb565,
                 complete.result.thumbnail_rgb565,
                 MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) != 0);

    fixture_destroy(&fixture);
    return true;
}

static bool test_external_core_stream_queue(void)
{
    const MeisoResourceDescriptor *descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_RENDER_RESULT
        );
    const MeisoStreamConfig config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS,
        0U,
        0U
    };
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 0x8001U};
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionReferenceRendererCompleteView complete;
    MeisoCompositionReferenceRendererSnapshot renderer_snapshot;
    MeisoStream *stream;
    void *stream_arena;
    size_t stream_arena_size;
    uint64_t sequence;

    CHECK(fixture_init(&fixture));
    CHECK(descriptor != NULL &&
          MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS == 4U);
    stream_arena_size = meiso_stream_memory_required(descriptor, &config);
    CHECK(stream_arena_size != 0U);
    stream_arena = calloc(1U, stream_arena_size);
    CHECK(stream_arena != NULL);
    CHECK(meiso_stream_init(
        stream_arena, stream_arena_size, descriptor, &config, &stream
    ) == MEISO_STREAM_INIT_OK);
    CHECK(meiso_stream_begin_producer(stream, producer, 31U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));

    for (sequence = 1U; sequence <= 5U; ++sequence) {
        uint8_t payload[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
        MeisoStreamSample sample;
        MeisoStreamReceiveResult receive;
        size_t payload_size = 0U;

        CHECK(render_active(&fixture, &active, &complete));
        CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[0]));
        CHECK(meiso_composition_scene_render_result_encode(
            &complete.result,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
        memset(&sample, 0, sizeof(sample));
        sample.key = (MeisoStreamKey){producer, 31U, sequence};
        sample.source_time = (MeisoTimePoint){
            complete.result.source_clock_epoch,
            complete.result.render_end_monotonic_ns
        };
        sample.payload = (MeisoByteView){payload, payload_size};
        receive = meiso_stream_receive(stream, &sample, NULL);
        CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
              receive.missing_before == 0U &&
              receive.flags == (sequence == 5U ?
                  MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST :
                  MEISO_STREAM_RECEIVE_FLAG_NONE));
        CHECK(meiso_composition_scene_store_record_result_published(
            fixture.scene_store, &active.ref, 31U, sequence
        ));
    }
    CHECK(meiso_stream_count(stream) ==
          MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &renderer_snapshot
    ) && renderer_snapshot.queued_jobs == 0U &&
          renderer_snapshot.complete_available &&
          renderer_snapshot.target_slots_allocated == 2U);
    for (sequence = 2U; sequence <= 5U; ++sequence) {
        MeisoCompositionSceneRenderResult decoded;
        MeisoStreamSample queued;

        CHECK(meiso_stream_peek(stream, &queued) &&
              queued.key.sequence == sequence &&
              queued.key.epoch == 31U &&
              queued.key.producer.incarnation == producer.incarnation &&
              meiso_composition_scene_render_result_decode(
                  queued.payload, &decoded
              ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
              memcmp(decoded.full_target_sha256,
                     RENDER_GOLDENS[0].color_sha256,
                     MEISO_SHA256_DIGEST_SIZE) == 0);
        CHECK(meiso_stream_pop(stream));
    }
    CHECK(meiso_stream_count(stream) == 0U);
    free(stream_arena);
    fixture_destroy(&fixture);
    return true;
}

static bool check_retire_zero_diff(
    Fixture *fixture,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldRef *world,
    MeisoCompositionReferenceRendererRetireStatus expected
)
{
    uint8_t *before =
        (uint8_t *)malloc(fixture->renderer_arena_size);
    MeisoCompositionReferenceRendererRetireStatus status;
    bool okay;

    if (before == NULL) {
        return false;
    }
    memcpy(before, fixture->renderer_arena, fixture->renderer_arena_size);
    status = meiso_composition_reference_renderer_retire_world(
        fixture->renderer, scope, world
    );
    okay = status == expected &&
        memcmp(before, fixture->renderer_arena,
               fixture->renderer_arena_size) == 0;
    free(before);
    return okay;
}

static bool test_world_retire_boundary(void)
{
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneDesired old_desired_copy;
    MeisoCompositionSceneWorldView old_active;
    MeisoCompositionSceneWorldView stale_old_view;
    MeisoCompositionSceneWorldView new_active;
    MeisoCompositionSceneWorldView deleted_active;
    MeisoCompositionReferenceRendererJobRef old_pending;
    MeisoCompositionReferenceRendererJobRef new_pending;
    MeisoCompositionReferenceRendererJobRef stale_old_pending;
    MeisoCompositionReferenceRendererCompleteView complete;
    MeisoCompositionReferenceRendererCompleteView latest;
    MeisoCompositionReferenceRendererSnapshot snapshot;
    MeisoCompositionReferenceRendererStats stats;
    MeisoCompositionSceneControlScope wrong_scope;
    MeisoByteView retired_color;
    MeisoByteView retired_depth;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &old_active));
    old_desired_copy = *old_active.desired;
    stale_old_view = old_active;
    stale_old_view.desired = &old_desired_copy;
    CHECK(render_active(&fixture, &old_active, &complete));
    retired_color = complete.color_rgba8888;
    retired_depth = complete.depth_implementation_32;
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &old_active, 55U, &old_pending
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);

    desired.objects[0].transform.position[0] = 1.0F;
    CHECK(activate_scene(&fixture, &desired, 2U, 71U, &new_active));
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &new_active, 55U, &new_pending
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    wrong_scope = fixture.scope;
    ++wrong_scope.control_scope_generation;
    CHECK(check_retire_zero_diff(
        &fixture,
        &wrong_scope,
        &old_active.ref,
        MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_SCOPE_MISMATCH
    ));
    CHECK(check_retire_zero_diff(
        &fixture,
        (const MeisoCompositionSceneControlScope *)fixture.renderer_arena,
        &old_active.ref,
        MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_INVALID_ARGUMENT
    ));

    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &old_active.ref
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &snapshot
    ) && snapshot.queued_jobs == 1U &&
          job_ref_equal(snapshot.head_job, new_pending) &&
          !snapshot.complete_available && bytes_are_zero(retired_color) &&
          bytes_are_zero(retired_depth));
    memset(&complete, 0xa5, sizeof(complete));
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &old_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_JOB_MISMATCH);

    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &new_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE);
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[1]));
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &old_active.ref
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_UNCHANGED);
    CHECK(meiso_composition_reference_renderer_latest_complete(
        fixture.renderer, &latest
    ) && world_ref_equal(latest.job.world, new_active.ref) &&
          verify_render_golden(&latest, &RENDER_GOLDENS[1]));
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &new_active, 55U, &new_pending
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &stale_old_view, 55U,
        &stale_old_pending
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &old_active.ref
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &snapshot
    ) && snapshot.queued_jobs == 1U &&
          job_ref_equal(snapshot.head_job, new_pending) &&
          snapshot.complete_available &&
          world_ref_equal(snapshot.latest_complete_job.world,
                          new_active.ref));
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &stale_old_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_JOB_MISMATCH);
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &new_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE);
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &new_active, 55U, &new_pending
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);

    desired.object_count = 0U;
    memset(desired.objects, 0, sizeof(desired.objects));
    CHECK(activate_scene(&fixture, &desired, 3U, 71U, &deleted_active));
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &new_active.ref
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &snapshot
    ) && snapshot.queued_jobs == 0U && !snapshot.complete_available);
    memset(&complete, 0xa5, sizeof(complete));
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &new_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB);
    CHECK(meiso_composition_scene_store_disable(
        fixture.scene_store, &deleted_active.ref
    ) == MEISO_COMPOSITION_SCENE_DISABLE_DISABLED);
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &deleted_active.ref
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_UNCHANGED);
    CHECK(!meiso_composition_reference_renderer_latest_complete(
        fixture.renderer, &complete
    ));
    CHECK(meiso_composition_reference_renderer_stats(
        fixture.renderer, &stats
    ) && stats.world_retires == 3U && stats.jobs_retired == 3U &&
          stats.completes_retired == 2U);

    fixture_destroy(&fixture);
    return true;
}

static bool check_public_aliases_zero_diff(
    Fixture *fixture,
    const MeisoCompositionSceneWorldView *active,
    MeisoCompositionReferenceRendererJobRef *job
)
{
    MeisoCompositionSceneControlScope scope_before;
    MeisoCompositionReferenceRendererJobRef job_before;
    uint8_t *before =
        (uint8_t *)malloc(fixture->renderer_arena_size);
    bool okay = false;

    if (before == NULL) {
        return false;
    }
#define SAVE_RENDERER()                                                        \
    memcpy(before, fixture->renderer_arena, fixture->renderer_arena_size)
#define RENDERER_UNCHANGED()                                                   \
    (memcmp(before, fixture->renderer_arena, fixture->renderer_arena_size) == 0)

    SAVE_RENDERER();
    if (meiso_composition_reference_renderer_begin_control_scope(
            fixture->renderer,
            (const MeisoCompositionSceneControlScope *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    SAVE_RENDERER();
    if (meiso_composition_reference_renderer_submit(
            fixture->renderer,
            &fixture->scope,
            active,
            55U,
            (MeisoCompositionReferenceRendererJobRef *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    scope_before = fixture->scope;
    SAVE_RENDERER();
    if (meiso_composition_reference_renderer_submit(
            fixture->renderer,
            &fixture->scope,
            active,
            55U,
            (MeisoCompositionReferenceRendererJobRef *)(void *)
                &fixture->scope
        ) != MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED() ||
        memcmp(&scope_before, &fixture->scope, sizeof(scope_before)) != 0) {
        goto cleanup;
    }
    SAVE_RENDERER();
    if (meiso_composition_reference_renderer_render_next(
            fixture->renderer,
            job,
            1000U,
            1010U,
            (MeisoCompositionReferenceRendererCompleteView *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    job_before = *job;
    SAVE_RENDERER();
    if (meiso_composition_reference_renderer_render_next(
            fixture->renderer,
            job,
            1000U,
            1010U,
            (MeisoCompositionReferenceRendererCompleteView *)(void *)job
        ) != MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED() ||
        memcmp(&job_before, job, sizeof(job_before)) != 0) {
        goto cleanup;
    }
    SAVE_RENDERER();
    if (meiso_composition_reference_renderer_discard_head(
            fixture->renderer,
            (const MeisoCompositionReferenceRendererJobRef *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_INVALID_ARGUMENT ||
        meiso_composition_reference_renderer_snapshot(
            fixture->renderer,
            (MeisoCompositionReferenceRendererSnapshot *)
                fixture->renderer_arena
        ) ||
        meiso_composition_reference_renderer_stats(
            fixture->renderer,
            (MeisoCompositionReferenceRendererStats *)
                fixture->renderer_arena
        ) ||
        meiso_composition_reference_renderer_latest_complete(
            fixture->renderer,
            (MeisoCompositionReferenceRendererCompleteView *)
                fixture->renderer_arena
        ) ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    okay = true;

cleanup:
#undef RENDERER_UNCHANGED
#undef SAVE_RENDERER
    free(before);
    return okay;
}

static bool test_public_alias_rejection(void)
{
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionReferenceRendererJobRef job;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));

    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &job
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(check_public_aliases_zero_diff(&fixture, &active, &job));

    CHECK(meiso_composition_reference_renderer_discard_head(
        fixture.renderer, &job
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_DISCARDED);
    fixture_destroy(&fixture);
    return true;
}

static bool test_scope_reset_discard_and_disable_no_result(void)
{
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionReferenceRendererJobRef first;
    MeisoCompositionReferenceRendererJobRef second;
    MeisoCompositionReferenceRendererCompleteView output;
    MeisoCompositionReferenceRendererSnapshot before;
    MeisoCompositionReferenceRendererSnapshot after;
    MeisoCompositionSceneControlScope old_scope;
    MeisoCompositionSceneControlScope new_scope;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));
    active_ref = active.ref;
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &first
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &second
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &before
    ));
    CHECK(meiso_composition_reference_renderer_discard_head(
        fixture.renderer, &second
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_JOB_MISMATCH);
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &after
    ) && after.queued_jobs == before.queued_jobs &&
          job_ref_equal(after.head_job, before.head_job));
    CHECK(meiso_composition_reference_renderer_discard_head(
        fixture.renderer, &first
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_DISCARDED);
    CHECK(meiso_composition_reference_renderer_discard_head(
        fixture.renderer, &second
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_DISCARDED);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &output
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB);
    CHECK(!meiso_composition_reference_renderer_latest_complete(
        fixture.renderer, &output
    ));

    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &first
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    old_scope = fixture.scope;
    CHECK(meiso_composition_reference_renderer_begin_control_scope(
        fixture.renderer, &old_scope
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_UNCHANGED);
    CHECK(meiso_composition_scene_store_reset_control_scope(
        fixture.scene_store, fixture.host_owner.incarnation
    ) == MEISO_COMPOSITION_SCENE_OWNER_SCOPE_RESET);
    CHECK(meiso_composition_scene_store_control_scope(
        fixture.scene_store, &new_scope
    ) && !control_scope_equal(old_scope, new_scope));
    CHECK(meiso_composition_reference_renderer_begin_control_scope(
        fixture.renderer, &new_scope
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_STARTED);
    fixture.scope = new_scope;
    CHECK(meiso_composition_reference_renderer_snapshot(
        fixture.renderer, &after
    ) && after.queued_jobs == 0U && !after.complete_available &&
          control_scope_equal(after.control_scope, new_scope));
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &output
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB);
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &old_scope, &active, 55U, &second
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_SCOPE_MISMATCH);

    CHECK(publish_cube(&fixture, true));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 2U, 72U, &active));
    CHECK(meiso_composition_reference_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 56U, &first
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED);
    CHECK(meiso_composition_scene_store_disable(
        fixture.scene_store, &active.ref
    ) == MEISO_COMPOSITION_SCENE_DISABLE_DISABLED);
    CHECK(meiso_composition_reference_renderer_retire_world(
        fixture.renderer, &fixture.scope, &active.ref
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_composition_reference_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &output
    ) == MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &active
    ));
    CHECK(active_ref.world_generation != 0U);
    fixture_destroy(&fixture);
    return true;
}

int main(void)
{
    if (!test_memory_alignment_and_alias_contract() ||
        !test_two_job_capacity_and_atomic_complete() ||
        !test_fixed_render_goldens_and_mesh_dependency() ||
        !test_external_core_stream_queue() ||
        !test_world_retire_boundary() ||
        !test_public_alias_rejection() ||
        !test_scope_reset_discard_and_disable_no_result()) {
        return 1;
    }
    (void)puts("composition reference renderer tests passed");
    return 0;
}
