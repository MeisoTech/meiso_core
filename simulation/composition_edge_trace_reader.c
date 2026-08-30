#include "meiso_composition_edge_trace_codec_private.h"

/* Trace reader owns immutable trace borrow validation and replay cursor state. */

#include <string.h>

static bool meiso_composition_edge_trace_private_reader_valid(const MeisoCompositionEdgeTraceReader *reader)
{
    return reader != NULL && reader->magic == MEISO_COMPOSITION_EDGE_TRACE_READER_MAGIC &&
        reader->arena_begin == (uintptr_t)reader &&
        reader->arena_end > reader->arena_begin &&
        reader->arena_end - reader->arena_begin == sizeof(*reader) &&
        reader->trace != NULL && reader->trace_begin == (uintptr_t)reader->trace &&
        reader->trace_end > reader->trace_begin &&
        reader->trace_end - reader->trace_begin == reader->trace_size &&
        reader->trace_size >= MEISO_COMPOSITION_EDGE_TRACE_MIN_BYTES &&
        reader->trace_size <= MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES &&
        reader->cursor <= reader->trace_size && reader->record_count >= 3U &&
        reader->record_count <= MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS &&
        reader->next_sequence >= 2U &&
        reader->next_sequence <= reader->record_count + 1U &&
        !meiso_composition_edge_trace_private_ranges_overlap(reader->arena_begin, reader->arena_end,
                              reader->trace_begin, reader->trace_end);
}

static bool meiso_composition_edge_trace_private_header_valid(
    const uint8_t *trace,
    size_t trace_size,
    uint32_t *out_record_count
)
{
    uint8_t header[MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES];
    uint32_t stored_header_crc;
    uint32_t stored_body_crc;
    uint32_t record_count;

    if (trace == NULL || out_record_count == NULL ||
        trace_size < MEISO_COMPOSITION_EDGE_TRACE_MIN_BYTES ||
        trace_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES ||
        memcmp(trace, "MECT", 4U) != 0 ||
        meiso_composition_edge_trace_private_get_u16(trace + 4U) != MEISO_COMPOSITION_EDGE_TRACE_VERSION ||
        meiso_composition_edge_trace_private_get_u16(trace + 6U) !=
            MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES ||
        meiso_composition_edge_trace_private_get_u32(trace + 8U) != MEISO_COMPOSITION_EDGE_TRACE_FLAG_SYNTHETIC ||
        meiso_composition_edge_trace_private_get_u32(trace + 16U) != trace_size ||
        !meiso_composition_edge_trace_private_bytes_zero(trace + 28U, 4U)) {
        return false;
    }
    record_count = meiso_composition_edge_trace_private_get_u32(trace + 12U);
    if (record_count < 3U ||
        record_count > MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS) {
        return false;
    }
    stored_body_crc = meiso_composition_edge_trace_private_get_u32(trace + 20U);
    if (stored_body_crc != meiso_composition_edge_trace_private_crc32c(
            trace + MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES,
            trace_size - MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES
        )) {
        return false;
    }
    memmove(header, trace, sizeof(header));
    stored_header_crc = meiso_composition_edge_trace_private_get_u32(header + 24U);
    memset(header + 24U, 0, 4U);
    if (stored_header_crc != meiso_composition_edge_trace_private_crc32c(header, sizeof(header))) {
        return false;
    }
    *out_record_count = record_count;
    return true;
}

static MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_private_record_at(
    const uint8_t *trace,
    size_t trace_size,
    size_t cursor,
    uint32_t expected_sequence,
    MeisoCompositionEdgeTraceRecordKind *out_kind,
    const uint8_t **out_payload,
    size_t *out_payload_size,
    size_t *out_next_cursor
)
{
    const uint8_t *header;
    const uint8_t *payload;
    size_t payload_size;
    size_t next_cursor;

    if (trace == NULL || out_kind == NULL || out_payload == NULL ||
        out_payload_size == NULL || out_next_cursor == NULL ||
        cursor > trace_size ||
        trace_size - cursor <
            MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES) {
        return MEISO_COMPOSITION_EDGE_TRACE_TRUNCATED;
    }
    header = trace + cursor;
    if (header[1U] != 0U ||
        meiso_composition_edge_trace_private_get_u16(header + 2U) !=
            MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES ||
        meiso_composition_edge_trace_private_get_u32(header + 8U) != expected_sequence ||
        !meiso_composition_edge_trace_private_bytes_zero(header + 16U, 8U)) {
        return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
    }
    payload_size = meiso_composition_edge_trace_private_get_u32(header + 4U);
    if (payload_size > MEISO_COMPOSITION_EDGE_TRACE_MAX_FRAME_PAYLOAD ||
        !meiso_composition_edge_trace_private_checked_add(
            cursor + MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES,
            payload_size, &next_cursor
        ) || next_cursor > trace_size) {
        return MEISO_COMPOSITION_EDGE_TRACE_TRUNCATED;
    }
    payload = header + MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES;
    if (meiso_composition_edge_trace_private_get_u32(header + 12U) !=
        meiso_composition_edge_trace_private_crc32c(payload, payload_size)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    if (header[0U] < MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG ||
        header[0U] > MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE) {
        return MEISO_COMPOSITION_EDGE_TRACE_MALFORMED;
    }
    *out_kind = header[0U];
    *out_payload = payload;
    *out_payload_size = payload_size;
    *out_next_cursor = next_cursor;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

size_t meiso_composition_edge_trace_reader_alignment_required(void)
{
    return _Alignof(MeisoCompositionEdgeTraceReader);
}

size_t meiso_composition_edge_trace_reader_memory_required(void)
{
    return sizeof(MeisoCompositionEdgeTraceReader);
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_reader_init(
    void *arena,
    size_t arena_size,
    MeisoByteView trace,
    MeisoCompositionEdgeTraceReader **out_reader
)
{
    MeisoCompositionEdgeTraceReader candidate;
    MeisoCompositionEdgeSimulatorConfig config;
    MeisoCompositionEdgeTraceSemanticState semantic;
    MeisoCompositionEdgeTraceRecordKind kind;
    const uint8_t *payload;
    size_t payload_size;
    size_t cursor;
    size_t next_cursor;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t trace_begin;
    uintptr_t trace_end;
    uintptr_t result_begin;
    uintptr_t result_end;
    uint32_t record_count;
    uint32_t sequence;
    MeisoCompositionEdgeTraceStatus status;

    if (arena == NULL || out_reader == NULL ||
        !meiso_byte_view_valid(trace) || trace.size < MEISO_COMPOSITION_EDGE_TRACE_MIN_BYTES ||
        trace.size > MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES ||
        !meiso_composition_edge_trace_private_checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !meiso_composition_edge_trace_private_checked_range(trace.data, trace.size, &trace_begin, &trace_end) ||
        !meiso_composition_edge_trace_private_checked_range(out_reader, sizeof(*out_reader),
                             &result_begin, &result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if ((arena_begin % _Alignof(MeisoCompositionEdgeTraceReader)) != 0U) {
        return MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(MeisoCompositionEdgeTraceReader)) {
        return MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL;
    }
    arena_end = arena_begin + sizeof(MeisoCompositionEdgeTraceReader);
    if (meiso_composition_edge_trace_private_ranges_overlap(arena_begin, arena_end, trace_begin, trace_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(arena_begin, arena_end,
                             result_begin, result_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(trace_begin, trace_end,
                             result_begin, result_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_edge_trace_private_header_valid(trace.data, trace.size, &record_count)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    cursor = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES;
    status = meiso_composition_edge_trace_private_record_at(
        trace.data, trace.size, cursor, 1U, &kind, &payload,
        &payload_size, &next_cursor
    );
    if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
        return status;
    }
    if (kind != MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG ||
        payload_size != MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES ||
        !meiso_composition_edge_trace_private_decode_config(payload, payload_size, &config)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG;
    }
    cursor = next_cursor;
    meiso_composition_edge_trace_private_semantic_init(&semantic, &config);
    for (sequence = 2U; sequence <= record_count; ++sequence) {
        status = meiso_composition_edge_trace_private_record_at(
            trace.data, trace.size, cursor, sequence, &kind, &payload,
            &payload_size, &next_cursor
        );
        if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
            return status;
        }
        status = meiso_composition_edge_trace_private_semantic_apply(
            &semantic, kind, payload, payload_size, NULL
        );
        if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
            return status;
        }
        if (semantic.terminated && sequence != record_count) {
            return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
        }
        cursor = next_cursor;
    }
    if (cursor != trace.size || !semantic.activated ||
        !semantic.terminated) {
        return MEISO_COMPOSITION_EDGE_TRACE_ORDER;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.magic = MEISO_COMPOSITION_EDGE_TRACE_READER_MAGIC;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = arena_end;
    candidate.trace_begin = trace_begin;
    candidate.trace_end = trace_end;
    candidate.trace = trace.data;
    candidate.trace_size = trace.size;
    candidate.cursor = MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +
        MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES;
    candidate.record_count = record_count;
    candidate.next_sequence = 2U;
    candidate.config = config;
    memset(arena, 0, sizeof(candidate));
    memmove(arena, &candidate, sizeof(candidate));
    *out_reader = (MeisoCompositionEdgeTraceReader *)arena;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_reader_simulator_config(
    const MeisoCompositionEdgeTraceReader *reader,
    MeisoCompositionEdgeSimulatorConfig *out_config
)
{
    uintptr_t output_begin;
    uintptr_t output_end;
    uint32_t record_count;

    if (!meiso_composition_edge_trace_private_reader_valid(reader) || out_config == NULL ||
        !meiso_composition_edge_trace_private_checked_range(out_config, sizeof(*out_config),
                             &output_begin, &output_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(output_begin, output_end,
                             reader->arena_begin, reader->arena_end) ||
        meiso_composition_edge_trace_private_ranges_overlap(output_begin, output_end,
                             reader->trace_begin, reader->trace_end)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_edge_trace_private_header_valid(reader->trace, reader->trace_size, &record_count) ||
        record_count != reader->record_count) {
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    *out_config = reader->config;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}

static bool meiso_composition_edge_trace_private_replay_ranges_valid(
    const MeisoCompositionEdgeTraceReader *reader,
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeTraceReplayResult *out_result
)
{
    uintptr_t output_begin;
    uintptr_t output_end;

    return reader != NULL && handle != NULL && out_result != NULL &&
        meiso_composition_edge_trace_private_checked_range(out_result, sizeof(*out_result),
                            &output_begin, &output_end) &&
        !meiso_composition_edge_trace_private_ranges_overlap(output_begin, output_end,
                              reader->arena_begin, reader->arena_end) &&
        !meiso_composition_edge_trace_private_ranges_overlap(output_begin, output_end,
                              reader->trace_begin, reader->trace_end) &&
        meiso_composition_edge_internal_simulator_storage_disjoint(
            handle, reader, sizeof(*reader)
        ) &&
        meiso_composition_edge_internal_simulator_storage_disjoint(
            handle, reader->trace, reader->trace_size
        ) &&
        meiso_composition_edge_internal_simulator_storage_disjoint(
            handle, out_result, sizeof(*out_result)
        );
}

MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_reader_replay_next(
    MeisoCompositionEdgeTraceReader *reader,
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeTraceReplayResult *out_result
)
{
    MeisoCompositionEdgeTraceReplayResult result;
    MeisoCompositionEdgeTraceRecordKind kind;
    const uint8_t *payload;
    size_t payload_size;
    size_t next_cursor;
    bool enabled;
    MeisoWifiTelemetryDesiredParameters wifi;
    MeisoCompositionMediaDesired media;
    MeisoCompositionEdgeSimulatorFrame frame;
    MeisoEdgeCommandReceipt receipt;
    MeisoCompositionEdgeSimulatorTransition transition;
    MeisoEdgeStatus edge_status;
    MeisoCompositionEdgeTraceStatus status;

    if (!meiso_composition_edge_trace_private_reader_valid(reader) ||
        !meiso_composition_edge_trace_private_replay_ranges_valid(reader, handle, out_result)) {
        return MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT;
    }
    if (reader->cursor == reader->trace_size) {
        return MEISO_COMPOSITION_EDGE_TRACE_COMPLETE;
    }
    if ((reader->bound_handle == 0U &&
         !meiso_composition_edge_internal_simulator_fresh_matches(
             handle, &reader->config
         )) ||
        (reader->bound_handle != 0U &&
         reader->bound_handle != (uintptr_t)handle)) {
        return MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED;
    }
    status = meiso_composition_edge_trace_private_record_at(
        reader->trace, reader->trace_size, reader->cursor,
        reader->next_sequence, &kind, &payload, &payload_size, &next_cursor
    );
    if (status != MEISO_COMPOSITION_EDGE_TRACE_OK) {
        return status;
    }
    memset(&result, 0, sizeof(result));
    result.record_sequence = reader->next_sequence;
    result.record_kind = kind;
    switch (kind) {
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE:
        edge_status = meiso_composition_edge_simulator_activate(handle);
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI:
        if (!meiso_composition_edge_trace_private_decode_wifi_record(payload, payload_size, &enabled, &wifi)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = meiso_composition_edge_set_wifi(
            handle, enabled, enabled ? &wifi : NULL, &receipt
        );
        if (edge_status == MEISO_EDGE_OK) {
            result.receipt_present = true;
            result.receipt = receipt;
        }
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA:
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_CAMERA_MEDIA:
        if (!meiso_composition_edge_trace_private_decode_media_record(payload, payload_size,
                                       &enabled, &media)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = kind ==
                MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA ?
            meiso_composition_edge_set_render_media(
                handle, enabled, enabled ? &media : NULL, &receipt
            ) :
            meiso_composition_edge_set_camera_media(
                handle, enabled, enabled ? &media : NULL, &receipt
            );
        if (edge_status == MEISO_EDGE_OK) {
            result.receipt_present = true;
            result.receipt = receipt;
        }
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP:
        if (payload_size != 16U) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = meiso_composition_edge_simulator_step(
            handle,
            (MeisoTimePoint){meiso_composition_edge_trace_private_get_u64(payload),
                             meiso_composition_edge_trace_private_get_u64(payload + 8U)},
            &transition
        );
        if (edge_status == MEISO_EDGE_OK) {
            result.transition_present = true;
            result.transition = transition;
        }
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME:
        if (!meiso_composition_edge_trace_private_decode_frame_record(payload, payload_size, NULL, &frame)) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status =
            meiso_composition_edge_simulator_publish_frame(handle, &frame);
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE:
        if (payload_size != 8U) {
            return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
        }
        edge_status = meiso_composition_edge_simulator_terminate(
            handle, payload[0]
        );
        break;
    case MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG:
    default:
        return MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY;
    }
    if (edge_status != MEISO_EDGE_OK) {
        return MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED;
    }
    reader->bound_handle = (uintptr_t)handle;
    reader->cursor = next_cursor;
    ++reader->next_sequence;
    *out_result = result;
    return MEISO_COMPOSITION_EDGE_TRACE_OK;
}
