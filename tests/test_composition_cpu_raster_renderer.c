#include "composition_cpu_raster_renderer.h"

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
    MeisoCompositionCpuRasterRenderer *renderer;
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

/*
 * These literals are pinned to this renderer source on the macOS arm64 host
 * used to create the feasibility receipt. They are deliberately not a wire
 * or cross-target promise; every platform still runs the independent raster
 * geometry, target/result consistency, and repeatability assertions below.
 */
static const RenderGolden RENDER_GOLDENS[3] = {
    {
        {0x2c, 0x80, 0xbc, 0x4d, 0xf0, 0x59, 0x9a, 0xa8,
         0xc2, 0xdf, 0xcf, 0xb6, 0x36, 0x85, 0x4e, 0x3f,
         0x24, 0x1e, 0x4f, 0x18, 0xe3, 0x3a, 0x27, 0xac,
         0x19, 0x7f, 0xd6, 0xc1, 0x6c, 0x22, 0x4a, 0x97},
        {0xa1, 0xfb, 0x58, 0x6d, 0xc7, 0x85, 0xf5, 0x96,
         0x26, 0xc2, 0xa7, 0x22, 0x2b, 0x15, 0x0e, 0xe6,
         0xe9, 0x65, 0xb0, 0x6a, 0xb5, 0x8c, 0xa5, 0x32,
         0x6d, 0x50, 0xc7, 0x5e, 0x89, 0xbe, 0x2c, 0x05},
        {0x4f, 0x2a, 0x05, 0x0a, 0xa1, 0xbb, 0x77, 0xbf,
         0x66, 0x41, 0xd7, 0x08, 0x11, 0x77, 0xfb, 0xda,
         0x10, 0x4f, 0x7f, 0xb4, 0xc7, 0x2d, 0x6a, 0x28,
         0x78, 0x7d, 0x3f, 0x22, 0x51, 0xed, 0x0e, 0x01},
        {{0xff, 0xff, 0xff, 0xff},
         {0xf6, 0x01, 0x89, 0x98},
         {0xff, 0xff, 0xff, 0xff}}
    },
    {
        {0x66, 0x4d, 0x8a, 0x1c, 0x02, 0x00, 0xc2, 0xe3,
         0xa4, 0xe6, 0x8e, 0x78, 0x41, 0xfd, 0x5a, 0x85,
         0x4d, 0x6f, 0x6a, 0x28, 0xd9, 0x2e, 0x45, 0x2b,
         0xd3, 0x66, 0x4d, 0x97, 0x58, 0x23, 0x54, 0xb5},
        {0x0c, 0xb9, 0xf6, 0xd9, 0x1f, 0x32, 0xe0, 0x18,
         0xf1, 0x4f, 0x66, 0xd0, 0xd2, 0xb8, 0xcd, 0x9b,
         0xf7, 0x0f, 0x73, 0x25, 0xda, 0xa6, 0x8d, 0xa9,
         0x4c, 0xe2, 0xa7, 0x36, 0xa4, 0x3c, 0xda, 0x36},
        {0xc2, 0x26, 0x06, 0x04, 0xf6, 0x2d, 0x6a, 0x20,
         0xf6, 0xa1, 0x99, 0x87, 0x34, 0xf6, 0xb4, 0xb3,
         0x99, 0x26, 0x74, 0x99, 0x62, 0x7a, 0x8c, 0xa3,
         0x57, 0x7a, 0x3d, 0xd1, 0x35, 0x9c, 0xe0, 0xa3},
        {{0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff}}
    },
    {
        {0x63, 0x94, 0xb7, 0x67, 0xe7, 0x6e, 0xa8, 0x42,
         0x3b, 0x03, 0x8f, 0xb7, 0x3a, 0x8a, 0x4b, 0x79,
         0x50, 0x75, 0x16, 0x29, 0x84, 0x7b, 0x64, 0xf7,
         0xdc, 0x74, 0x91, 0xad, 0x17, 0x05, 0xc4, 0xe2},
        {0xf9, 0x16, 0x37, 0x73, 0x73, 0x01, 0x84, 0xfc,
         0x9a, 0x52, 0x0e, 0xdf, 0x2a, 0x13, 0x4b, 0x20,
         0x3a, 0xa6, 0xde, 0x1e, 0xfc, 0x4d, 0x4a, 0x79,
         0x54, 0x95, 0xbd, 0x7f, 0xf8, 0xa2, 0x78, 0x2c},
        {0x45, 0x6a, 0x89, 0xf6, 0xe0, 0xe8, 0xf0, 0x98,
         0x18, 0xa9, 0xe8, 0xc1, 0xba, 0x33, 0x3c, 0x14,
         0xb4, 0x3b, 0x04, 0xb0, 0x38, 0x43, 0xb6, 0xe0,
         0xb4, 0x5b, 0x18, 0x43, 0x92, 0x9c, 0x9f, 0x21},
        {{0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff}}
    }
};

static bool source_host_golden_enabled(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

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
    MeisoCompositionCpuRasterJobRef lhs,
    MeisoCompositionCpuRasterJobRef rhs
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

static bool encode_mesh(
    const MeisoCompositionSceneMeshVertex *vertices,
    uint32_t vertex_count,
    const uint32_t *indices,
    uint32_t index_count,
    bool has_vertex_color,
    uint8_t *storage,
    size_t capacity,
    MeisoByteView *out_bytes,
    MeisoContentId *out_id
)
{
    MeisoCompositionSceneMesh mesh;
    size_t encoded_size = 0U;

    memset(&mesh, 0, sizeof(mesh));
    mesh.has_vertex_color = has_vertex_color;
    mesh.vertex_count = vertex_count;
    mesh.vertices = vertices;
    mesh.index_count = index_count;
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

static void set_vertex(
    MeisoCompositionSceneMeshVertex *vertex,
    float x,
    float y,
    float z,
    uint8_t red,
    uint8_t green,
    uint8_t blue
)
{
    memset(vertex, 0, sizeof(*vertex));
    vertex->position[0] = x;
    vertex->position[1] = y;
    vertex->position[2] = z;
    vertex->color_rgba[0] = red;
    vertex->color_rgba[1] = green;
    vertex->color_rgba[2] = blue;
    vertex->color_rgba[3] = UINT8_C(0xff);
}

static const uint8_t *color_pixel(
    const MeisoCompositionCpuRasterCompleteView *complete,
    uint32_t x,
    uint32_t y
)
{
    const size_t offset =
        ((size_t)y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH + x) * 4U;

    return complete->color_rgba8888.data + offset;
}

static uint32_t depth_pixel(
    const MeisoCompositionCpuRasterCompleteView *complete,
    uint32_t x,
    uint32_t y
)
{
    const uint8_t *pixel = complete->depth_implementation_32.data +
        ((size_t)y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH + x) * 4U;

    return ((uint32_t)pixel[0] << 24U) |
        ((uint32_t)pixel[1] << 16U) |
        ((uint32_t)pixel[2] << 8U) | (uint32_t)pixel[3];
}

static bool is_clear_pixel(const uint8_t *rgba)
{
    return rgba[0] == MEISO_COMPOSITION_CPU_RASTER_CLEAR_R &&
        rgba[1] == MEISO_COMPOSITION_CPU_RASTER_CLEAR_G &&
        rgba[2] == MEISO_COMPOSITION_CPU_RASTER_CLEAR_B &&
        rgba[3] == MEISO_COMPOSITION_CPU_RASTER_CLEAR_A;
}

static bool verify_thumbnail_samples(
    const MeisoCompositionCpuRasterCompleteView *complete
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
    const MeisoCompositionCpuRasterCompleteView *complete,
    const RenderGolden *golden
)
{
    static const size_t selected_offsets[3] = {
        0U,
        ((size_t)(MEISO_COMPOSITION_SCENE_TARGET_HEIGHT / 2U) *
             MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
         MEISO_COMPOSITION_SCENE_TARGET_WIDTH / 2U) * 4U,
        MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE - 4U
    };
    const MeisoByteView thumbnail = {
        complete->result.thumbnail_rgb565,
        complete->result.thumbnail_byte_size
    };
    uint32_t index;


    CHECK(complete->color_rgba8888.size ==
              MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE &&
          complete->depth_implementation_32.size ==
              MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE &&
          complete->result.evidence_flags == UINT32_C(0x00000003) &&
          (!source_host_golden_enabled() ||
           (sha256_matches(complete->color_rgba8888,
                           golden->color_sha256) &&
            memcmp(complete->result.full_target_sha256,
                   golden->color_sha256,
                   MEISO_SHA256_DIGEST_SIZE) == 0 &&
            sha256_matches(complete->depth_implementation_32,
                           golden->depth_sha256) &&
            sha256_matches(thumbnail, golden->thumbnail_sha256))) &&
          verify_thumbnail_samples(complete) &&
          verify_result_codec(&complete->result));
    for (index = 0U; source_host_golden_enabled() && index < 3U; ++index) {
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
    MeisoCompositionCpuRasterCompleteView *out_complete
)
{
    MeisoCompositionCpuRasterJobRef job;

    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture->renderer, &fixture->scope, active, 55U, &job
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture->renderer, &job, 1000U, 1010U, out_complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE);
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
    /* The CPU backend's camera looks down local -Z. */
    value.camera.position[2] = 3.0F;
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
        meiso_composition_cpu_raster_renderer_memory_required();
    CHECK(fixture->renderer_arena_size >
          MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES);
    fixture->renderer_arena = calloc(1U, fixture->renderer_arena_size);
    CHECK(fixture->renderer_arena != NULL);
    CHECK(meiso_composition_cpu_raster_renderer_init(
        fixture->renderer_arena,
        fixture->renderer_arena_size,
        fixture->blob_store,
        &fixture->renderer
    ) == MEISO_COMPOSITION_CPU_RASTER_INIT_OK);
    CHECK(meiso_composition_cpu_raster_renderer_begin_control_scope(
        fixture->renderer, &fixture->scope
    ) == MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED);
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
    MeisoCompositionCpuRasterRenderer *renderer =
        (MeisoCompositionCpuRasterRenderer *)(uintptr_t)UINTPTR_MAX;
    uint8_t *storage = NULL;
    uint8_t *before = NULL;
    uint8_t *arena;
    size_t storage_size;
    size_t arena_size;
    MeisoCompositionCpuRasterInitStatus expected;
    bool okay = false;

    if (variant == 0U) {
        storage_size = required;
        arena_size = required - 1U;
        expected = MEISO_COMPOSITION_CPU_RASTER_INIT_ARENA_TOO_SMALL;
    } else if (variant == 1U) {
        storage_size = required + alignment;
        arena_size = required;
        expected = MEISO_COMPOSITION_CPU_RASTER_INIT_MISALIGNED_ARENA;
    } else {
        storage_size = required;
        arena_size = required;
        expected = MEISO_COMPOSITION_CPU_RASTER_INIT_INVALID_ARGUMENT;
    }
    storage = (uint8_t *)malloc(storage_size);
    before = (uint8_t *)malloc(storage_size);
    if (storage == NULL || before == NULL) {
        goto cleanup;
    }
    memset(storage, variant == 1U ? 0x5a : 0xa5, storage_size);
    memcpy(before, storage, storage_size);
    arena = variant == 1U ? storage + 1U : storage;
    if (meiso_composition_cpu_raster_renderer_init(
            arena,
            arena_size,
            blob_store,
            variant == 2U ?
                (MeisoCompositionCpuRasterRenderer **)(void *)arena :
                &renderer
        ) != expected) {
        goto cleanup;
    }
    okay = memcmp(storage, before, storage_size) == 0 &&
        (variant == 2U || renderer ==
            (MeisoCompositionCpuRasterRenderer *)(uintptr_t)UINTPTR_MAX);

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
    MeisoCompositionCpuRasterRenderer *renderer = NULL;
    MeisoCompositionCpuRasterSnapshot snapshot;
    uint8_t *arena = (uint8_t *)calloc(1U, required + extra);
    bool okay = false;

    if (arena == NULL) {
        return false;
    }
    if (meiso_composition_cpu_raster_renderer_init(
            arena, required + extra, blob_store, &renderer
        ) == MEISO_COMPOSITION_CPU_RASTER_INIT_OK &&
        meiso_composition_cpu_raster_renderer_snapshot(renderer, &snapshot) &&
        snapshot.target_slots_allocated == 2U &&
        snapshot.target_bytes ==
            MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES &&
        snapshot.renderer_metadata_bytes == required + extra -
            MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES) {
        okay = true;
    }
    free(arena);
    return okay;
}

static bool test_memory_alignment_and_alias_contract(void)
{
    Fixture fixture;
    MeisoCompositionCpuRasterSnapshot snapshot;
    size_t required;
    size_t alignment;

    CHECK(fixture_init(&fixture));
    required = meiso_composition_cpu_raster_renderer_memory_required();
    alignment = meiso_composition_cpu_raster_renderer_alignment_required();
    CHECK(required == fixture.renderer_arena_size && required >
              MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES &&
          alignment != 0U &&
          MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES ==
              UINT64_C(1228800));
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &snapshot
    ));
    CHECK(snapshot.control_scope_bound &&
          control_scope_equal(snapshot.control_scope, fixture.scope) &&
          snapshot.target_slots_allocated ==
              MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT &&
          snapshot.target_bytes ==
              MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES &&
          snapshot.renderer_metadata_bytes ==
              required -
                  MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES &&
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
    MeisoCompositionCpuRasterJobRef first;
    MeisoCompositionCpuRasterJobRef second;
    MeisoCompositionCpuRasterJobRef unchanged;
    MeisoCompositionCpuRasterSnapshot snapshot;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionCpuRasterCompleteView latest;
    MeisoCompositionCpuRasterCompleteView output;
    MeisoCompositionCpuRasterStats stats;
    MeisoByteView old_color;
    MeisoByteView old_depth;
    MeisoByteView inactive_target;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &first
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &second
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(first.job_id != 0U && second.job_id == first.job_id + 1U &&
          first.renderer_generation == second.renderer_generation &&
          control_scope_equal(first.control_scope, fixture.scope));
    memset(&unchanged, 0xa5, sizeof(unchanged));
    {
        const MeisoCompositionCpuRasterJobRef before = unchanged;

        CHECK(meiso_composition_cpu_raster_renderer_submit(
            fixture.renderer, &fixture.scope, &active, 55U, &unchanged
        ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUE_FULL);
        CHECK(memcmp(&before, &unchanged, sizeof(before)) == 0);
    }
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &snapshot
    ));
    CHECK(snapshot.queued_jobs == 2U &&
          job_ref_equal(snapshot.head_job, first) &&
          !snapshot.complete_available);

    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCompositionCpuRasterCompleteView before = output;

        CHECK(meiso_composition_cpu_raster_renderer_render_next(
            fixture.renderer, &second, 1000U, 1010U, &output
        ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_JOB_MISMATCH);
        CHECK(memcmp(&before, &output, sizeof(before)) == 0);
    }
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCompositionCpuRasterCompleteView before = output;

        CHECK(meiso_composition_cpu_raster_renderer_render_next(
            fixture.renderer, &first, 1010U, 1000U, &output
        ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_TIME);
        CHECK(memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE);
    CHECK(job_ref_equal(complete.job, first) &&
          complete.result.evidence_flags ==
              MEISO_COMPOSITION_CPU_RASTER_RENDERER_EVIDENCE_FLAGS &&
          complete.result.evidence_flags == UINT32_C(0x00000003) &&
          complete.color_rgba8888.size ==
              MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE &&
          complete.depth_implementation_32.size ==
              MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE &&
          meiso_composition_cpu_raster_renderer_latest_complete(
              fixture.renderer, &latest
          ) && job_ref_equal(latest.job, first) &&
          latest.target_slot == complete.target_slot &&
          latest.color_rgba8888.data == complete.color_rgba8888.data &&
          latest.depth_implementation_32.data ==
              complete.depth_implementation_32.data);
    old_color = complete.color_rgba8888;
    old_depth = complete.depth_implementation_32;
    inactive_target.data = complete.target_slot == 0U ?
        complete.color_rgba8888.data +
            MEISO_COMPOSITION_CPU_RASTER_TARGET_SET_BYTE_SIZE :
        complete.color_rgba8888.data -
            MEISO_COMPOSITION_CPU_RASTER_TARGET_SET_BYTE_SIZE;
    inactive_target.size =
        MEISO_COMPOSITION_CPU_RASTER_TARGET_SET_BYTE_SIZE;
    CHECK(bytes_are_zero(inactive_target));
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[0]));

    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCompositionCpuRasterCompleteView before = output;

        CHECK(meiso_composition_cpu_raster_renderer_render_next(
            fixture.renderer, &second, 1020U, 1010U, &output
        ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_TIME);
        CHECK(memcmp(&before, &output, sizeof(before)) == 0 &&
              (!source_host_golden_enabled() ||
               (sha256_matches(
                    old_color, RENDER_GOLDENS[0].color_sha256
                ) && sha256_matches(
                    old_depth, RENDER_GOLDENS[0].depth_sha256
                ))) && bytes_are_zero(inactive_target) &&
              meiso_composition_cpu_raster_renderer_latest_complete(
                  fixture.renderer, &latest
              ) && job_ref_equal(latest.job, first));
    }
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &second, 1020U, 1030U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE);
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &snapshot
    ));
    CHECK(meiso_composition_cpu_raster_renderer_stats(
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
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionCpuRasterCompleteView repeated;
    MeisoCompositionCpuRasterStats baseline_stats;
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
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &baseline_stats
    ) && baseline_stats.triangles_submitted == 12U &&
          baseline_stats.triangles_clipped == 0U &&
          baseline_stats.triangles_culled == 10U &&
          baseline_stats.triangles_rasterized == 2U &&
          baseline_stats.fragments_depth_passed == 7056U);
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
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired_world
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(!meiso_composition_cpu_raster_renderer_latest_complete(
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
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired_world
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
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
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired_world
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
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
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionCpuRasterSnapshot renderer_snapshot;
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
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
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
              (!source_host_golden_enabled() ||
               memcmp(decoded.full_target_sha256,
                      RENDER_GOLDENS[0].color_sha256,
                      MEISO_SHA256_DIGEST_SIZE) == 0));
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
    MeisoCompositionCpuRasterRetireStatus expected
)
{
    uint8_t *before =
        (uint8_t *)malloc(fixture->renderer_arena_size);
    MeisoCompositionCpuRasterRetireStatus status;
    bool okay;

    if (before == NULL) {
        return false;
    }
    memcpy(before, fixture->renderer_arena, fixture->renderer_arena_size);
    status = meiso_composition_cpu_raster_renderer_retire_world(
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
    MeisoCompositionCpuRasterJobRef old_pending;
    MeisoCompositionCpuRasterJobRef new_pending;
    MeisoCompositionCpuRasterJobRef stale_old_pending;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionCpuRasterCompleteView latest;
    MeisoCompositionCpuRasterSnapshot snapshot;
    MeisoCompositionCpuRasterStats stats;
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
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &old_active, 55U, &old_pending
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);

    desired.objects[0].transform.position[0] = 1.0F;
    CHECK(activate_scene(&fixture, &desired, 2U, 71U, &new_active));
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &new_active, 55U, &new_pending
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    wrong_scope = fixture.scope;
    ++wrong_scope.control_scope_generation;
    CHECK(check_retire_zero_diff(
        &fixture,
        &wrong_scope,
        &old_active.ref,
        MEISO_COMPOSITION_CPU_RASTER_RETIRE_SCOPE_MISMATCH
    ));
    CHECK(check_retire_zero_diff(
        &fixture,
        (const MeisoCompositionSceneControlScope *)fixture.renderer_arena,
        &old_active.ref,
        MEISO_COMPOSITION_CPU_RASTER_RETIRE_INVALID_ARGUMENT
    ));

    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &old_active.ref
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &snapshot
    ) && snapshot.queued_jobs == 1U &&
          job_ref_equal(snapshot.head_job, new_pending) &&
          !snapshot.complete_available && bytes_are_zero(retired_color) &&
          bytes_are_zero(retired_depth));
    memset(&complete, 0xa5, sizeof(complete));
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &old_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_JOB_MISMATCH);

    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &new_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE);
    CHECK(verify_render_golden(&complete, &RENDER_GOLDENS[1]));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &old_active.ref
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED);
    CHECK(meiso_composition_cpu_raster_renderer_latest_complete(
        fixture.renderer, &latest
    ) && world_ref_equal(latest.job.world, new_active.ref) &&
          verify_render_golden(&latest, &RENDER_GOLDENS[1]));
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &new_active, 55U, &new_pending
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &stale_old_view, 55U,
        &stale_old_pending
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &old_active.ref
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &snapshot
    ) && snapshot.queued_jobs == 1U &&
          job_ref_equal(snapshot.head_job, new_pending) &&
          snapshot.complete_available &&
          world_ref_equal(snapshot.latest_complete_job.world,
                          new_active.ref));
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &stale_old_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_JOB_MISMATCH);
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &new_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE);
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &new_active, 55U, &new_pending
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);

    desired.object_count = 0U;
    memset(desired.objects, 0, sizeof(desired.objects));
    CHECK(activate_scene(&fixture, &desired, 3U, 71U, &deleted_active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &new_active.ref
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &snapshot
    ) && snapshot.queued_jobs == 0U && !snapshot.complete_available);
    memset(&complete, 0xa5, sizeof(complete));
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &new_pending, 1000U, 1010U, &complete
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB);
    CHECK(meiso_composition_scene_store_disable(
        fixture.scene_store, &deleted_active.ref
    ) == MEISO_COMPOSITION_SCENE_DISABLE_DISABLED);
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &deleted_active.ref
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED);
    CHECK(!meiso_composition_cpu_raster_renderer_latest_complete(
        fixture.renderer, &complete
    ));
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ) && stats.world_retires == 3U && stats.jobs_retired == 3U &&
          stats.completes_retired == 2U);

    fixture_destroy(&fixture);
    return true;
}

static bool check_public_aliases_zero_diff(
    Fixture *fixture,
    const MeisoCompositionSceneWorldView *active,
    MeisoCompositionCpuRasterJobRef *job
)
{
    MeisoCompositionSceneControlScope scope_before;
    MeisoCompositionCpuRasterJobRef job_before;
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
    if (meiso_composition_cpu_raster_renderer_begin_control_scope(
            fixture->renderer,
            (const MeisoCompositionSceneControlScope *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_CPU_RASTER_SCOPE_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    SAVE_RENDERER();
    if (meiso_composition_cpu_raster_renderer_submit(
            fixture->renderer,
            &fixture->scope,
            active,
            55U,
            (MeisoCompositionCpuRasterJobRef *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    scope_before = fixture->scope;
    SAVE_RENDERER();
    if (meiso_composition_cpu_raster_renderer_submit(
            fixture->renderer,
            &fixture->scope,
            active,
            55U,
            (MeisoCompositionCpuRasterJobRef *)(void *)
                &fixture->scope
        ) != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED() ||
        memcmp(&scope_before, &fixture->scope, sizeof(scope_before)) != 0) {
        goto cleanup;
    }
    SAVE_RENDERER();
    if (meiso_composition_cpu_raster_renderer_render_next(
            fixture->renderer,
            job,
            1000U,
            1010U,
            (MeisoCompositionCpuRasterCompleteView *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED()) {
        goto cleanup;
    }
    job_before = *job;
    SAVE_RENDERER();
    if (meiso_composition_cpu_raster_renderer_render_next(
            fixture->renderer,
            job,
            1000U,
            1010U,
            (MeisoCompositionCpuRasterCompleteView *)(void *)job
        ) != MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_ARGUMENT ||
        !RENDERER_UNCHANGED() ||
        memcmp(&job_before, job, sizeof(job_before)) != 0) {
        goto cleanup;
    }
    SAVE_RENDERER();
    if (meiso_composition_cpu_raster_renderer_discard_head(
            fixture->renderer,
            (const MeisoCompositionCpuRasterJobRef *)
                fixture->renderer_arena
        ) != MEISO_COMPOSITION_CPU_RASTER_DISCARD_INVALID_ARGUMENT ||
        meiso_composition_cpu_raster_renderer_snapshot(
            fixture->renderer,
            (MeisoCompositionCpuRasterSnapshot *)
                fixture->renderer_arena
        ) ||
        meiso_composition_cpu_raster_renderer_stats(
            fixture->renderer,
            (MeisoCompositionCpuRasterStats *)
                fixture->renderer_arena
        ) ||
        meiso_composition_cpu_raster_renderer_latest_complete(
            fixture->renderer,
            (MeisoCompositionCpuRasterCompleteView *)
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
    MeisoCompositionCpuRasterJobRef job;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));

    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &job
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(check_public_aliases_zero_diff(&fixture, &active, &job));

    CHECK(meiso_composition_cpu_raster_renderer_discard_head(
        fixture.renderer, &job
    ) == MEISO_COMPOSITION_CPU_RASTER_DISCARD_DISCARDED);
    fixture_destroy(&fixture);
    return true;
}

static bool test_scope_reset_discard_and_disable_no_result(void)
{
    Fixture fixture;
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldRef active_ref;
    MeisoCompositionCpuRasterJobRef first;
    MeisoCompositionCpuRasterJobRef second;
    MeisoCompositionCpuRasterCompleteView output;
    MeisoCompositionCpuRasterSnapshot before;
    MeisoCompositionCpuRasterSnapshot after;
    MeisoCompositionSceneControlScope old_scope;
    MeisoCompositionSceneControlScope new_scope;

    CHECK(fixture_init(&fixture));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 71U, &active));
    active_ref = active.ref;
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &first
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &second
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &before
    ));
    CHECK(meiso_composition_cpu_raster_renderer_discard_head(
        fixture.renderer, &second
    ) == MEISO_COMPOSITION_CPU_RASTER_DISCARD_JOB_MISMATCH);
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &after
    ) && after.queued_jobs == before.queued_jobs &&
          job_ref_equal(after.head_job, before.head_job));
    CHECK(meiso_composition_cpu_raster_renderer_discard_head(
        fixture.renderer, &first
    ) == MEISO_COMPOSITION_CPU_RASTER_DISCARD_DISCARDED);
    CHECK(meiso_composition_cpu_raster_renderer_discard_head(
        fixture.renderer, &second
    ) == MEISO_COMPOSITION_CPU_RASTER_DISCARD_DISCARDED);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &output
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB);
    CHECK(!meiso_composition_cpu_raster_renderer_latest_complete(
        fixture.renderer, &output
    ));

    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 55U, &first
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    old_scope = fixture.scope;
    CHECK(meiso_composition_cpu_raster_renderer_begin_control_scope(
        fixture.renderer, &old_scope
    ) == MEISO_COMPOSITION_CPU_RASTER_SCOPE_UNCHANGED);
    CHECK(meiso_composition_scene_store_reset_control_scope(
        fixture.scene_store, fixture.host_owner.incarnation
    ) == MEISO_COMPOSITION_SCENE_OWNER_SCOPE_RESET);
    CHECK(meiso_composition_scene_store_control_scope(
        fixture.scene_store, &new_scope
    ) && !control_scope_equal(old_scope, new_scope));
    CHECK(meiso_composition_cpu_raster_renderer_begin_control_scope(
        fixture.renderer, &new_scope
    ) == MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED);
    fixture.scope = new_scope;
    CHECK(meiso_composition_cpu_raster_renderer_snapshot(
        fixture.renderer, &after
    ) && after.queued_jobs == 0U && !after.complete_available &&
          control_scope_equal(after.control_scope, new_scope));
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &output
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB);
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &old_scope, &active, 55U, &second
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_SCOPE_MISMATCH);

    CHECK(publish_cube(&fixture, true));
    desired = scene_value(fixture.cube_id);
    CHECK(activate_scene(&fixture, &desired, 2U, 72U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_submit(
        fixture.renderer, &fixture.scope, &active, 56U, &first
    ) == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED);
    CHECK(meiso_composition_scene_store_disable(
        fixture.scene_store, &active.ref
    ) == MEISO_COMPOSITION_SCENE_DISABLE_DISABLED);
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &active.ref
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_composition_cpu_raster_renderer_render_next(
        fixture.renderer, &first, 1000U, 1010U, &output
    ) == MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB);
    CHECK(!meiso_composition_scene_store_active(
        fixture.scene_store, &active
    ));
    CHECK(active_ref.world_generation != 0U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_clip_cull_color_and_clear_rules(void)
{
    const uint32_t front_indices[3] = {0U, 1U, 2U};
    const uint32_t back_indices[3] = {0U, 2U, 1U};
    Fixture fixture;
    MeisoCompositionSceneMeshVertex vertices[3];
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldRef retired;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionCpuRasterStats stats;
    MeisoContentId mesh_id;
    MeisoByteView mesh_bytes;
    uint8_t storage[512];
    const uint32_t center_x = MEISO_COMPOSITION_SCENE_TARGET_WIDTH / 2U;
    const uint32_t center_y = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT / 2U;
    const uint8_t *pixel;

    CHECK(fixture_init(&fixture));
    set_vertex(&vertices[0], -1.0F, -0.75F, 0.0F, 255U, 0U, 0U);
    set_vertex(&vertices[1], 1.0F, -0.75F, 0.0F, 0U, 255U, 0U);
    set_vertex(&vertices[2], 0.0F, 1.0F, 0.0F, 0U, 0U, 255U);
    CHECK(encode_mesh(
        vertices, 3U, front_indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &mesh_id
    ) && publish_mesh(&fixture, mesh_id, mesh_bytes));
    desired = scene_value(mesh_id);
    CHECK(activate_scene(&fixture, &desired, 1U, 91U, &active));
    CHECK(render_active(&fixture, &active, &complete));
    pixel = color_pixel(&complete, center_x, center_y);
    CHECK(!is_clear_pixel(pixel) && pixel[3] == UINT8_C(0xff) &&
          pixel[0] != 0U && pixel[1] != 0U && pixel[2] != 0U &&
          is_clear_pixel(color_pixel(&complete, 0U, 0U)) &&
          depth_pixel(&complete, center_x, center_y) !=
              MEISO_COMPOSITION_CPU_RASTER_CLEAR_DEPTH &&
          depth_pixel(&complete, 0U, 0U) ==
              MEISO_COMPOSITION_CPU_RASTER_CLEAR_DEPTH &&
          verify_thumbnail_samples(&complete) &&
          verify_result_codec(&complete.result));
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ) && stats.triangles_submitted == 1U &&
          stats.triangles_clipped == 0U && stats.triangles_culled == 0U &&
          stats.triangles_rasterized == 1U &&
          stats.fragments_depth_passed != 0U);

    retired = active.ref;
    CHECK(encode_mesh(
        vertices, 3U, back_indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &mesh_id
    ) && publish_mesh(&fixture, mesh_id, mesh_bytes));
    desired = scene_value(mesh_id);
    CHECK(activate_scene(&fixture, &desired, 2U, 91U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(is_clear_pixel(color_pixel(&complete, center_x, center_y)) &&
          depth_pixel(&complete, center_x, center_y) ==
              MEISO_COMPOSITION_CPU_RASTER_CLEAR_DEPTH);
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ) && stats.triangles_submitted == 2U &&
          stats.triangles_culled == 1U &&
          stats.triangles_rasterized == 1U);

    retired = active.ref;
    set_vertex(&vertices[0], -0.5F, -0.5F, 2.0F, 255U, 0U, 0U);
    set_vertex(&vertices[1], 0.5F, -0.5F, 2.0F, 0U, 255U, 0U);
    set_vertex(&vertices[2], 0.0F, 0.3F, 2.95F, 0U, 0U, 255U);
    CHECK(encode_mesh(
        vertices, 3U, front_indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &mesh_id
    ) && publish_mesh(&fixture, mesh_id, mesh_bytes));
    desired = scene_value(mesh_id);
    CHECK(activate_scene(&fixture, &desired, 3U, 91U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(!is_clear_pixel(color_pixel(&complete, center_x, center_y)) &&
          depth_pixel(&complete, center_x, center_y) !=
              MEISO_COMPOSITION_CPU_RASTER_CLEAR_DEPTH);
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ) && stats.triangles_submitted == 3U &&
          stats.triangles_clipped == 1U &&
          stats.triangles_rasterized > 1U);

    retired = active.ref;
    set_vertex(&vertices[0], 10.0F, -0.5F, 0.0F, 255U, 0U, 0U);
    set_vertex(&vertices[1], 11.0F, -0.5F, 0.0F, 0U, 255U, 0U);
    set_vertex(&vertices[2], 10.0F, 0.5F, 0.0F, 0U, 0U, 255U);
    CHECK(encode_mesh(
        vertices, 3U, front_indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &mesh_id
    ) && publish_mesh(&fixture, mesh_id, mesh_bytes));
    desired = scene_value(mesh_id);
    CHECK(activate_scene(&fixture, &desired, 4U, 91U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(is_clear_pixel(color_pixel(&complete, center_x, center_y)) &&
          depth_pixel(&complete, center_x, center_y) ==
              MEISO_COMPOSITION_CPU_RASTER_CLEAR_DEPTH);
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ) && stats.triangles_submitted == 4U &&
          stats.triangles_clipped == 2U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_strict_depth_and_equal_tie(void)
{
    const uint32_t indices[3] = {0U, 1U, 2U};
    Fixture fixture;
    MeisoCompositionSceneMeshVertex vertices[3];
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldRef retired;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionCpuRasterStats stats;
    MeisoContentId red_id;
    MeisoContentId green_near_id;
    MeisoContentId green_equal_id;
    MeisoByteView mesh_bytes;
    uint8_t storage[512];
    uint8_t first_color_sha[MEISO_SHA256_DIGEST_SIZE];
    uint8_t first_depth[4];
    uint64_t first_fragments;
    uint64_t second_fragments;
    const uint32_t center_x = MEISO_COMPOSITION_SCENE_TARGET_WIDTH / 2U;
    const uint32_t center_y = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT / 2U;
    const uint8_t *pixel;

    CHECK(fixture_init(&fixture));
    set_vertex(&vertices[0], -0.8F, -0.7F, 0.0F, 255U, 0U, 0U);
    set_vertex(&vertices[1], 0.8F, -0.7F, 0.0F, 255U, 0U, 0U);
    set_vertex(&vertices[2], 0.0F, 0.8F, 0.0F, 255U, 0U, 0U);
    CHECK(encode_mesh(
        vertices, 3U, indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &red_id
    ) && publish_mesh(&fixture, red_id, mesh_bytes));
    set_vertex(&vertices[0], -0.8F, -0.7F, 0.5F, 0U, 255U, 0U);
    set_vertex(&vertices[1], 0.8F, -0.7F, 0.5F, 0U, 255U, 0U);
    set_vertex(&vertices[2], 0.0F, 0.8F, 0.5F, 0U, 255U, 0U);
    CHECK(encode_mesh(
        vertices, 3U, indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &green_near_id
    ) && publish_mesh(&fixture, green_near_id, mesh_bytes));
    set_vertex(&vertices[0], -0.8F, -0.7F, 0.0F, 0U, 255U, 0U);
    set_vertex(&vertices[1], 0.8F, -0.7F, 0.0F, 0U, 255U, 0U);
    set_vertex(&vertices[2], 0.0F, 0.8F, 0.0F, 0U, 255U, 0U);
    CHECK(encode_mesh(
        vertices, 3U, indices, 3U, true,
        storage, sizeof(storage), &mesh_bytes, &green_equal_id
    ) && publish_mesh(&fixture, green_equal_id, mesh_bytes));

    desired = scene_value(red_id);
    desired.object_count = 2U;
    desired.objects[1] = desired.objects[0];
    desired.objects[1].id = (MeisoCompositionSceneObjectId){1U, 2U};
    desired.objects[1].mesh_content_id = green_near_id;
    CHECK(activate_scene(&fixture, &desired, 1U, 92U, &active));
    CHECK(render_active(&fixture, &active, &complete));
    pixel = color_pixel(&complete, center_x, center_y);
    CHECK(pixel[0] == 0U && pixel[1] == UINT8_C(0xff) && pixel[2] == 0U);
    memcpy(first_color_sha, complete.result.full_target_sha256,
           sizeof(first_color_sha));
    {
        const uint8_t *depth = complete.depth_implementation_32.data +
            ((size_t)center_y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
             center_x) * 4U;
        memcpy(first_depth, depth, sizeof(first_depth));
    }
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ));
    first_fragments = stats.fragments_depth_passed;

    retired = active.ref;
    desired.objects[0].mesh_content_id = green_near_id;
    desired.objects[1].mesh_content_id = red_id;
    CHECK(activate_scene(&fixture, &desired, 2U, 92U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    CHECK(memcmp(first_color_sha, complete.result.full_target_sha256,
                 sizeof(first_color_sha)) == 0);
    {
        const uint8_t *depth = complete.depth_implementation_32.data +
            ((size_t)center_y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
             center_x) * 4U;
        CHECK(memcmp(first_depth, depth, sizeof(first_depth)) == 0);
    }
    CHECK(meiso_composition_cpu_raster_renderer_stats(
        fixture.renderer, &stats
    ));
    second_fragments = stats.fragments_depth_passed - first_fragments;
    CHECK(second_fragments < first_fragments);

    retired = active.ref;
    desired.objects[0].mesh_content_id = red_id;
    desired.objects[1].mesh_content_id = green_equal_id;
    CHECK(activate_scene(&fixture, &desired, 3U, 92U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    pixel = color_pixel(&complete, center_x, center_y);
    CHECK(pixel[0] == UINT8_C(0xff) && pixel[1] == 0U && pixel[2] == 0U);

    retired = active.ref;
    desired.objects[0].mesh_content_id = green_equal_id;
    desired.objects[1].mesh_content_id = red_id;
    CHECK(activate_scene(&fixture, &desired, 4U, 92U, &active));
    CHECK(meiso_composition_cpu_raster_renderer_retire_world(
        fixture.renderer, &fixture.scope, &retired
    ) == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED);
    CHECK(render_active(&fixture, &active, &complete));
    pixel = color_pixel(&complete, center_x, center_y);
    CHECK(pixel[0] == 0U && pixel[1] == UINT8_C(0xff) && pixel[2] == 0U);
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
        !test_scope_reset_discard_and_disable_no_result() ||
        !test_clip_cull_color_and_clear_rules() ||
        !test_strict_depth_and_equal_tie()) {
        return 1;
    }
    (void)puts("composition cpu raster renderer tests passed");
    return 0;
}
