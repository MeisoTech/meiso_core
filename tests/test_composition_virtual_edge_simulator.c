#define _POSIX_C_SOURCE 200809L

#include "composition_virtual_edge_application.h"
#include "meiso_composition_edge_simulator.h"

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
    const uint8_t *bytes = (const uint8_t *)value;
    size_t i;

    for (i = 0U; i < size; ++i) {
        if (bytes[i] != byte) {
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
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        100U, 1000U, 100U
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 |
            MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGB565,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        64U, 48U, 320U, 240U, 10000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 640U, 480U, 30000U,
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static MeisoCompositionEdgeSimulatorConfig simulator_config(
    uint64_t continuity_serial
)
{
    MeisoCompositionEdgeSimulatorConfig value;

    memset(&value, 0, sizeof(value));
    value.catalog_generation = 1U;
    value.catalog = catalog_config();
    value.continuity_serial = continuity_serial;
    value.desired_owner = (MeisoOwnerScope){MEISO_SESSION_ROLE_HOST, 31U};
    value.wifi_production_epoch = 77U;
    value.render_media_stream_epoch = 501U;
    value.camera_media_stream_epoch = 602U;
    value.wifi_sample.selected_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    value.wifi_sample.present_fields = value.wifi_sample.selected_fields;
    value.wifi_sample.link_state = true;
    value.wifi_sample.signal_mbm = -5100;
    value.device_telemetry_enabled = true;
    value.device_production_epoch = 88U;
    value.device_sample.selected_fields = MEISO_DEVICE_TELEMETRY_FIELD_ALL;
    value.device_sample.present_fields = MEISO_DEVICE_TELEMETRY_FIELD_ALL;
    value.device_sample.temperature_millic = 54500;
    value.device_sample.temperature_uncertainty_millic = 500U;
    value.device_sample.cpu_load_ppm = 250000U;
    value.device_sample.memory_total_bytes = UINT64_C(67108864);
    value.device_sample.memory_available_bytes = UINT64_C(33554432);
    return value;
}

int main(void)
{
    MeisoCompositionEdgeSimulatorConfig config = simulator_config(9001U);
    MeisoCompositionEdgeSimulatorConfig fresh_config = simulator_config(9002U);
    MeisoCompositionEdgeSimulatorConfig exhausted_config =
        simulator_config(9003U);
    MeisoCompositionEdgeSimulatorConfig invalid_config = config;
    MeisoCompositionEdgeSimulatorConfig unsupported_config = config;
    MeisoEdgeHandle *handle = NULL;
    MeisoEdgeHandle *fresh_handle = NULL;
    MeisoEdgeHandle *replay_handle = NULL;
    MeisoEdgeHandle *exhausted_handle = NULL;
    MeisoEdgeHandle *unsupported_handle = NULL;
    MeisoEdgeCommandReceipt wifi_receipt;
    MeisoEdgeCommandReceipt camera_receipt;
    MeisoEdgeCommandReceipt device_receipt;
    MeisoEdgeCommandReceipt replacement_receipt;
    MeisoEdgeCommandReceipt replay_receipt;
    MeisoEdgeCommandReceipt unchanged_receipt;
    MeisoCompositionEdgeSnapshot snapshot;
    MeisoCompositionEdgeSnapshot before;
    MeisoCompositionEdgeSimulatorTransition transition;
    MeisoCompositionMediaDesired camera_desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    MeisoCompositionMediaDesired render_desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    MeisoDeviceTelemetryParameters device_desired = {
        MEISO_DEVICE_TELEMETRY_FIELD_ALL, 500U
    };
    MeisoDeviceTelemetryParameters device_subset = {
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM |
            MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES,
        500U
    };
    MeisoCompositionSceneDesired scene;
    MeisoCompositionEdgeSimulatorFrame frame;
    uint8_t *arena = NULL;
    uint8_t *fresh_arena = NULL;
    uint8_t *replay_arena = NULL;
    uint8_t *exhausted_arena = NULL;
    uint8_t *unsupported_arena = NULL;
    uint8_t *short_arena = NULL;
    uint8_t *arena_copy = NULL;
    uint8_t *camera_bytes = NULL;
    uint8_t *render_bytes = NULL;
    size_t required =
        meiso_composition_edge_simulator_memory_required(&config);
    const size_t alignment =
        meiso_composition_edge_simulator_alignment_required();
    size_t i;

    CHECK(required != 0U && alignment != 0U);
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    fresh_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required - 1U, &config, &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_ARENA_TOO_SMALL);
    CHECK(bytes_all(short_arena, required, UINT8_C(0xa5)) &&
          fresh_handle == (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;

    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required, &config,
              (MeisoEdgeHandle **)(void *)short_arena
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT &&
          bytes_all(short_arena, required, UINT8_C(0xa5)));
    memmove(short_arena, &config, sizeof(config));
    arena_copy = malloc(required);
    CHECK(arena_copy != NULL);
    memmove(arena_copy, short_arena, required);
    fresh_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required,
              (const MeisoCompositionEdgeSimulatorConfig *)(const void *)
                  short_arena,
              &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT &&
          memcmp(short_arena, arena_copy, required) == 0 &&
          fresh_handle == (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;

    CHECK(posix_memalign(
              (void **)&short_arena, alignment, required + alignment
          ) == 0);
    memset(short_arena + 1U, 0xa5, required);
    fresh_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena + 1U, required, &config, &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_MISALIGNED_ARENA &&
          bytes_all(short_arena + 1U, required, UINT8_C(0xa5)) &&
          fresh_handle == (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;

    invalid_config.wifi_sample.signal_mbm = 1;
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    fresh_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required, &invalid_config, &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG &&
          bytes_all(short_arena, required, UINT8_C(0xa5)) &&
          fresh_handle == (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;

    invalid_config = config;
    invalid_config.device_production_epoch = 0U;
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    fresh_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required, &invalid_config, &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG &&
          bytes_all(short_arena, required, UINT8_C(0xa5)) &&
          fresh_handle == (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;

    invalid_config = config;
    invalid_config.device_sample.temperature_uncertainty_millic = 1001U;
    CHECK(posix_memalign((void **)&short_arena, alignment, required) == 0);
    memset(short_arena, 0xa5, required);
    fresh_handle = (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_simulator_init(
              short_arena, required, &invalid_config, &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG &&
          bytes_all(short_arena, required, UINT8_C(0xa5)) &&
          fresh_handle == (MeisoEdgeHandle *)(uintptr_t)UINT64_C(0x1234));
    free(short_arena);
    short_arena = NULL;

    CHECK(posix_memalign((void **)&arena, alignment, required) == 0);
    memset(arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              arena, required, &config, &handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    memmove(arena_copy, arena, required);
    CHECK(meiso_composition_edge_snapshot(
              handle, (MeisoCompositionEdgeSnapshot *)(void *)arena
          ) == MEISO_EDGE_INVALID_ARGUMENT &&
          memcmp(arena, arena_copy, required) == 0);
    CHECK(meiso_composition_edge_set_wifi(
              handle, true,
              &(MeisoWifiTelemetryDesiredParameters){
                  MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE, 500U
              },
              (MeisoEdgeCommandReceipt *)(void *)arena
          ) == MEISO_EDGE_INVALID_ARGUMENT &&
          memcmp(arena, arena_copy, required) == 0);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_device(
              handle, true,
              (const MeisoDeviceTelemetryParameters *)(const void *)arena,
              &unchanged_receipt
          ) == MEISO_EDGE_INVALID_ARGUMENT &&
          memcmp(arena, arena_copy, required) == 0 &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));
    CHECK(meiso_composition_edge_simulator_publish_frame(
              handle,
              (const MeisoCompositionEdgeSimulatorFrame *)(const void *)arena
          ) == MEISO_EDGE_INVALID_ARGUMENT &&
          memcmp(arena, arena_copy, required) == 0);
    CHECK(meiso_composition_edge_media_release(
              handle, (const MeisoEdgeMediaFrameKey *)(const void *)arena
          ) == MEISO_EDGE_INVALID_ARGUMENT &&
          memcmp(arena, arena_copy, required) == 0);
    CHECK(meiso_test_virtual_edge_bootstrap(handle));
    CHECK(meiso_test_virtual_edge_submit_wifi(handle, &wifi_receipt));
    CHECK(meiso_composition_edge_simulator_activate(handle) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){10U, 1000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK);
    CHECK(snapshot.edge.phase == MEISO_EDGE_PHASE_READY &&
          snapshot.edge.resource_graph_live &&
          snapshot.edge.operations ==
              (MEISO_EDGE_OPERATION_STATE_READ |
               MEISO_EDGE_OPERATION_STATE_SUBMIT |
               MEISO_EDGE_OPERATION_STREAM_READ |
               MEISO_EDGE_OPERATION_MEDIA_READ) &&
          snapshot.edge.endpoint_count ==
              MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT &&
          snapshot.edge.resource_count ==
              MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI]
              .delivery_accepted &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].present &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
              .production_epoch == 77U &&
          snapshot.wifi_sample_present &&
          snapshot.wifi_sample.signal_mbm == -5100 &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
              .key.sequence == 1U);
    CHECK(posix_memalign((void **)&replay_arena, alignment, required) == 0);
    memset(replay_arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              replay_arena, required, &config, &replay_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    CHECK(meiso_test_virtual_edge_submit_wifi(replay_handle, &replay_receipt));
    CHECK(meiso_composition_edge_simulator_activate(replay_handle) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              replay_handle, (MeisoTimePoint){10U, 1000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED &&
          meiso_composition_edge_snapshot(replay_handle, &snapshot) ==
              MEISO_EDGE_OK &&
          snapshot.edge.phase == MEISO_EDGE_PHASE_READY &&
          snapshot.edge.online && snapshot.edge.resource_graph_live &&
          snapshot.edge.continuity_serial == config.continuity_serial &&
          snapshot.edge.catalog_generation == config.catalog_generation &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI]
                  .command_id == replay_receipt.command_id &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI]
                  .state_version == replay_receipt.desired_state_version &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI]
                  .delivery_accepted &&
          !snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI]
                   .delivery_pending &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].present &&
          meiso_owner_scope_equal(
              snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].state_owner,
              config.desired_owner
          ) &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .state_version == 1U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .observed_desired.present &&
          meiso_owner_scope_equal(
              snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .observed_desired.owner,
              config.desired_owner
          ) &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .observed_desired.version == 1U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .applied_desired.present &&
          meiso_owner_scope_equal(
              snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .applied_desired.owner,
              config.desired_owner
          ) &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .applied_desired.version == 1U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].apply_result ==
              MEISO_RUNTIME_APPLY_APPLIED &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI]
                  .production_epoch == config.wifi_production_epoch &&
          snapshot.wifi_effective_present &&
          snapshot.wifi_effective.selected_fields ==
              (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
               MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
          snapshot.wifi_effective.sample_period_ms == 500U &&
          snapshot.wifi_sample_present && snapshot.wifi_sample.link_state &&
          snapshot.wifi_sample.signal_mbm == -5100 &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
              .present &&
          meiso_owner_scope_equal(
              snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
                  .key.producer,
              config.desired_owner
          ) &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
                  .key.epoch == config.wifi_production_epoch &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
                  .key.sequence == 1U &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
                  .source_time.clock_epoch == 10U &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
                  .source_time.monotonic_ns == 1000U);
    CHECK(meiso_composition_edge_simulator_terminate(
              replay_handle, MEISO_EDGE_TERMINAL_LOCAL_ABORT
          ) == MEISO_EDGE_OK);
    free(replay_arena);
    replay_arena = NULL;
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT; ++i) {
        MeisoEdgeEndpointView endpoint;
        CHECK(meiso_edge_endpoint_at(handle, i, &endpoint) == MEISO_EDGE_OK &&
              meiso_endpoint_id_valid(endpoint.id) &&
              endpoint.incarnation != 0U && endpoint.capability.size != 0U);
    }
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT; ++i) {
        MeisoEdgeResourceView resource;
        CHECK(meiso_edge_resource_at(handle, i, &resource) == MEISO_EDGE_OK &&
              meiso_resource_descriptor_validate(&resource.resource) ==
                  MEISO_DESCRIPTOR_VALID);
    }
    {
        bool supported = true;
        CHECK(meiso_edge_operation_supported(
                  handle, MEISO_EDGE_OPERATION_COMPOSITION, &supported
              ) == MEISO_EDGE_OK && !supported);
    }

    memset(&scene, 0, sizeof(scene));
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_scene(
              handle, true, &scene, &unchanged_receipt
          ) == MEISO_EDGE_UNSUPPORTED &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));
    CHECK(meiso_composition_edge_set_device(
              handle, true, &device_desired, &device_receipt
          ) == MEISO_EDGE_OK &&
          device_receipt.kind == MEISO_EDGE_COMMAND_SET_DEVICE &&
          device_receipt.desired_channel ==
              MEISO_COMPOSITION_EDGE_DESIRED_DEVICE &&
          device_receipt.desired_state_version == 1U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){10U, 1250U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_DEVICE]
              .delivery_accepted &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE].present &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE]
                  .production_epoch == config.device_production_epoch &&
          snapshot.device_effective_present &&
          snapshot.device_effective.selected_fields ==
              MEISO_DEVICE_TELEMETRY_FIELD_ALL &&
          snapshot.device_effective.sample_period_ms == 500U &&
          snapshot.device_sample_present &&
          snapshot.device_sample.temperature_millic == 54500 &&
          snapshot.device_sample.temperature_uncertainty_millic == 500U &&
          snapshot.device_sample.cpu_load_ppm == 250000U &&
          snapshot.device_sample.memory_total_bytes == UINT64_C(67108864) &&
          snapshot.device_sample.memory_available_bytes == UINT64_C(33554432) &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
              .present &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
                  .key.epoch == config.device_production_epoch &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
                  .key.sequence == 1U &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
                  .source_time.monotonic_ns == 1250U);
    CHECK(meiso_composition_edge_set_device(
              handle, true, &device_subset, &device_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){10U, 1300U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE]
                  .production_epoch == config.device_production_epoch + 1U &&
          snapshot.device_sample.selected_fields ==
              device_subset.selected_fields &&
          snapshot.device_sample.present_fields ==
              device_subset.selected_fields &&
          snapshot.device_sample.temperature_millic == 0 &&
          snapshot.device_sample.temperature_uncertainty_millic == 0U &&
          snapshot.device_sample.cpu_load_ppm == 250000U &&
          snapshot.device_sample.memory_total_bytes == 0U &&
          snapshot.device_sample.memory_available_bytes == UINT64_C(33554432) &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
                  .key.epoch == config.device_production_epoch + 1U &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
                  .received_count == 2U);
    CHECK(meiso_composition_edge_set_camera_media(
              handle, true, &camera_desired, &camera_receipt
          ) == MEISO_EDGE_OK);
    CHECK(camera_receipt.desired_channel ==
              MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA);
    CHECK(meiso_composition_edge_set_camera_media(
              handle, true, &camera_desired, &replacement_receipt
          ) == MEISO_EDGE_OK &&
          replacement_receipt.local_result ==
              MEISO_EDGE_LOCAL_REPLACED_PENDING &&
          replacement_receipt.replaced_command_present &&
          replacement_receipt.replaced_command_id == camera_receipt.command_id &&
          replacement_receipt.desired_state_version == 2U);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){10U, 1500U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA]
              .observed_desired.version == 2U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA]
              .applied_desired.version == 2U &&
          snapshot.camera_media_actual_present &&
          snapshot.camera_media_actual.media_stream_epoch == 602U);

    camera_bytes = malloc(115200U);
    CHECK(camera_bytes != NULL);
    for (i = 0U; i < 115200U; ++i) {
        camera_bytes[i] = (uint8_t)((i * 17U + 3U) & 0xffU);
    }
    memset(&frame, 0, sizeof(frame));
    frame.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    frame.media_stream_epoch = 602U;
    frame.frame_sequence = 1U;
    frame.source_time = (MeisoTimePoint){10U, 2000U};
    frame.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    frame.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    frame.width = 320U;
    frame.height = 240U;
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    frame.access_unit = (MeisoByteView){camera_bytes, 115200U};
    CHECK(meiso_composition_edge_snapshot(handle, &before) == MEISO_EDGE_OK);
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_BEGIN |
        MEISO_COMPOSITION_MEDIA_FRAME_END |
        MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
              MEISO_EDGE_CONFLICT &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    frame.flags = 0U;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
              MEISO_EDGE_CONFLICT &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    frame.access_unit.size = 115199U;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
              MEISO_EDGE_CONFLICT &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    frame.access_unit.size = 115200U;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
          MEISO_EDGE_OK);
    CHECK(meiso_test_virtual_edge_camera_frame(
        handle, 9001U, camera_bytes, 115200U));

    CHECK(meiso_composition_edge_set_render_media(
              handle, true, &render_desired, &camera_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){10U, 2500U}, &transition
          ) == MEISO_EDGE_OK &&
          transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED);
    render_bytes = malloc(307200U);
    CHECK(render_bytes != NULL);
    for (i = 0U; i < 307200U; ++i) {
        render_bytes[i] = (uint8_t)((i * 29U + 7U) & 0xffU);
    }
    memset(&frame, 0, sizeof(frame));
    frame.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    frame.media_stream_epoch = 501U;
    frame.frame_sequence = 1U;
    frame.source_time = (MeisoTimePoint){10U, 3000U};
    frame.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    frame.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    frame.width = 320U;
    frame.height = 240U;
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    frame.access_unit = (MeisoByteView){render_bytes, 307200U};
    CHECK(meiso_composition_edge_snapshot(handle, &before) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
              MEISO_EDGE_CONFLICT &&
          meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          memcmp(&before, &snapshot, sizeof(snapshot)) == 0);
    frame.applied_scene_owner = config.desired_owner;
    frame.applied_scene_state_version = 41U;
    frame.active_plan_generation = 42U;
    frame.world_generation = 43U;
    CHECK(meiso_composition_edge_simulator_publish_frame(handle, &frame) ==
          MEISO_EDGE_OK);
    {
        MeisoEdgeMediaFrameView render_frame;
        CHECK(meiso_composition_edge_media_latest(
                  handle, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
                  &render_frame
              ) == MEISO_EDGE_OK &&
              render_frame.key.continuity_serial == 9001U &&
              render_frame.key.media_stream_epoch == 501U &&
              render_frame.key.frame_sequence == 1U &&
              meiso_owner_scope_equal(render_frame.applied_scene_owner,
                                      config.desired_owner) &&
              render_frame.applied_scene_state_version == 41U &&
              render_frame.active_plan_generation == 42U &&
              render_frame.world_generation == 43U &&
              render_frame.access_unit.size == 307200U &&
              memcmp(render_frame.access_unit.data, render_bytes,
                     307200U) == 0 &&
              meiso_composition_edge_media_release(handle,
                                                    &render_frame.key) ==
                  MEISO_EDGE_OK);
    }

    CHECK(meiso_composition_edge_set_device(
              handle, false, NULL, &device_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              handle, (MeisoTimePoint){10U, 3500U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) == MEISO_EDGE_OK &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE].present &&
          !snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE]
               .effective_enabled &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE]
                  .production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE]
                  .production_epoch == 0U &&
          !snapshot.device_effective_present &&
          !snapshot.device_sample_present &&
          !snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE]
               .present);

    CHECK(meiso_composition_edge_simulator_terminate(
              handle, MEISO_EDGE_TERMINAL_LOCAL_ABORT
          ) == MEISO_EDGE_OK);
    CHECK(meiso_test_virtual_edge_terminal(handle));

    CHECK(posix_memalign((void **)&fresh_arena, alignment, required) == 0);
    memset(fresh_arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              fresh_arena, required, &fresh_config, &fresh_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    CHECK(meiso_composition_edge_snapshot(fresh_handle, &snapshot) ==
              MEISO_EDGE_OK &&
          snapshot.edge.continuity_serial == 9002U &&
          snapshot.edge.phase == MEISO_EDGE_PHASE_BOOTSTRAPPING);
    CHECK(meiso_composition_edge_set_device(
              fresh_handle, true, &device_desired, &device_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_wifi(
              fresh_handle, true,
              &(MeisoWifiTelemetryDesiredParameters){
                  MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE, 500U
              },
              &wifi_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_activate(fresh_handle) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              fresh_handle, (MeisoTimePoint){30U, 4000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED);
    CHECK(meiso_composition_edge_simulator_step(
              fresh_handle, (MeisoTimePoint){30U, 4100U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED);

    unsupported_config.device_telemetry_enabled = false;
    unsupported_config.device_production_epoch = 0U;
    memset(&unsupported_config.device_sample, 0,
           sizeof(unsupported_config.device_sample));
    CHECK(posix_memalign((void **)&unsupported_arena, alignment, required) == 0);
    memset(unsupported_arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              unsupported_arena, required, &unsupported_config,
              &unsupported_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    CHECK(meiso_composition_edge_simulator_activate(unsupported_handle) ==
          MEISO_EDGE_OK);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_device(
              unsupported_handle, true, &device_desired, &unchanged_receipt
          ) == MEISO_EDGE_UNSUPPORTED &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));

    exhausted_config.camera_media_stream_epoch = UINT64_MAX;
    exhausted_config.device_production_epoch = UINT64_MAX;
    CHECK(posix_memalign((void **)&exhausted_arena, alignment, required) == 0);
    memset(exhausted_arena, 0, required);
    CHECK(meiso_composition_edge_simulator_init(
              exhausted_arena, required, &exhausted_config,
              &exhausted_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    CHECK(meiso_composition_edge_simulator_activate(exhausted_handle) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_set_camera_media(
              exhausted_handle, true, &camera_desired, &camera_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){20U, 1000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED);
    CHECK(meiso_composition_edge_set_camera_media(
              exhausted_handle, false, NULL, &camera_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){20U, 2000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_camera_media(
              exhausted_handle, true, &camera_desired, &unchanged_receipt
          ) == MEISO_EDGE_COUNTER_EXHAUSTED &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));
    CHECK(meiso_composition_edge_set_device(
              exhausted_handle, true, &device_desired, &device_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){20U, 3000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED);
    CHECK(meiso_composition_edge_set_device(
              exhausted_handle, false, NULL, &device_receipt
          ) == MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_simulator_step(
              exhausted_handle, (MeisoTimePoint){20U, 4000U}, &transition
          ) == MEISO_EDGE_OK &&
          transition == MEISO_COMPOSITION_EDGE_SIMULATOR_DEVICE_APPLIED);
    memset(&unchanged_receipt, 0xa5, sizeof(unchanged_receipt));
    CHECK(meiso_composition_edge_set_device(
              exhausted_handle, true, &device_desired, &unchanged_receipt
          ) == MEISO_EDGE_COUNTER_EXHAUSTED &&
          bytes_all(&unchanged_receipt, sizeof(unchanged_receipt),
                    UINT8_C(0xa5)));

    free(camera_bytes);
    free(render_bytes);
    free(arena_copy);
    free(exhausted_arena);
    free(unsupported_arena);
    free(fresh_arena);
    free(replay_arena);
    free(arena);
    puts("composition virtual edge simulator: PASS");
    return 0;

fail:
    free(camera_bytes);
    free(render_bytes);
    free(arena_copy);
    free(exhausted_arena);
    free(unsupported_arena);
    free(fresh_arena);
    free(replay_arena);
    free(short_arena);
    free(arena);
    return 1;
}
