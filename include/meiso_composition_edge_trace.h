#ifndef MEISO_COMPOSITION_EDGE_TRACE_H
#define MEISO_COMPOSITION_EDGE_TRACE_H

#include "meiso_composition_edge_simulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_EDGE_TRACE_VERSION 1U
#define MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES 32U
#define MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES 24U
#define MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES 384U
#define MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES 112U
#define MEISO_COMPOSITION_EDGE_TRACE_MAX_BYTES 16777216U
#define MEISO_COMPOSITION_EDGE_TRACE_MAX_RECORDS 4096U

typedef uint8_t MeisoCompositionEdgeTraceStatus;
enum {
    MEISO_COMPOSITION_EDGE_TRACE_OK = 0,
    MEISO_COMPOSITION_EDGE_TRACE_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_EDGE_TRACE_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_EDGE_TRACE_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_EDGE_TRACE_ARENA_TOO_SMALL = 4,
    MEISO_COMPOSITION_EDGE_TRACE_CAPACITY = 5,
    MEISO_COMPOSITION_EDGE_TRACE_TRUNCATED = 6,
    MEISO_COMPOSITION_EDGE_TRACE_MALFORMED = 7,
    MEISO_COMPOSITION_EDGE_TRACE_INTEGRITY = 8,
    MEISO_COMPOSITION_EDGE_TRACE_ORDER = 9,
    MEISO_COMPOSITION_EDGE_TRACE_COMPLETE = 10,
    MEISO_COMPOSITION_EDGE_TRACE_REPLAY_REJECTED = 11,
    MEISO_COMPOSITION_EDGE_TRACE_INTERNAL_FAILURE = 12
};

typedef uint8_t MeisoCompositionEdgeTraceRecordKind;
enum {
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_CONFIG = 1,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_ACTIVATE = 2,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_WIFI = 3,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_RENDER_MEDIA = 4,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_SET_CAMERA_MEDIA = 5,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_STEP = 6,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_FRAME = 7,
    MEISO_COMPOSITION_EDGE_TRACE_RECORD_TERMINATE = 8
};

typedef struct MeisoCompositionEdgeTraceWriter
    MeisoCompositionEdgeTraceWriter;
typedef struct MeisoCompositionEdgeTraceReader
    MeisoCompositionEdgeTraceReader;

typedef struct {
    uint32_t record_sequence;
    MeisoCompositionEdgeTraceRecordKind record_kind;
    bool receipt_present;
    MeisoEdgeCommandReceipt receipt;
    bool transition_present;
    MeisoCompositionEdgeSimulatorTransition transition;
} MeisoCompositionEdgeTraceReplayResult;

size_t meiso_composition_edge_trace_writer_alignment_required(void);
size_t meiso_composition_edge_trace_writer_memory_required(void);

/*
 * Writer arena, full output capacity, config, out_writer and every later
 * typed input/output are mutually disjoint. The writer owns the full output
 * capacity until finalize and never allocates. Every failure is byte-exact.
 */
MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_init(
    void *arena,
    size_t arena_size,
    MeisoMutableBuffer output,
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoCompositionEdgeTraceWriter **out_writer
);

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_activate(
    MeisoCompositionEdgeTraceWriter *writer
);
MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_wifi(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters
);
MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_render_media(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
);
MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_set_camera_media(
    MeisoCompositionEdgeTraceWriter *writer,
    bool enabled,
    const MeisoCompositionMediaDesired *desired
);
MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_record_step(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoTimePoint now
);
MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_frame(
    MeisoCompositionEdgeTraceWriter *writer,
    const MeisoCompositionEdgeSimulatorFrame *frame
);
MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_writer_record_terminate(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoEdgeTerminalReason reason
);

/* Finalize requires one ACTIVATE and a final TERMINATE. */
MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_writer_finalize(
    MeisoCompositionEdgeTraceWriter *writer,
    MeisoByteView *out_trace
);

size_t meiso_composition_edge_trace_reader_alignment_required(void);
size_t meiso_composition_edge_trace_reader_memory_required(void);

/*
 * Init validates the complete immutable trace before committing reader arena.
 * Trace bytes remain borrowed and disjoint for the reader lifetime.
 */
MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_reader_init(
    void *arena,
    size_t arena_size,
    MeisoByteView trace,
    MeisoCompositionEdgeTraceReader **out_reader
);

MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_reader_simulator_config(
    const MeisoCompositionEdgeTraceReader *reader,
    MeisoCompositionEdgeSimulatorConfig *out_config
);

/*
 * The first call binds an exact fresh simulator created from reader config.
 * Until COMPLETE, only this function may submit/activate/step/publish/
 * terminate that handle. Snapshot and exact media latest/release are allowed.
 * Rejection leaves reader cursor and out_result byte-exact and is retryable.
 */
MeisoCompositionEdgeTraceStatus meiso_composition_edge_trace_reader_replay_next(
    MeisoCompositionEdgeTraceReader *reader,
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeTraceReplayResult *out_result
);

#ifdef __cplusplus
}
#endif

#endif
