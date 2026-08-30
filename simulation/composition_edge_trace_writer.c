#include "meiso_composition_edge_trace_codec_private.h"

/* Trace writer owns its caller buffer until one terminal finalize boundary. */

#include <string.h>

static void meiso_composition_edge_trace_private_write_record_header(
    uint8_t output[MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES],
    MeisoCompositionEdgeTraceRecordKind kind,
    uint32_t payload_size,
    uint32_t sequence,
    uint32_t payload_crc
)
{
    memset(output, 0, MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES);
    output[0] = kind;
    meiso_composition_edge_trace_private_put_u16(output + 2U,
                  MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES);
    meiso_composition_edge_trace_private_put_u32(output + 4U, payload_size);
    meiso_composition_edge_trace_private_put_u32(output + 8U, sequence);
    meiso_composition_edge_trace_private_put_u32(output + 12U, payload_crc);
}

static bool meiso_composition_edge_trace_private_writer_valid(
    const MeisoCompositionEdgeTraceWriter *writer
)
{
    return writer != NULL && writer->magic == MEISO_COMPOSITION_EDGE_TRACE_WRITER_MAGIC &&
        writer->arena_begin == (uintptr_t)writer &&
        writer->arena_end > writer->arena_begin &&
        writer->arena_end - writer->arena_begin == sizeof(*writer) &&
        writer->output != NULL &&
        writer->output_begin == (uintptr_t)writer->output &&
        writer->output_end > writer->output_begin &&
        writer->output_end - writer->output_begin == writer->capacity &&
        writer->capacity >= MEISO_COMPOSITION_EDGE_TRACE_MIN_BYTES &&
        writer->capacity <= MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES &&
        writer->size >= MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
                MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
                MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES &&
        writer->size <= writer->capacity &&
        writer->record_count >= 1U &&
        writer->record_count <= MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS &&
        !meiso_composition_edge_trace_private_ranges_overlap(writer->arena_begin, writer->arena_end,
                              writer->output_begin, writer->output_end);
}

static bool meiso_composition_edge_trace_private_writer_input_disjoint(
    const MeisoCompositionEdgeTraceWriter *writer,
    const void *input,
    size_t input_size
)
{
    uintptr_t begin;
    uintptr_t end;

    if (input == NULL && input_size == 0U) {
        return true;
    }
    return meiso_composition_edge_trace_private_checked_range(input, input_size, &begin, &end) &&
        !meiso_composition_edge_trace_private_ranges_overlap(begin, end, writer->arena_begin,
                              writer->arena_end) &&
        !meiso_composition_edge_trace_private_ranges_overlap(begin, end, writer->output_begin,
                              writer->output_end);
}

static MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_private_writer_append(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoCompositionEdgeTraceRecordKind kind,
    const uint8_t *prefix,
    size_t prefix_size,
    const uint8_t *suffix,
    size_t suffix_size
)
{
    MeisoCompositionEdgeTraceSemanticState semantic;
    uint8_t header[MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES];
    size_t payload_size;
    size_t record_size;
    size_t next_size;
    uint32_t sequence;
    uint32_t payload_crc;
    MeisoCompositionEdgeTraceStatus status;

    if (!meiso_composition_edge_trace_private_writer_valid(writer) || writer->finalized ||
        (prefix_size != 0U && prefix == NULL) ||
        (suffix_size != 0U && suffix == NULL) ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(writer, prefix, prefix_size) ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(writer, suffix, suffix_size) ||
        !meiso_composition_edge_trace_private_range_disjoint(prefix, prefix_size, suffix, suffix_size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_edge_trace_private_checked_add(prefix_size, suffix_size, &payload_size) ||
        payload_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_FRAME_PAYLOAD ||
        payload_size > UINT32_MAX ||
        !meiso_composition_edge_trace_private_checked_add(MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES,
                           payload_size, &record_size) ||
        !meiso_composition_edge_trace_private_checked_add(writer->size, record_size, &next_size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_CAPACITY;
    }
    if (writer->record_count == MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS ||
        next_size > writer->capacity ||
        next_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES) {
        return MEISO_COMPOSITION_EDGE_TRACE_CAPACITY;
    }
    semantic = writer->semantic;
    status = meiso_composition_edge_trace_private_semantic_apply(
        &semantic, kind, prefix, payload_size,
        kind == MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME ? suffix : NULL
    );
    if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
        return status;
    }
    sequence = writer->record_count + 1U;
    payload_crc = meiso_composition_edge_trace_private_crc32c_pair(prefix, prefix_size, suffix, suffix_size);
    meiso_composition_edge_trace_private_write_record_header(header, kind, (uint32_t)payload_size, sequence,
                              payload_crc);
    memmove(writer->output + writer->size, header, sizeof(header));
    if (prefix_size != 0U) {
        memmove(writer->output + writer->size + sizeof(header),
                prefix, prefix_size);
    }
    if (suffix_size != 0U) {
        memmove(writer->output + writer->size + sizeof(header) + prefix_size,
                suffix, suffix_size);
    }
    writer->semantic = semantic;
    writer->size = next_size;
    writer->record_count = sequence;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

size_t meiso_composition_edge_trace_writer_alignment_required(void)
{
    return _Alignof(MeisoCompositionEdgeTraceWriter);
}

size_t meiso_composition_edge_trace_writer_memory_required(void)
{
    return sizeof(MeisoCompositionEdgeTraceWriter);
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_init(
    void *arena,
    size_t arena_size,
    MeisoMutableBuffer output,
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoCompositionEdgeTraceWriter **out_writer
)
{
    MeisoCompositionEdgeTraceWriter candidate;
    uint8_t config_bytes[MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES];
    uint8_t record_header[
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES
    ];
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    uintptr_t config_begin;
    uintptr_t config_end;
    uintptr_t result_begin;
    uintptr_t result_end;
    size_t initial_size = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES;

    if (arena == NULL || output.data == NULL || config == NULL ||
        out_writer == NULL || output.capacity < MEISO_COMPOSITION_EDGE_TRACE_MIN_BYTES ||
        output.capacity > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES ||
        !meiso_composition_edge_trace_private_checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !meiso_composition_edge_trace_private_checked_range(output.data, output.capacity,
                             &output_begin, &output_end) ||
        !meiso_composition_edge_trace_private_checked_range(config, sizeof(*config),
                             &config_begin, &config_end) ||
        !meiso_composition_edge_trace_private_checked_range(out_writer, sizeof(*out_writer),
                             &result_begin, &result_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(config_begin, config_end,
                             result_begin, result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if ((arena_begin % _Alignof(MeisoCompositionEdgeTraceWriter)) != 0U) {
        return MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(MeisoCompositionEdgeTraceWriter)) {
        return MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL;
    }
    arena_end = arena_begin + sizeof(MeisoCompositionEdgeTraceWriter);
    if (meiso_composition_edge_trace_private_ranges_overlap(arena_begin, arena_end,
                             output_begin, output_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(arena_begin, arena_end,
                             config_begin, config_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(arena_begin, arena_end,
                             result_begin, result_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(output_begin, output_end,
                             config_begin, config_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(output_begin, output_end,
                             result_begin, result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_edge_trace_private_encode_config(config, config_bytes)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.magic = MEISO_COMPOSITION_EDGE_TRACE_WRITER_MAGIC;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = arena_end;
    candidate.output_begin = output_begin;
    candidate.output_end = output_end;
    candidate.output = output.data;
    candidate.capacity = output.capacity;
    candidate.size = initial_size;
    candidate.record_count = 1U;
    meiso_composition_edge_trace_private_semantic_init(&candidate.semantic, config);
    meiso_composition_edge_trace_private_write_record_header(
        record_header, MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG,
        MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES, 1U,
        meiso_composition_edge_trace_private_crc32c(config_bytes, sizeof(config_bytes))
    );
    memset(output.data, 0, MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES);
    memmove(output.data + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES,
            record_header, sizeof(record_header));
    memmove(output.data + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
                sizeof(record_header),
            config_bytes, sizeof(config_bytes));
    memset(arena, 0, sizeof(candidate));
    memmove(arena, &candidate, sizeof(candidate));
    *out_writer = (MeisoCompositionEdgeTraceWriter *)arena;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_activate(
    MeisoCompositionEdgeTraceWriter *writer
)
{
    return meiso_composition_edge_trace_private_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE,
        NULL, 0U, NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_wifi(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters
)
{
    uint8_t payload[16U];
    size_t encoded_size = 0U;

    if (!meiso_composition_edge_trace_private_writer_valid(writer) ||
        (enabled && parameters == NULL) ||
        (!enabled && parameters != NULL) ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(
            writer, parameters, parameters == NULL ? 0U : sizeof(*parameters)
        )) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    memset(payload, 0, sizeof(payload));
    payload[0] = enabled ? 1U : 0U;
    if (enabled &&
        (meiso_wifi_telemetry_desired_parameters_encode(
             parameters,
             (MeisoMutableBuffer){
                 payload + 8U,
                 MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
             },
             &encoded_size
         ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
         encoded_size !=
             MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    return meiso_composition_edge_trace_private_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI,
        payload, sizeof(payload), NULL, 0U
    );
}

static MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_private_writer_record_media(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoCompositionEdgeTraceRecordKind kind,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
)
{
    uint8_t payload[40U];
    size_t encoded_size = 0U;

    if (!meiso_composition_edge_trace_private_writer_valid(writer) ||
        (enabled && desired == NULL) || (!enabled && desired != NULL) ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(
            writer, desired, desired == NULL ? 0U : sizeof(*desired)
        )) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    memset(payload, 0, sizeof(payload));
    payload[0] = enabled ? 1U : 0U;
    if (enabled &&
        (meiso_composition_media_desired_encode(
             desired,
             (MeisoMutableBuffer){
                 payload + 8U, MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE
             },
             &encoded_size
         ) != MEISO_COMPOSITION_PROFILE_OK ||
         encoded_size != MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    return meiso_composition_edge_trace_private_writer_append(
        writer, kind, payload, sizeof(payload), NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_render_media(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
)
{
    return meiso_composition_edge_trace_private_writer_record_media(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA,
        enabled, desired
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_camera_media(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
)
{
    return meiso_composition_edge_trace_private_writer_record_media(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_CAMERA_MEDIA,
        enabled, desired
    );
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_record_step(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoTimePoint now
)
{
    uint8_t payload[16U];

    if (!meiso_composition_edge_trace_private_writer_valid(writer) || !meiso_time_point_valid(now)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    meiso_composition_edge_trace_private_put_u64(payload, now.clock_epoch);
    meiso_composition_edge_trace_private_put_u64(payload + 8U, now.monotonic_ns);
    return meiso_composition_edge_trace_private_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP,
        payload, sizeof(payload), NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_frame(
    MeisoCompositionEdgeTraceWriter *writer,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    uint8_t prefix[MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES];

    if (!meiso_composition_edge_trace_private_writer_valid(writer) || frame == NULL ||
        !meiso_byte_view_valid(frame->access_unit) ||
        frame->access_unit.size == 0U ||
        frame->access_unit.size > UINT32_MAX ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(writer, frame, sizeof(*frame)) ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(writer, frame->access_unit.data,
                                     frame->access_unit.size) ||
        !meiso_composition_edge_trace_private_range_disjoint(frame, sizeof(*frame), frame->access_unit.data,
                              frame->access_unit.size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    memset(prefix, 0, sizeof(prefix));
    prefix[0] = frame->source;
    meiso_composition_edge_trace_private_put_u16(prefix + 2U, frame->flags);
    meiso_composition_edge_trace_private_put_u16(prefix + 4U, frame->pixel_format);
    meiso_composition_edge_trace_private_put_u16(prefix + 6U, frame->codec);
    meiso_composition_edge_trace_private_put_u16(prefix + 8U, frame->width);
    meiso_composition_edge_trace_private_put_u16(prefix + 10U, frame->height);
    meiso_composition_edge_trace_private_put_u64(prefix + 16U, frame->media_stream_epoch);
    meiso_composition_edge_trace_private_put_u64(prefix + 24U, frame->frame_sequence);
    meiso_composition_edge_trace_private_put_u64(prefix + 32U, frame->source_time.clock_epoch);
    meiso_composition_edge_trace_private_put_u64(prefix + 40U, frame->source_time.monotonic_ns);
    prefix[48U] = frame->applied_scene_owner.role;
    meiso_composition_edge_trace_private_put_u64(prefix + 56U, frame->applied_scene_owner.incarnation);
    meiso_composition_edge_trace_private_put_u64(prefix + 64U, frame->applied_scene_state_version);
    meiso_composition_edge_trace_private_put_u64(prefix + 72U, frame->active_plan_generation);
    meiso_composition_edge_trace_private_put_u64(prefix + 80U, frame->world_generation);
    meiso_composition_edge_trace_private_put_u64(prefix + 88U, frame->dependency_sequence);
    meiso_composition_edge_trace_private_put_u64(prefix + 96U, frame->producer_dropped_frames);
    meiso_composition_edge_trace_private_put_u32(prefix + 104U, (uint32_t)frame->access_unit.size);
    return meiso_composition_edge_trace_private_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME,
        prefix, sizeof(prefix), frame->access_unit.data,
        frame->access_unit.size
    );
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_terminate(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoEdgeTerminalReason reason
)
{
    uint8_t payload[8U] = {0U};

    if (!meiso_composition_edge_trace_private_writer_valid(writer) || reason == MEISO_EDGE_TERMINAL_NONE ||
        reason > MEISO_EDGE_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    payload[0] = reason;
    return meiso_composition_edge_trace_private_writer_append(
        writer, MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE,
        payload, sizeof(payload), NULL, 0U
    );
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_finalize(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoByteView *out_trace
)
{
    uint8_t header[MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES];

    if (!meiso_composition_edge_trace_private_writer_valid(writer) || out_trace == NULL ||
        !meiso_composition_edge_trace_private_writer_input_disjoint(writer, out_trace, sizeof(*out_trace))) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (writer->finalized || !writer->semantic.activated ||
        !writer->semantic.terminated || writer->record_count < 3U ||
        writer->size > UINT32_MAX) {
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
    memset(header, 0, sizeof(header));
    memmove(header, "MECT", 4U);
    meiso_composition_edge_trace_private_put_u16(header + 4U, MEISO_COMPOSITION_EDGE_TRACE_VERSION);
    meiso_composition_edge_trace_private_put_u16(header + 6U, MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES);
    meiso_composition_edge_trace_private_put_u32(header + 8U, MEISO_COMPOSITION_EDGE_TRACE_FLAG_SYNTHETIC);
    meiso_composition_edge_trace_private_put_u32(header + 12U, writer->record_count);
    meiso_composition_edge_trace_private_put_u32(header + 16U, (uint32_t)writer->size);
    meiso_composition_edge_trace_private_put_u32(
        header + 20U,
        meiso_composition_edge_trace_private_crc32c(
            writer->output + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES,
            writer->size - MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES
        )
    );
    meiso_composition_edge_trace_private_put_u32(header + 24U, meiso_composition_edge_trace_private_crc32c(header, sizeof(header)));
    memmove(writer->output, header, sizeof(header));
    writer->finalized = true;
    *out_trace = (MeisoByteView){writer->output, writer->size};
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}
