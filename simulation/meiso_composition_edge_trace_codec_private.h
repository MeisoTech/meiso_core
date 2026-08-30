#ifndef MEISO_COMPOSITION_EDGE_TRACE_CODEC_PRIVATE_H
#define MEISO_COMPOSITION_EDGE_TRACE_CODEC_PRIVATE_H

/* Build-private/non-installed; reader borrows bytes, writer owns its output loan. */

#include "meiso_composition_edge_trace_private.h"

#include <meiso/simulation/composition_trace.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEISO_COMPOSITION_EDGE_TRACE_WRITER_MAGIC \
    UINT64_C(0x4d45435457525431)
#define MEISO_COMPOSITION_EDGE_TRACE_READER_MAGIC \
    UINT64_C(0x4d45435452445231)
#define MEISO_COMPOSITION_EDGE_TRACE_FLAG_SYNTHETIC UINT32_C(1)
#define MEISO_COMPOSITION_EDGE_TRACE_MIN_BYTES                              \
    (MEISO_COMPOSITION_EDGE_TRACE_HEADER_BYTES +                            \
     MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +                    \
     MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES +                           \
     MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES +                    \
     MEISO_COMPOSITION_EDGE_TRACE_RECORD_HEADER_BYTES + 8U)
#define MEISO_COMPOSITION_EDGE_TRACE_MAX_FRAME_PAYLOAD                     \
    (MEISO_COMPOSITION_EDGE_TRACE_FRAME_PREFIX_BYTES +                     \
     MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES)

typedef struct {
    bool pending;
    bool pending_enabled;
    MeisoCompositionMediaDesired pending_desired;
    bool active;
    MeisoCompositionMediaDesired active_desired;
    uint64_t active_epoch;
    uint64_t next_epoch;
    bool epoch_available;
    uint64_t sequence_high_water;
    uint64_t drop_high_water;
} MeisoCompositionEdgeTraceMediaState;

typedef struct {
    MeisoCompositionEdgeSimulatorConfig config;
    bool activated;
    bool terminated;
    bool wifi_pending;
    bool wifi_pending_enabled;
    MeisoWifiTelemetryDesiredParameters wifi_pending_desired;
    uint64_t next_wifi_epoch;
    bool wifi_epoch_available;
    MeisoCompositionEdgeTraceMediaState
        media[MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT];
} MeisoCompositionEdgeTraceSemanticState;

struct MeisoCompositionEdgeTraceWriter {
    uint64_t magic;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    uint8_t *output;
    size_t capacity;
    size_t size;
    uint32_t record_count;
    bool finalized;
    MeisoCompositionEdgeTraceSemanticState semantic;
};

struct MeisoCompositionEdgeTraceReader {
    uint64_t magic;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t trace_begin;
    uintptr_t trace_end;
    const uint8_t *trace;
    size_t trace_size;
    size_t cursor;
    uint32_t record_count;
    uint32_t next_sequence;
    uintptr_t bound_handle;
    MeisoCompositionEdgeSimulatorConfig config;
};

bool meiso_composition_edge_trace_private_checked_range(
    const void *pointer,
    size_t size,
    uintptr_t *out_begin,
    uintptr_t *out_end
);
bool meiso_composition_edge_trace_private_checked_add(
    size_t lhs,
    size_t rhs,
    size_t *out
);
bool meiso_composition_edge_trace_private_ranges_overlap(
    uintptr_t lhs_begin,
    uintptr_t lhs_end,
    uintptr_t rhs_begin,
    uintptr_t rhs_end
);
bool meiso_composition_edge_trace_private_range_disjoint(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
bool meiso_composition_edge_trace_private_bytes_zero(
    const uint8_t *bytes,
    size_t size
);
uint16_t meiso_composition_edge_trace_private_get_u16(
    const uint8_t *bytes
);
uint32_t meiso_composition_edge_trace_private_get_u32(
    const uint8_t *bytes
);
uint64_t meiso_composition_edge_trace_private_get_u64(
    const uint8_t *bytes
);
void meiso_composition_edge_trace_private_put_u16(
    uint8_t *bytes,
    uint16_t value
);
void meiso_composition_edge_trace_private_put_u32(
    uint8_t *bytes,
    uint32_t value
);
void meiso_composition_edge_trace_private_put_u64(
    uint8_t *bytes,
    uint64_t value
);
uint32_t meiso_composition_edge_trace_private_crc32c_update(
    uint32_t crc,
    const uint8_t *bytes,
    size_t size
);
uint32_t meiso_composition_edge_trace_private_crc32c(
    const uint8_t *bytes,
    size_t size
);
uint32_t meiso_composition_edge_trace_private_crc32c_pair(
    const uint8_t *prefix,
    size_t prefix_size,
    const uint8_t *suffix,
    size_t suffix_size
);
bool meiso_composition_edge_trace_private_config_valid(
    const MeisoCompositionEdgeSimulatorConfig *config
);
bool meiso_composition_edge_trace_private_encode_config(
    const MeisoCompositionEdgeSimulatorConfig *config,
    uint8_t bytes[MEISO_COMPOSITION_EDGE_TRACE_CONFIG_BYTES]
);
bool meiso_composition_edge_trace_private_decode_config(
    const uint8_t *input,
    size_t input_size,
    MeisoCompositionEdgeSimulatorConfig *out_config
);
void meiso_composition_edge_trace_private_semantic_init(
    MeisoCompositionEdgeTraceSemanticState *semantic,
    const MeisoCompositionEdgeSimulatorConfig *config
);
MeisoCompositionEdgeTraceStatus
meiso_composition_edge_trace_private_semantic_apply(
    MeisoCompositionEdgeTraceSemanticState *semantic,
    MeisoCompositionEdgeTraceRecordKind kind,
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *frame_unit_override
);
bool meiso_composition_edge_trace_private_decode_wifi_record(
    const uint8_t *payload,
    size_t payload_size,
    bool *out_enabled,
    MeisoWifiTelemetryDesiredParameters *out_parameters
);
bool meiso_composition_edge_trace_private_decode_media_record(
    const uint8_t *payload,
    size_t payload_size,
    bool *out_enabled,
    MeisoCompositionMediaDesired *out_desired
);
bool meiso_composition_edge_trace_private_decode_frame_record(
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *unit_override,
    MeisoCompositionEdgeSimulatorFrame *out_frame
);

#endif
