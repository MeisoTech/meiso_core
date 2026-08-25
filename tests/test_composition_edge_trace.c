#define _POSIX_C_SOURCE 200809L

#include "composition_virtual_edge_application.h"
#include "meiso_composition_edge_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_CAPACITY UINT32_C(1048576)
#define TRACE_MINIMUM_BYTES UINT32_C(496)
#define EXPECTED_TRACE_BYTES UINT32_C(730792)

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

static uint32_t get_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24U |
        (uint32_t)bytes[1] << 16U |
        (uint32_t)bytes[2] << 8U |
        (uint32_t)bytes[3];
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static uint32_t crc32c_update(
    uint32_t crc,
    const uint8_t *bytes,
    size_t size
)
{
    size_t i;
    unsigned bit;

    for (i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                ((crc & 1U) != 0U ? UINT32_C(0x82f63b78) : 0U);
        }
    }
    return crc;
}

static uint32_t crc32c(const uint8_t *bytes, size_t size)
{
    if (size == 0U) {
        return 0U;
    }
    return crc32c_update(UINT32_MAX, bytes, size) ^ UINT32_MAX;
}

static void refresh_trace_header(uint8_t *trace, size_t size)
{
    put_u32(trace + 20U, crc32c(trace + 32U, size - 32U));
    memset(trace + 24U, 0, 4U);
    put_u32(trace + 24U, crc32c(trace, 32U));
}

static size_t record_offset(const uint8_t *trace, uint32_t sequence)
{
    size_t cursor = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES;
    uint32_t current;

    for (current = 1U; current < sequence; ++current) {
        cursor += MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
            get_u32(trace + cursor + 4U);
    }
    return cursor;
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
    return value;
}

static bool config_fields_equal(
    const MeisoCompositionEdgeSimulatorConfig *lhs,
    const MeisoCompositionEdgeSimulatorConfig *rhs
)
{
    return lhs->catalog_generation == rhs->catalog_generation &&
        lhs->continuity_serial == rhs->continuity_serial &&
        meiso_owner_scope_equal(lhs->desired_owner, rhs->desired_owner) &&
        lhs->wifi_production_epoch == rhs->wifi_production_epoch &&
        lhs->render_media_stream_epoch == rhs->render_media_stream_epoch &&
        lhs->camera_media_stream_epoch == rhs->camera_media_stream_epoch &&
        lhs->device_telemetry_enabled == rhs->device_telemetry_enabled &&
        lhs->device_production_epoch == rhs->device_production_epoch &&
        lhs->device_sample.selected_fields ==
            rhs->device_sample.selected_fields &&
        lhs->device_sample.present_fields == rhs->device_sample.present_fields &&
        lhs->device_sample.temperature_millic ==
            rhs->device_sample.temperature_millic &&
        lhs->device_sample.temperature_uncertainty_millic ==
            rhs->device_sample.temperature_uncertainty_millic &&
        lhs->device_sample.cpu_load_ppm == rhs->device_sample.cpu_load_ppm &&
        lhs->device_sample.memory_total_bytes ==
            rhs->device_sample.memory_total_bytes &&
        lhs->device_sample.memory_available_bytes ==
            rhs->device_sample.memory_available_bytes &&
        lhs->composition_enabled == rhs->composition_enabled &&
        lhs->composition_blob_slot_count ==
            rhs->composition_blob_slot_count &&
        lhs->composition_clock_epoch == rhs->composition_clock_epoch &&
        lhs->presentation_production_epoch ==
            rhs->presentation_production_epoch &&
        lhs->scene_production_epoch == rhs->scene_production_epoch &&
        lhs->cpu_raster_preview_enabled ==
            rhs->cpu_raster_preview_enabled &&
        lhs->catalog.wifi_incarnation == rhs->catalog.wifi_incarnation &&
        lhs->catalog.presentation_incarnation ==
            rhs->catalog.presentation_incarnation &&
        lhs->catalog.scene_incarnation == rhs->catalog.scene_incarnation &&
        lhs->catalog.device_telemetry_incarnation ==
            rhs->catalog.device_telemetry_incarnation &&
        lhs->catalog.render_media_incarnation ==
            rhs->catalog.render_media_incarnation &&
        lhs->catalog.camera_media_incarnation ==
            rhs->catalog.camera_media_incarnation &&
        lhs->wifi_sample.selected_fields == rhs->wifi_sample.selected_fields &&
        lhs->wifi_sample.present_fields == rhs->wifi_sample.present_fields &&
        lhs->wifi_sample.link_state == rhs->wifi_sample.link_state &&
        lhs->wifi_sample.signal_mbm == rhs->wifi_sample.signal_mbm;
}

int main(void)
{
    MeisoCompositionEdgeSimulatorConfig config = simulator_config(9001U);
    MeisoCompositionEdgeSimulatorConfig wrong_config = simulator_config(9002U);
    MeisoCompositionEdgeSimulatorConfig decoded;
    MeisoCompositionEdgeTraceWriter *writer = NULL;
    MeisoCompositionEdgeTraceReader *reader = NULL;
    MeisoCompositionEdgeTraceReader *tamper_reader = NULL;
    MeisoEdgeHandle *handle = NULL;
    MeisoEdgeHandle *wrong_handle = NULL;
    MeisoCompositionEdgeTraceReplayResult result;
    MeisoCompositionEdgeTraceReplayResult unchanged_result;
    MeisoCompositionEdgeTraceStatus replay_status;
    MeisoCompositionEdgeSnapshot snapshot;
    MeisoEdgeMediaFrameView media_frame;
    MeisoByteView trace = {NULL, 0U};
    MeisoByteView unchanged_view;
    MeisoWifiTelemetryDesiredParameters wifi_desired = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
    };
    MeisoCompositionMediaDesired render_desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    MeisoCompositionMediaDesired camera_desired = {
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 1000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    MeisoCompositionEdgeSimulatorFrame render_frame;
    MeisoCompositionEdgeSimulatorFrame camera_frame;
    uint8_t *writer_arena = NULL;
    uint8_t *reader_arena = NULL;
    uint8_t *tamper_reader_arena = NULL;
    uint8_t *handle_arena = NULL;
    uint8_t *wrong_handle_arena = NULL;
    uint8_t *trace_bytes = NULL;
    uint8_t *small_trace = NULL;
    uint8_t *trace_copy = NULL;
    uint8_t *mutated = NULL;
    uint8_t *arena_copy = NULL;
    uint8_t *buffer_copy = NULL;
    uint8_t *render_bytes = NULL;
    uint8_t *camera_bytes = NULL;
    const size_t writer_size =
        meiso_composition_edge_trace_writer_memory_required();
    const size_t writer_alignment =
        meiso_composition_edge_trace_writer_alignment_required();
    const size_t reader_size =
        meiso_composition_edge_trace_reader_memory_required();
    const size_t reader_alignment =
        meiso_composition_edge_trace_reader_alignment_required();
    const size_t handle_size =
        meiso_composition_edge_simulator_memory_required(&config);
    const size_t wrong_handle_size =
        meiso_composition_edge_simulator_memory_required(&wrong_config);
    const size_t handle_alignment =
        meiso_composition_edge_simulator_alignment_required();
    size_t offset;
    size_t i;

    CHECK(writer_size != 0U && writer_alignment != 0U &&
          reader_size != 0U && reader_alignment != 0U &&
          handle_size != 0U && handle_alignment != 0U);
    CHECK(posix_memalign((void **)&writer_arena, writer_alignment,
                         writer_size + writer_alignment) == 0);
    CHECK(posix_memalign((void **)&reader_arena, reader_alignment,
                         reader_size + reader_alignment) == 0);
    CHECK(posix_memalign((void **)&tamper_reader_arena, reader_alignment,
                         reader_size) == 0);
    CHECK(posix_memalign((void **)&handle_arena, handle_alignment,
                         handle_size) == 0);
    CHECK(posix_memalign((void **)&wrong_handle_arena, handle_alignment,
                         wrong_handle_size) == 0);
    trace_bytes = malloc(TRACE_CAPACITY);
    small_trace = malloc(TRACE_MINIMUM_BYTES);
    trace_copy = malloc(TRACE_CAPACITY);
    mutated = malloc(TRACE_CAPACITY);
    arena_copy = malloc(writer_size);
    buffer_copy = malloc(TRACE_CAPACITY);
    render_bytes = malloc(MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES);
    camera_bytes = malloc(320U * 240U * 3U / 2U);
    CHECK(trace_bytes != NULL && small_trace != NULL &&
          trace_copy != NULL && mutated != NULL &&
          arena_copy != NULL && buffer_copy != NULL && render_bytes != NULL &&
          camera_bytes != NULL);
    for (i = 0U; i < MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES;
         ++i) {
        render_bytes[i] = (uint8_t)(i * 29U + 7U);
    }
    for (i = 0U; i < 320U * 240U * 3U / 2U; ++i) {
        camera_bytes[i] = (uint8_t)(i * 17U + 3U);
    }
    wrong_config.device_telemetry_enabled = true;
    wrong_config.device_production_epoch = 88U;
    wrong_config.device_sample.selected_fields =
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
    wrong_config.device_sample.present_fields =
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
    wrong_config.device_sample.cpu_load_ppm = 250000U;

    memset(writer_arena, 0xa5, writer_size + writer_alignment);
    memset(trace_bytes, 0xa5, TRACE_CAPACITY);
    writer = (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size - 1U,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL &&
          bytes_all(writer_arena, writer_size + writer_alignment, 0xa5) &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5) &&
          writer ==
              (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234));
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena + 1U, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5));
    CHECK(meiso_composition_edge_trace_writer_init(
              trace_bytes, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5));
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &wrong_config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG &&
          bytes_all(writer_arena, writer_size + writer_alignment, 0xa5) &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5) &&
          writer ==
              (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234));
    wrong_config = config;
    wrong_config.device_sample.selected_fields =
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
    wrong_config.device_sample.present_fields =
        MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM;
    wrong_config.device_sample.cpu_load_ppm = 250000U;
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &wrong_config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG &&
          bytes_all(writer_arena, writer_size + writer_alignment, 0xa5) &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5) &&
          writer ==
              (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234));
    wrong_config = config;
    wrong_config.composition_enabled = true;
    wrong_config.composition_blob_slot_count = 1U;
    wrong_config.composition_clock_epoch = 701U;
    wrong_config.presentation_production_epoch = 801U;
    wrong_config.scene_production_epoch = 901U;
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &wrong_config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG &&
          bytes_all(writer_arena, writer_size + writer_alignment, 0xa5) &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5) &&
          writer ==
              (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234));
    wrong_config = config;
    wrong_config.cpu_raster_preview_enabled = true;
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &wrong_config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG &&
          bytes_all(writer_arena, writer_size + writer_alignment, 0xa5) &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5) &&
          writer ==
              (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234));
    wrong_config = config;
    wrong_config.scene_production_epoch = 1U;
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &wrong_config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG &&
          bytes_all(writer_arena, writer_size + writer_alignment, 0xa5) &&
          bytes_all(trace_bytes, TRACE_CAPACITY, 0xa5) &&
          writer ==
              (MeisoCompositionEdgeTraceWriter *)(uintptr_t)UINT64_C(0x1234));
    wrong_config = simulator_config(9002U);

    memset(writer_arena, 0, writer_size);
    memset(small_trace, 0xa5, TRACE_MINIMUM_BYTES);
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){small_trace, TRACE_MINIMUM_BYTES},
              &config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_set_wifi(
              writer, true, &wifi_desired
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    memmove(arena_copy, writer_arena, writer_size);
    memmove(buffer_copy, small_trace, TRACE_MINIMUM_BYTES);
    CHECK(meiso_composition_edge_trace_writer_record_activate(writer) ==
              MEISO_COMPOSITION_EDGE_TRACE_CAPACITY &&
          memcmp(writer_arena, arena_copy, writer_size) == 0 &&
          memcmp(small_trace, buffer_copy, TRACE_MINIMUM_BYTES) == 0);

    memset(writer_arena, 0, writer_size);
    memset(trace_bytes, 0, TRACE_CAPACITY);
    CHECK(meiso_composition_edge_trace_writer_init(
              writer_arena, writer_size,
              (MeisoMutableBuffer){trace_bytes, TRACE_CAPACITY},
              &config, &writer
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    memmove(arena_copy, writer_arena, writer_size);
    memmove(buffer_copy, trace_bytes, TRACE_CAPACITY);
    render_frame = (MeisoCompositionEdgeSimulatorFrame){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW, 501U, 1U,
        {10U, 1500U}, {MEISO_OWNER_ROLE_HOST, 31U}, 7U, 8U, 9U, 0U, 0U,
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW, 320U, 240U,
        MEISO_COMPOSITION_MEDIA_FRAME_KEY,
        {render_bytes, MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES}
    };
    CHECK(meiso_composition_edge_trace_writer_record_frame(
              writer, &render_frame
          ) == MEISO_COMPOSITION_EDGE_TRACE_ORDER &&
          memcmp(writer_arena, arena_copy, writer_size) == 0 &&
          memcmp(trace_bytes, buffer_copy, TRACE_CAPACITY) == 0);
    unchanged_view = (MeisoByteView){
        (const uint8_t *)(uintptr_t)UINT64_C(0x1234), 99U
    };
    CHECK(meiso_composition_edge_trace_writer_finalize(
              writer, &unchanged_view
          ) == MEISO_COMPOSITION_EDGE_TRACE_ORDER &&
          unchanged_view.data ==
              (const uint8_t *)(uintptr_t)UINT64_C(0x1234) &&
          unchanged_view.size == 99U);
    CHECK(meiso_composition_edge_trace_writer_record_set_wifi(
              writer, true, &wifi_desired
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    memmove(arena_copy, writer_arena, writer_size);
    memmove(buffer_copy, trace_bytes, TRACE_CAPACITY);
    CHECK(meiso_composition_edge_trace_writer_record_set_wifi(
              writer, true,
              (const MeisoWifiTelemetryDesiredParameters *)(const void *)
                  trace_bytes
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT &&
          memcmp(writer_arena, arena_copy, writer_size) == 0 &&
          memcmp(trace_bytes, buffer_copy, TRACE_CAPACITY) == 0);
    CHECK(meiso_composition_edge_trace_writer_record_activate(writer) ==
          MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_step(
              writer, (MeisoTimePoint){10U, 1000U}
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_set_render_media(
              writer, true, &render_desired
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_step(
              writer, (MeisoTimePoint){10U, 1200U}
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_frame(
              writer, &render_frame
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    render_frame.frame_sequence = 2U;
    render_frame.source_time.monotonic_ns = 1600U;
    CHECK(meiso_composition_edge_trace_writer_record_frame(
              writer, &render_frame
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_set_camera_media(
              writer, true, &camera_desired
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_step(
              writer, (MeisoTimePoint){10U, 1800U}
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    camera_frame = (MeisoCompositionEdgeSimulatorFrame){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA, 602U, 1U,
        {10U, 2000U}, {0U, 0U}, 0U, 0U, 0U, 0U, 0U,
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW, 320U, 240U,
        MEISO_COMPOSITION_MEDIA_FRAME_KEY,
        {camera_bytes, 320U * 240U * 3U / 2U}
    };
    CHECK(meiso_composition_edge_trace_writer_record_frame(
              writer, &camera_frame
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(meiso_composition_edge_trace_writer_record_terminate(
              writer, MEISO_EDGE_TERMINAL_LOCAL_ABORT
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    memmove(arena_copy, writer_arena, writer_size);
    memmove(buffer_copy, trace_bytes, TRACE_CAPACITY);
    CHECK(meiso_composition_edge_trace_writer_record_step(
              writer, (MeisoTimePoint){10U, 3000U}
          ) == MEISO_COMPOSITION_EDGE_TRACE_ORDER &&
          memcmp(writer_arena, arena_copy, writer_size) == 0 &&
          memcmp(trace_bytes, buffer_copy, TRACE_CAPACITY) == 0);
    CHECK(meiso_composition_edge_trace_writer_finalize(writer, &trace) ==
          MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(trace.data == trace_bytes && trace.size == EXPECTED_TRACE_BYTES &&
          memcmp(trace.data, "MECT", 4U) == 0 &&
          trace.data[4U] == 0U && trace.data[5U] == 1U &&
          get_u32(trace.data + 8U) == 1U &&
          get_u32(trace.data + 12U) == 12U &&
          get_u32(trace.data + 16U) == EXPECTED_TRACE_BYTES &&
          get_u32(trace.data + 20U) != 0U &&
          get_u32(trace.data + 24U) != 0U &&
          bytes_all(trace.data + 28U, 4U, 0U));
    for (i = 1U; i <= 12U; ++i) {
        offset = record_offset(trace.data, (uint32_t)i);
        CHECK(get_u32(trace.data + offset + 8U) == i &&
              trace.data[offset + 1U] == 0U &&
              trace.data[offset + 2U] == 0U &&
              trace.data[offset + 3U] ==
                  MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES &&
              bytes_all(trace.data + offset + 16U, 8U, 0U));
    }
    unchanged_view = (MeisoByteView){
        (const uint8_t *)(uintptr_t)UINT64_C(0x1234), 99U
    };
    CHECK(meiso_composition_edge_trace_writer_finalize(
              writer, &unchanged_view
          ) == MEISO_COMPOSITION_EDGE_TRACE_ORDER &&
          unchanged_view.data ==
              (const uint8_t *)(uintptr_t)UINT64_C(0x1234) &&
          unchanged_view.size == 99U);
    memmove(trace_copy, trace.data, trace.size);

    memset(reader_arena, 0xa5, reader_size + reader_alignment);
    reader = (MeisoCompositionEdgeTraceReader *)(uintptr_t)UINT64_C(0x1234);
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size - 1U, trace, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL &&
          bytes_all(reader_arena, reader_size + reader_alignment, 0xa5) &&
          reader ==
              (MeisoCompositionEdgeTraceReader *)(uintptr_t)UINT64_C(0x1234));
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena + 1U, reader_size, trace, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA);
    CHECK(meiso_composition_edge_trace_reader_init(
              trace_bytes, reader_size, trace, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT &&
          memcmp(trace_bytes, trace_copy, trace.size) == 0);

    memmove(mutated, trace.data, trace.size);
    mutated[record_offset(mutated, 2U) +
            MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES + 8U] ^= 1U;
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size,
              (MeisoByteView){mutated, trace.size}, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY);
    memmove(mutated, trace.data, trace.size);
    offset = record_offset(mutated, 1U);
    memset(mutated + offset +
               MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES,
           0, 8U);
    put_u32(
        mutated + offset + 12U,
        crc32c(
            mutated + offset +
                MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES,
            MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES
        )
    );
    refresh_trace_header(mutated, trace.size);
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size,
              (MeisoByteView){mutated, trace.size}, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG);
    memmove(mutated, trace.data, trace.size);
    offset = record_offset(mutated, 3U);
    mutated[offset] = MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP;
    refresh_trace_header(mutated, trace.size);
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size,
              (MeisoByteView){mutated, trace.size}, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_ORDER);
    memmove(mutated, trace.data, trace.size);
    offset = record_offset(mutated, 4U);
    mutated[offset] = 99U;
    refresh_trace_header(mutated, trace.size);
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size,
              (MeisoByteView){mutated, trace.size}, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_MALFORMED);
    memmove(mutated, trace.data, trace.size);
    offset = record_offset(mutated, 4U);
    mutated[offset + 1U] = 1U;
    refresh_trace_header(mutated, trace.size);
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size,
              (MeisoByteView){mutated, trace.size}, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_MALFORMED);

    memset(reader_arena, 0, reader_size);
    CHECK(meiso_composition_edge_trace_reader_init(
              reader_arena, reader_size, trace, &reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    memset(&decoded, 0xa5, sizeof(decoded));
    CHECK(meiso_composition_edge_trace_reader_simulator_config(
              reader, &decoded
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          config_fields_equal(&decoded, &config));
    memmove(arena_copy, reader_arena, reader_size);
    CHECK(meiso_composition_edge_trace_reader_simulator_config(
              reader,
              (MeisoCompositionEdgeSimulatorConfig *)(void *)reader_arena
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT &&
          memcmp(reader_arena, arena_copy, reader_size) == 0);

    memset(handle_arena, 0, handle_size);
    memset(wrong_handle_arena, 0, wrong_handle_size);
    CHECK(meiso_composition_edge_simulator_init(
              handle_arena, handle_size, &config, &handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    CHECK(meiso_composition_edge_simulator_init(
              wrong_handle_arena, wrong_handle_size, &wrong_config,
              &wrong_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    memset(&unchanged_result, 0xa5, sizeof(unchanged_result));
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, wrong_handle, &unchanged_result
          ) == MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED &&
          bytes_all(&unchanged_result, sizeof(unchanged_result), 0xa5));
    memmove(arena_copy, reader_arena, reader_size);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle,
              (MeisoCompositionEdgeTraceReplayResult *)(void *)reader_arena
          ) == MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT &&
          memcmp(reader_arena, arena_copy, reader_size) == 0);

    replay_status = meiso_composition_edge_trace_reader_replay_next(
        reader, handle, &result
    );
    CHECK(replay_status == MEISO_COMPOSITION_EDGE_TRACE_OK);
    CHECK(result.record_sequence == 2U &&
          result.record_kind ==
              MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI &&
          result.receipt_present && result.receipt.command_id == 1U &&
          !result.transition_present);
    CHECK(meiso_test_virtual_edge_bootstrap(handle));
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 3U &&
          result.record_kind ==
              MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE &&
          !result.receipt_present && !result.transition_present);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 4U && result.transition_present &&
          result.transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_WIFI_APPLIED);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) ==
              MEISO_EDGE_OK &&
          snapshot.edge.phase == MEISO_EDGE_PHASE_READY &&
          snapshot.edge.online && snapshot.edge.resource_graph_live &&
          snapshot.edge.continuity_serial == 9001U &&
          snapshot.edge.catalog_generation == 1U &&
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
          snapshot.wifi_effective_present && snapshot.wifi_sample_present &&
          snapshot.wifi_sample.signal_mbm == -5100 &&
          snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI]
                  .received_count == 1U);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 5U && result.receipt_present &&
          result.receipt.command_id == 2U);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 6U && result.transition_present &&
          result.transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_RENDER_MEDIA_APPLIED);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 7U &&
          meiso_composition_edge_media_latest(
              handle, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &media_frame
          ) == MEISO_EDGE_OK &&
          media_frame.key.frame_sequence == 1U &&
          media_frame.key.media_stream_epoch == 501U &&
          media_frame.applied_scene_state_version == 7U &&
          media_frame.active_plan_generation == 8U &&
          media_frame.world_generation == 9U &&
          media_frame.access_unit.size ==
              MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES &&
          memcmp(media_frame.access_unit.data, render_bytes,
                 media_frame.access_unit.size) == 0);
    memset(&unchanged_result, 0xa5, sizeof(unchanged_result));
    memmove(arena_copy, reader_arena, reader_size);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &unchanged_result
          ) == MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED &&
          bytes_all(&unchanged_result, sizeof(unchanged_result), 0xa5) &&
          memcmp(reader_arena, arena_copy, reader_size) == 0);
    CHECK(meiso_composition_edge_media_release(handle, &media_frame.key) ==
          MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 8U &&
          meiso_composition_edge_media_latest(
              handle, MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
              &media_frame
          ) == MEISO_EDGE_OK &&
          media_frame.key.frame_sequence == 2U &&
          media_frame.source_time.monotonic_ns == 1600U &&
          meiso_composition_edge_media_release(handle, &media_frame.key) ==
              MEISO_EDGE_OK);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 9U && result.receipt_present &&
          result.receipt.command_id == 3U);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 10U && result.transition_present &&
          result.transition ==
              MEISO_COMPOSITION_EDGE_SIMULATOR_CAMERA_MEDIA_APPLIED);
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 11U &&
          meiso_test_virtual_edge_camera_frame(
              handle, 9001U, camera_bytes, 320U * 240U * 3U / 2U
          ));
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 12U &&
          result.record_kind ==
              MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE &&
          meiso_test_virtual_edge_terminal(handle));
    memset(&unchanged_result, 0xa5, sizeof(unchanged_result));
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              reader, handle, &unchanged_result
          ) == MEISO_COMPOSITION_EDGE_TRACE_COMPLETE &&
          bytes_all(&unchanged_result, sizeof(unchanged_result), 0xa5));

    memmove(mutated, trace.data, trace.size);
    memset(tamper_reader_arena, 0, reader_size);
    CHECK(meiso_composition_edge_trace_reader_init(
              tamper_reader_arena, reader_size,
              (MeisoByteView){mutated, trace.size}, &tamper_reader
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK);
    memset(wrong_handle_arena, 0, handle_size);
    wrong_handle = NULL;
    CHECK(meiso_composition_edge_simulator_init(
              wrong_handle_arena, handle_size, &config, &wrong_handle
          ) == MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK);
    offset = record_offset(mutated, 2U) +
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES + 8U;
    mutated[offset] ^= 1U;
    memset(&unchanged_result, 0xa5, sizeof(unchanged_result));
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              tamper_reader, wrong_handle, &unchanged_result
          ) == MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY &&
          bytes_all(&unchanged_result, sizeof(unchanged_result), 0xa5));
    mutated[offset] ^= 1U;
    CHECK(meiso_composition_edge_trace_reader_replay_next(
              tamper_reader, wrong_handle, &result
          ) == MEISO_COMPOSITION_EDGE_TRACE_OK &&
          result.record_sequence == 2U);

    CHECK(memcmp(trace_bytes, trace_copy, trace.size) == 0);
    CHECK(meiso_composition_edge_snapshot(handle, &snapshot) ==
              MEISO_EDGE_OK &&
          snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL);
    puts("composition edge deterministic trace: PASS");
    free(camera_bytes);
    free(render_bytes);
    free(buffer_copy);
    free(arena_copy);
    free(mutated);
    free(trace_copy);
    free(small_trace);
    free(trace_bytes);
    free(wrong_handle_arena);
    free(handle_arena);
    free(tamper_reader_arena);
    free(reader_arena);
    free(writer_arena);
    return 0;

fail:
    free(camera_bytes);
    free(render_bytes);
    free(buffer_copy);
    free(arena_copy);
    free(mutated);
    free(trace_copy);
    free(small_trace);
    free(trace_bytes);
    free(wrong_handle_arena);
    free(handle_arena);
    free(tamper_reader_arena);
    free(reader_arena);
    free(writer_arena);
    return 1;
}
