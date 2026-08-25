#define _POSIX_C_SOURCE 200809L

#include "composition_cpu_raster_renderer.h"
#include "meiso_composition_edge_simulator.h"
#include "scene_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "check failed at line %d\n", __LINE__);          \
            goto fail;                                                         \
        }                                                                      \
    } while (0)

static bool bytes_all(const void *value, size_t size, uint8_t byte)
{
    const uint8_t *bytes = value;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != byte) {
            return false;
        }
    }
    return true;
}

static MeisoCompositionSessionV3CatalogConfig catalog_config(void)
{
    MeisoCompositionSessionV3CatalogConfig value;

    memset(&value, 0, sizeof(value));
    value.wifi_incarnation = 11U;
    value.presentation_incarnation = 12U;
    value.scene_incarnation = 13U;
    value.device_telemetry_incarnation = 14U;
    value.render_media_incarnation = 15U;
    value.camera_media_incarnation = 16U;
    value.wifi_capability = (MeisoWifiTelemetryCapability){
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE, 100U, 1000U, 100U
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 320U, 240U, 10000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 320U, 240U, 30000U,
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static MeisoCompositionEdgeSimulatorConfig simulator_config(
    uint64_t continuity_serial,
    uint32_t blob_slots
)
{
    MeisoCompositionEdgeSimulatorConfig value;

    memset(&value, 0, sizeof(value));
    value.catalog_generation = 1U;
    value.catalog = catalog_config();
    value.continuity_serial = continuity_serial;
    value.desired_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    value.wifi_production_epoch = 77U;
    value.render_media_stream_epoch = 501U;
    value.camera_media_stream_epoch = 602U;
    value.wifi_sample.selected_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE;
    value.wifi_sample.present_fields = value.wifi_sample.selected_fields;
    value.wifi_sample.link_state = true;
    value.composition_enabled = true;
    value.composition_blob_slot_count = blob_slots;
    value.composition_clock_epoch = 700U;
    value.presentation_production_epoch = 1001U;
    value.scene_production_epoch = 2001U;
    return value;
}

static bool cube_wire(
    uint8_t *buffer,
    size_t capacity,
    size_t *out_size,
    MeisoContentId *out_id
)
{
    return buffer != NULL && out_size != NULL && out_id != NULL &&
        meiso_composition_scene_mesh_encode(
            meiso_composition_scene_canonical_cube(),
            (MeisoMutableBuffer){buffer, capacity}, out_size
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
        meiso_composition_scene_mesh_content_id(
            (MeisoByteView){buffer, *out_size}, out_id
        );
}

static bool triangle_wire(
    uint8_t color_bias,
    uint8_t *buffer,
    size_t capacity,
    size_t *out_size,
    MeisoContentId *out_id
)
{
    MeisoCompositionSceneMeshVertex vertices[3];
    const uint32_t indices[3] = {0U, 1U, 2U};
    MeisoCompositionSceneMesh mesh;

    memset(vertices, 0, sizeof(vertices));
    vertices[0].position[0] = -0.5F;
    vertices[0].position[1] = -0.5F;
    vertices[1].position[0] = 0.5F;
    vertices[1].position[1] = -0.5F;
    vertices[2].position[1] = 0.5F;
    vertices[0].color_rgba[0] = (uint8_t)(17U + color_bias);
    vertices[0].color_rgba[3] = UINT8_MAX;
    vertices[1].color_rgba[1] = (uint8_t)(33U + color_bias);
    vertices[1].color_rgba[3] = UINT8_MAX;
    vertices[2].color_rgba[2] = (uint8_t)(65U + color_bias);
    vertices[2].color_rgba[3] = UINT8_MAX;
    mesh = (MeisoCompositionSceneMesh){
        true, 3U, vertices, 3U, indices
    };
    return buffer != NULL && out_size != NULL && out_id != NULL &&
        meiso_composition_scene_mesh_encode(
            &mesh, (MeisoMutableBuffer){buffer, capacity}, out_size
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
        meiso_composition_scene_mesh_content_id(
            (MeisoByteView){buffer, *out_size}, out_id
        );
}

static MeisoCompositionSceneDesired scene_value(
    MeisoContentId mesh_id,
    uint64_t clock_epoch,
    uint64_t time_ns,
    float x,
    bool with_object
)
{
    MeisoCompositionSceneDesired value;

    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = 9U;
    value.camera.source_time = (MeisoTimePoint){clock_epoch, time_ns};
    value.camera.position[2] = 6.0F;
    value.camera.orientation_xyzw[3] = 1.0F;
    value.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    value.camera.vertical_fov_degrees = 55.0F;
    value.camera.near_z = 0.1F;
    value.camera.far_z = 100.0F;
    value.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    if (with_object) {
        value.object_count = 1U;
        value.objects[0].id = (MeisoCompositionSceneObjectId){1U, 1U};
        value.objects[0].mesh_content_id = mesh_id;
        value.objects[0].space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
        value.objects[0].space_epoch = value.camera.space_epoch;
        value.objects[0].transform.position[0] = x;
        value.objects[0].transform.orientation_xyzw[3] = 1.0F;
        value.objects[0].transform.scale[0] = 1.0F;
        value.objects[0].transform.scale[1] = 1.0F;
        value.objects[0].transform.scale[2] = 1.0F;
        value.objects[0].flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
    }
    return value;
}

static MeisoCompositionEdgePresentationSettings presentation_settings(void)
{
    return (MeisoCompositionEdgePresentationSettings){
        0U,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_COLOR_RGBA8888,
        MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32,
        MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ,
        1000U
    };
}

static bool test_cpu_raster_preview(void)
{
    MeisoCompositionEdgeSimulatorConfig config = simulator_config(9201U, 2U);
    MeisoCompositionEdgeSimulatorConfig base = config;
    MeisoCompositionEdgeSimulatorConfig invalid = config;
    MeisoEdgeHandle *handle = NULL;
    MeisoEdgeCommandReceipt receipt;
    MeisoCompositionEdgeSimulatorTransition transition;
    MeisoCompositionEdgeSnapshot snapshot;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionEdgePresentationSettings settings =
        presentation_settings();
    MeisoCompositionEdgeSimulatorFrame external_frame;
    MeisoCompositionMediaDesired render_desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    MeisoEdgeMediaFrameView frame;
    MeisoEdgeMediaFrameKey old_frame_key;
    MeisoContentId cube_id;
    MeisoContentId frame_id;
    MeisoContentId next_frame_id;
    uint8_t cube[1024];
    uint8_t external_byte = 0U;
    size_t cube_size = 0U;
    size_t required;
    size_t base_required;
    const size_t alignment =
        meiso_composition_edge_simulator_alignment_required();
    void *arena = NULL;

    config.cpu_raster_preview_enabled = true;
    base.cpu_raster_preview_enabled = false;
    invalid.composition_enabled = false;
    invalid.composition_blob_slot_count = 0U;
    invalid.composition_clock_epoch = 0U;
    invalid.presentation_production_epoch = 0U;
    invalid.scene_production_epoch = 0U;
    invalid.cpu_raster_preview_enabled = true;
    required = meiso_composition_edge_simulator_memory_required(&config);
    base_required = meiso_composition_edge_simulator_memory_required(&base);
    CHECK(required > base_required && base_required != 0U &&
          meiso_composition_edge_simulator_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.catalog.render_media_capability.pixel_formats |=
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGB565;
    CHECK(cube_wire(cube, sizeof(cube), &cube_size, &cube_id));
    CHECK(posix_memalign(&arena, alignment, required) == 0);
    memset(arena, 0xa5, required);
    handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              arena, required, &invalid, &handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG &&
          bytes_all(arena, required, UINT8_C(0xa5)) &&
          handle ==
              (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    handle = NULL;
    memset(arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              arena, required, &config, &handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    scene = scene_value(cube_id, 700U, 100U, 0.0F, true);
    CHECK(meiso_composition_edge_put_mesh(
              handle, (MeisoByteView){cube, cube_size}, &receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_scene(
              handle, true, &scene, &receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_presentation(
              handle, true, &settings, &receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_render_media(
              handle, true, &render_desired, &receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_activate(handle) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1000U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1100U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1200U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1300U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_ACTIVATED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1400U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.render_result_present &&
          snapshot.render_result.evidence_flags ==
              MEISO_COMPOSITION_CPU_RASTER_RENDERER_EVIDENCE_FLAGS &&
          snapshot.render_result.object_count == 1U &&
          snapshot.render_result.unique_mesh_count == 1U &&
          snapshot.scene_status.target_slots_used ==
              MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT &&
          snapshot.scene_status.target_bytes ==
              MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES &&
          snapshot.scene_status.render_jobs_in_flight == 0U &&
          !snapshot.media[0].latest_present);
    memset(&frame_id, 0, sizeof(frame_id));
    frame_id.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    frame_id.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memmove(frame_id.digest, snapshot.render_result.full_target_sha256,
            MEISO_SHA256_DIGEST_SIZE);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1500U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED);
    memset(&external_frame, 0, sizeof(external_frame));
    external_frame.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    external_frame.source_time = (MeisoTimePoint){700U, 1500U};
    external_frame.access_unit = (MeisoByteView){&external_byte, 1U};
    CHECK(meiso_composition_edge_simulator_publish_frame(
              handle, &external_frame
          ) == MEISO_EDGE_CONFLICT);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1600U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_PREVIEW_PUBLISHED);
    CHECK(meiso_composition_edge_media_latest(
              handle, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW, &frame
          ) == MEISO_EDGE_OK &&
          frame.access_unit.size ==
              MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES &&
          frame.pixel_format == MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888 &&
          frame.codec == MEISO_COMPOSITION_MEDIA_CODEC_RAW &&
          frame.width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
          frame.height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
          frame.applied_scene_state_version == 1U &&
          frame.active_plan_generation == 1U &&
          frame.world_generation == 1U &&
          meiso_runtime_blob_sha256_verify(
              &frame_id, frame.access_unit, NULL
          ));
    old_frame_key = frame.key;
    scene = scene_value(cube_id, 700U, 200U, 1.0F, true);
    CHECK(meiso_composition_edge_set_scene(
              handle, true, &scene, &receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1700U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1800U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.render_result_present &&
          snapshot.render_result.object_count == 1U &&
          snapshot.scene_status.world_generation == 2U);
    memset(&next_frame_id, 0, sizeof(next_frame_id));
    next_frame_id.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    next_frame_id.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memmove(next_frame_id.digest,
            snapshot.render_result.full_target_sha256,
            MEISO_SHA256_DIGEST_SIZE);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1900U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE &&
          meiso_runtime_blob_sha256_verify(
              &frame_id, frame.access_unit, NULL
          ));
    CHECK(meiso_composition_edge_media_release(handle, &old_frame_key) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 2000U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_PREVIEW_PUBLISHED);
    CHECK(meiso_composition_edge_media_latest(
              handle, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW, &frame
          ) == MEISO_EDGE_OK &&
          frame.key.frame_sequence == 2U &&
          frame.applied_scene_state_version == 2U &&
          frame.active_plan_generation == 1U &&
          frame.world_generation == 2U &&
          meiso_runtime_blob_sha256_verify(
              &next_frame_id, frame.access_unit, NULL
          ));
    CHECK(meiso_composition_edge_media_release(handle, &frame.key) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_terminate(
              handle, MEISO_EDGE_TERMINAL_LOCAL_ABORT
          ) == MEISO_EDGE_OK);
    free(arena);
    return true;

fail:
    free(arena);
    return false;
}

int main(void)
{
    MeisoCompositionEdgeSimulatorConfig config = simulator_config(9101U, 2U);
    MeisoCompositionEdgeSimulatorConfig exhausted_config =
        simulator_config(9102U, 2U);
    MeisoCompositionEdgeSimulatorConfig disabled = config;
    MeisoCompositionEdgeSimulatorConfig invalid;
    MeisoEdgeHandle *handle = NULL;
    MeisoEdgeHandle *exhausted_handle = NULL;
    MeisoEdgeHandle *unchanged_handle;
    MeisoEdgeCommandReceipt mesh_receipt;
    MeisoEdgeCommandReceipt scene_receipt;
    MeisoEdgeCommandReceipt replacement_receipt;
    MeisoEdgeCommandReceipt presentation_receipt;
    MeisoEdgeCommandReceipt media_receipt;
    MeisoEdgeCommandReceipt unchanged_receipt;
    MeisoCompositionEdgeSnapshot snapshot;
    MeisoCompositionEdgeSnapshot before;
    MeisoCompositionEdgeSimulatorTransition transition;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionSceneDesired moved;
    MeisoCompositionSceneDesired moved_again;
    MeisoCompositionSceneDesired missing;
    MeisoCompositionSceneDesired empty;
    MeisoCompositionMediaDesired render_desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    MeisoCompositionEdgeSimulatorFrame frame;
    MeisoEdgeMediaFrameView frame_view;
    MeisoCompositionEdgePresentationSettings settings =
        presentation_settings();
    uint8_t cube[1024];
    uint8_t triangle[512];
    uint8_t triangle_two[512];
    uint8_t first_hash[MEISO_SHA256_DIGEST_SIZE];
    MeisoContentId cube_id;
    MeisoContentId triangle_id;
    MeisoContentId triangle_two_id;
    size_t cube_size = 0U;
    size_t triangle_size = 0U;
    size_t triangle_two_size = 0U;
    size_t required;
    size_t disabled_required;
    const size_t alignment =
        meiso_composition_edge_simulator_alignment_required();
    uint8_t *arena = NULL;
    uint8_t *short_arena = NULL;
    uint8_t *exhausted_arena = NULL;
    uint8_t *arena_copy = NULL;
    uint8_t *render_bytes = NULL;
    bool supported = false;

    CHECK(test_cpu_raster_preview());
    disabled.composition_enabled = false;
    disabled.composition_blob_slot_count = 0U;
    disabled.composition_clock_epoch = 0U;
    disabled.presentation_production_epoch = 0U;
    disabled.scene_production_epoch = 0U;
    required = meiso_composition_edge_simulator_memory_required(&config);
    disabled_required =
        meiso_composition_edge_simulator_memory_required(&disabled);
    CHECK(required > disabled_required && disabled_required != 0U &&
          alignment != 0U);
    invalid = disabled;
    invalid.composition_clock_epoch = 1U;
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    unchanged_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required, &invalid, &unchanged_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG &&
          bytes_all(short_arena, required, UINT8_C(0xa5)) &&
          unchanged_handle ==
              (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;
    invalid = config;
    invalid.composition_blob_slot_count = 0U;
    CHECK(meiso_composition_edge_simulator_memory_required(&invalid) == 0U);
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    unchanged_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required, &invalid, &unchanged_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG &&
          bytes_all(short_arena, required, UINT8_C(0xa5)) &&
          unchanged_handle ==
              (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required - 1U, &config, &unchanged_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_ARENA_TOO_SMALL &&
          bytes_all(short_arena, required, UINT8_C(0xa5)));
    free(short_arena);
    short_arena = NULL;

    CHECK(cube_wire(cube, sizeof(cube), &cube_size, &cube_id) &&
          triangle_wire(
              0U, triangle, sizeof(triangle), &triangle_size, &triangle_id
          ) &&
          triangle_wire(
              1U, triangle_two, sizeof(triangle_two), &triangle_two_size,
              &triangle_two_id
          ) &&
          !meiso_content_id_equal(&cube_id, &triangle_id) &&
          !meiso_content_id_equal(&triangle_id, &triangle_two_id));
    scene = scene_value(cube_id, 700U, 100U, 0.0F, true);
    moved = scene_value(cube_id, 700U, 200U, 1.0F, true);
    moved_again = scene_value(cube_id, 700U, 300U, 2.0F, true);
    missing = scene_value(triangle_id, 700U, 400U, -1.0F, true);
    empty = scene_value(cube_id, 700U, 500U, 0.0F, false);

    CHECK(posix_memalign((void **)&arena, alignment, required) == 0);
    memset(arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              arena, required, &config, &handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    arena_copy = malloc(required);
    CHECK(arena_copy != NULL);
    memmove(arena_copy, arena, required);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_put_mesh(
              handle, (MeisoByteView){arena, cube_size}, &unchanged_receipt
          ) == MEISO_EDGE_INVALID_ARGUMENT &&
          memcmp(arena, arena_copy, required) == 0 &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));

    CHECK(meiso_composition_edge_put_mesh(
              handle, (MeisoByteView){cube, cube_size}, &mesh_receipt
          ) == MEISO_EDGE_OK && mesh_receipt.command_id == 1U &&
          mesh_receipt.kind == MEISO_EDGE_COMMAND_PUT_MESH &&
          mesh_receipt.mesh_content_id_present &&
          meiso_content_id_equal(&mesh_receipt.mesh_content_id, &cube_id));
    CHECK(meiso_composition_edge_set_scene(
              handle, true, &scene, &scene_receipt
          ) == MEISO_EDGE_OK && scene_receipt.command_id == 2U &&
          scene_receipt.desired_state_version == 1U);
    CHECK(meiso_composition_edge_set_presentation(
              handle, true, &settings, &presentation_receipt
          ) == MEISO_EDGE_OK && presentation_receipt.command_id == 3U &&
          presentation_receipt.desired_state_version == 1U &&
          presentation_receipt.plan_generation_present &&
          presentation_receipt.plan_generation == 1U);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.mesh.queued && snapshot.mesh.total_bytes == cube_size &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_SCENE]
              .delivery_pending &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION]
              .delivery_pending);
    CHECK(meiso_composition_edge_simulator_activate(handle) == MEISO_EDGE_OK &&
          meiso_edge_operation_supported(
              handle, MEISO_EDGE_OPERATION_BLOB_PUT, &supported
          ) == MEISO_EDGE_OK && supported &&
          meiso_edge_operation_supported(
              handle, MEISO_EDGE_OPERATION_COMPOSITION, &supported
          ) == MEISO_EDGE_OK && supported);
    CHECK(meiso_composition_edge_snapshot(handle, &before) == MEISO_EDGE_OK);
    transition = UINT8_C(0xa5);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){701U, 999U}, &transition
          ) == MEISO_EDGE_INVALID_ARGUMENT && transition == UINT8_C(0xa5) &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.mesh.terminal_result_present &&
          snapshot.mesh.terminal_result ==
              MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          snapshot.mesh.edge_core_published &&
          snapshot.mesh.next_offset == cube_size);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1100U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE].present &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_status == MEISO_RUNTIME_PRODUCTION_STARTING);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1200U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.presentation_status_present &&
          snapshot.presentation_status.candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION]
                  .production_status == MEISO_RUNTIME_PRODUCTION_STARTING);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1300U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_ACTIVATED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION]
                  .state_version == 2U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION]
                  .production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION]
                  .production_epoch == 1001U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_epoch == 2001U &&
          snapshot.presentation_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          snapshot.presentation_status.active_plan_generation == 1U &&
          snapshot.scene_status_present &&
          snapshot.scene_status.reason == MEISO_COMPOSITION_SCENE_REASON_READY &&
          snapshot.scene_status.world_generation == 1U &&
          snapshot.scene_status.active_object_count == 1U &&
          snapshot.scene_status.unique_mesh_count == 1U &&
          snapshot.scene_status.published_blob_count == 1U &&
          snapshot.scene_status.published_blob_bytes == cube_size);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1400U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.render_result_present &&
          snapshot.render_result.result_code ==
              MEISO_COMPOSITION_SCENE_RESULT_COMPLETE &&
          snapshot.render_result.evidence_flags ==
              (MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |
               MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED) &&
          snapshot.render_result.world_generation == 1U &&
          snapshot.render_result.source_clock_epoch == 700U &&
          snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
          ].key.epoch == 2001U &&
          snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
          ].key.sequence == 1U &&
          snapshot.scene_status.last_result_stream_epoch == 2001U &&
          snapshot.scene_status.last_result_sequence == 1U);
    memmove(first_hash, snapshot.render_result.full_target_sha256,
            sizeof(first_hash));

    CHECK(meiso_composition_edge_set_render_media(
              handle, true, &render_desired, &media_receipt
          ) == MEISO_EDGE_OK &&
          media_receipt.desired_state_version == 1U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 1500U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED);
    render_bytes = malloc(MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES);
    CHECK(render_bytes != NULL);
    memset(render_bytes, 0x3c,
           MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES);
    memset(&frame, 0, sizeof(frame));
    frame.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    frame.media_stream_epoch = config.render_media_stream_epoch;
    frame.frame_sequence = 1U;
    frame.source_time = (MeisoTimePoint){700U, 1600U};
    frame.applied_scene_owner = config.desired_owner;
    frame.applied_scene_state_version = 1U;
    frame.active_plan_generation = 1U;
    frame.world_generation = 2U;
    frame.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    frame.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    frame.width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    frame.height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    frame.access_unit = (MeisoByteView){
        render_bytes, MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES
    };
    CHECK(meiso_composition_edge_snapshot(handle, &before) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
              MEISO_EDGE_CONFLICT &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    frame.world_generation = 1U;
    frame.applied_scene_state_version = 2U;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
              MEISO_EDGE_CONFLICT &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    frame.applied_scene_state_version = 1U;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_media_latest(
              handle, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &frame_view
          ) == MEISO_EDGE_OK &&
          frame_view.applied_scene_state_version == 1U &&
          frame_view.active_plan_generation == 1U &&
          frame_view.world_generation == 1U &&
          frame_view.access_unit.size ==
              MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES &&
          memcmp(frame_view.access_unit.data, render_bytes,
                 frame_view.access_unit.size) == 0 &&
          meiso_composition_edge_media_release(handle, &frame_view.key) ==
              MEISO_EDGE_OK);

    CHECK(meiso_composition_edge_set_scene(
              handle, true, &moved, &scene_receipt
          ) == MEISO_EDGE_OK && scene_receipt.desired_state_version == 2U);
    CHECK(meiso_composition_edge_set_scene(
              handle, true, &moved_again, &replacement_receipt
          ) == MEISO_EDGE_OK &&
          replacement_receipt.local_result ==
              MEISO_EDGE_LOCAL_REPLACED_PENDING &&
          replacement_receipt.replaced_command_present &&
          replacement_receipt.replaced_command_id == scene_receipt.command_id &&
          replacement_receipt.desired_state_version == 3U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 2000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 2100U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .observed_desired.version == 3U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_epoch == 2002U &&
          snapshot.scene_status.world_generation == 2U &&
          snapshot.render_result.world_generation == 2U &&
          snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
          ].key.sequence == 1U &&
          snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
          ].received_count == 2U &&
          snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
          ].replaced_count == 0U);
    CHECK(memcmp(first_hash, snapshot.render_result.full_target_sha256,
                 sizeof(first_hash)) != 0);

    CHECK(meiso_composition_edge_set_scene(
              handle, true, &missing, &scene_receipt
          ) == MEISO_EDGE_OK && scene_receipt.desired_state_version == 4U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_status == MEISO_RUNTIME_PRODUCTION_STARTING &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_epoch == 0U &&
          snapshot.scene_status.world_generation == 2U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3050U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE);
    CHECK(meiso_composition_edge_put_mesh(
              handle, (MeisoByteView){triangle, triangle_size}, &mesh_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3100U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3200U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3300U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_epoch == 2003U &&
          snapshot.scene_status.world_generation == 3U &&
          snapshot.scene_status.published_blob_count == 2U &&
          snapshot.scene_status.published_blob_bytes ==
              cube_size + triangle_size);

    CHECK(meiso_composition_edge_put_mesh(
              handle,
              (MeisoByteView){triangle_two, triangle_two_size},
              &mesh_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3400U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.mesh.terminal_result ==
              MEISO_RUNTIME_BLOB_RESULT_CAPACITY &&
          !snapshot.mesh.edge_core_published && snapshot.mesh.next_offset == 0U &&
          snapshot.scene_status.published_blob_count == 2U);
    CHECK(meiso_composition_edge_put_mesh(
              handle, (MeisoByteView){cube, cube_size}, &mesh_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 3500U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.mesh.terminal_result ==
              MEISO_RUNTIME_BLOB_RESULT_DUPLICATE &&
          snapshot.mesh.edge_core_published &&
          snapshot.scene_status.published_blob_count == 2U &&
          snapshot.scene_status.published_blob_bytes ==
              cube_size + triangle_size);

    CHECK(meiso_composition_edge_set_scene(
              handle, true, &empty, &scene_receipt
          ) == MEISO_EDGE_OK && scene_receipt.desired_state_version == 5U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 4000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 4100U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_RESULT_PUBLISHED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.scene_status.world_generation == 4U &&
          snapshot.scene_status.active_object_count == 0U &&
          snapshot.render_result.object_count == 0U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_epoch == 2004U);

    CHECK(meiso_composition_edge_set_scene(
              handle, false, NULL, &scene_receipt
          ) == MEISO_EDGE_OK && scene_receipt.desired_state_version == 6U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 4200U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE].apply_result ==
              MEISO_RUNTIME_APPLY_REJECTED &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .applied_desired.version == 5U &&
          snapshot.scene_status.world_generation == 4U);
    CHECK(meiso_composition_edge_set_presentation(
              handle, false, NULL, &presentation_receipt
          ) == MEISO_EDGE_OK &&
          presentation_receipt.desired_state_version == 3U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 4300U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION]
                  .production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE].apply_result ==
              MEISO_RUNTIME_APPLY_APPLIED &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .applied_desired.version == 6U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE]
                  .production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
          snapshot.presentation_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_NONE &&
          !snapshot.render_result_present &&
          !snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
          ].present);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){700U, 4400U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE);

    CHECK(meiso_composition_edge_simulator_terminate(
              handle, MEISO_EDGE_TERMINAL_LOCAL_ABORT
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL &&
          !snapshot.mesh.queued && !snapshot.presentation_status_present &&
          !snapshot.scene_status_present && !snapshot.render_result_present);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_scene(
              handle, true, &scene, &unchanged_receipt
          ) == MEISO_EDGE_TERMINAL &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));

    exhausted_config.presentation_production_epoch = 1001U;
    exhausted_config.scene_production_epoch = UINT64_MAX;
    CHECK(posix_memalign(
              (void **)&exhausted_arena, alignment, required
          ) == 0);
    memset(exhausted_arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              exhausted_arena, required, &exhausted_config,
              &exhausted_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    CHECK(meiso_composition_edge_put_mesh(
              exhausted_handle, (MeisoByteView){cube, cube_size},
              &mesh_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_scene(
              exhausted_handle, true, &scene, &scene_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_presentation(
              exhausted_handle, true, &settings, &presentation_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_activate(exhausted_handle) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){700U, 5000U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_MESH_PUBLISHED);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){700U, 5100U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_SCENE_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){700U, 5200U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){700U, 5300U}, &transition
          ) == MEISO_EDGE_OK && transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_PRESENTATION_ACTIVATED);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_scene(
              exhausted_handle, true, &moved, &unchanged_receipt
          ) == MEISO_EDGE_COUNTER_EXHAUSTED &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));
    CHECK(meiso_composition_edge_set_presentation(
              exhausted_handle, true, &settings, &unchanged_receipt
          ) == MEISO_EDGE_COUNTER_EXHAUSTED &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));
    CHECK(meiso_composition_edge_simulator_terminate(
              exhausted_handle, MEISO_EDGE_TERMINAL_LOCAL_ABORT
          ) == MEISO_EDGE_OK);

    free(exhausted_arena);
    free(render_bytes);
    free(arena_copy);
    free(arena);
    puts("composition virtual edge Composition simulator: PASS");
    return 0;

fail:
    if (handle != NULL &&
        meiso_composition_edge_snapshot(handle, &before) == MEISO_EDGE_OK) {
        (void)before;
    }
    free(arena_copy);
    free(render_bytes);
    free(exhausted_arena);
    free(short_arena);
    free(arena);
    return 1;
}
