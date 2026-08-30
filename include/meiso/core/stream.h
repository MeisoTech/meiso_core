#ifndef MEISO_CORE_STREAM_H
#define MEISO_CORE_STREAM_H

#include <meiso/core/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoStream MeisoStream;

typedef uint8_t MeisoStreamOrdering;
enum {
    MEISO_STREAM_ORDER_STRICT_MONOTONIC = 1
};

typedef uint8_t MeisoStreamOverflowPolicy;
enum {
    MEISO_STREAM_OVERFLOW_DROP_OLDEST = 1,
    MEISO_STREAM_OVERFLOW_REJECT_NEWEST = 2
};

typedef uint8_t MeisoStreamDeadlinePolicy;
enum {
    MEISO_STREAM_DEADLINE_UNSUPPORTED = 1,
    MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE = 2
};

typedef struct {
    MeisoStreamOrdering ordering;
    MeisoStreamOverflowPolicy overflow;
    MeisoStreamDeadlinePolicy deadline_policy;
    /* queue_slots must be nonzero; history_slots may be zero. */
    uint32_t queue_slots;
    uint32_t history_slots;
    size_t max_dependency_size;
} MeisoStreamConfig;

typedef struct {
    MeisoOwnerScope producer;
    uint64_t epoch;
    uint64_t sequence;
} MeisoStreamKey;

typedef struct {
    MeisoStreamKey key;
    MeisoTimePoint source_time;
    MeisoDeadline sender_deadline;
    MeisoByteView dependency;
    MeisoByteView payload;
} MeisoStreamSample;

typedef struct {
    /* These are adapter assertions about the mapped local deadline. */
    uint8_t comparable;
    uint8_t uncertainty_acceptable;
    MeisoTimePoint local_deadline;
    MeisoTimePoint local_now;
} MeisoStreamAdmission;

typedef struct {
    uint64_t producer_resets;
    uint64_t reset_drops;
    uint64_t accepted;
    uint64_t gaps;
    uint64_t missing_samples;
    uint64_t duplicates;
    uint64_t conflicts;
    uint64_t stale;
    uint64_t sequence_wraps;
    uint64_t scope_mismatches;
    uint64_t epoch_mismatches;
    uint64_t deadline_unsupported;
    uint64_t clock_scope_mismatches;
    uint64_t expired;
    uint64_t capacity_failures;
    uint64_t overflow_drops;
    uint64_t overflow_rejections;
    uint64_t history_evictions;
    uint64_t pops;
} MeisoStreamStats;

typedef uint8_t MeisoStreamInitStatus;
enum {
    MEISO_STREAM_INIT_OK = 0,
    MEISO_STREAM_INIT_INVALID_ARGUMENT = 1,
    MEISO_STREAM_INIT_INVALID_DESCRIPTOR = 2,
    MEISO_STREAM_INIT_INVALID_CONFIG = 3,
    MEISO_STREAM_INIT_MISALIGNED_ARENA = 4,
    MEISO_STREAM_INIT_ARENA_TOO_SMALL = 5
};

typedef uint8_t MeisoStreamProducerStatus;
enum {
    MEISO_STREAM_PRODUCER_STARTED = 0,
    MEISO_STREAM_PRODUCER_SWITCHED = 1,
    MEISO_STREAM_PRODUCER_UNCHANGED = 2,
    MEISO_STREAM_PRODUCER_SCOPE_MISMATCH = 3,
    MEISO_STREAM_PRODUCER_INVALID = 4
};

typedef uint8_t MeisoStreamReceiveCode;
enum {
    MEISO_STREAM_RECEIVE_ACCEPTED = 0,
    MEISO_STREAM_RECEIVE_DROPPED_EXPIRED = 1,
    MEISO_STREAM_RECEIVE_DUPLICATE = 2,
    MEISO_STREAM_RECEIVE_CONFLICT = 3,
    MEISO_STREAM_RECEIVE_STALE = 4,
    MEISO_STREAM_RECEIVE_SEQUENCE_WRAP = 5,
    MEISO_STREAM_RECEIVE_SCOPE_MISMATCH = 6,
    MEISO_STREAM_RECEIVE_EPOCH_MISMATCH = 7,
    MEISO_STREAM_RECEIVE_DEADLINE_UNSUPPORTED = 8,
    MEISO_STREAM_RECEIVE_CLOCK_SCOPE_MISMATCH = 9,
    MEISO_STREAM_RECEIVE_CAPACITY = 10,
    MEISO_STREAM_RECEIVE_OVERFLOW_REJECTED = 11,
    MEISO_STREAM_RECEIVE_NO_PRODUCER = 12,
    MEISO_STREAM_RECEIVE_INVALID = 13
};

typedef uint8_t MeisoStreamReceiveFlags;
enum {
    MEISO_STREAM_RECEIVE_FLAG_NONE = 0U,
    MEISO_STREAM_RECEIVE_FLAG_GAP = 1U,
    MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST = 2U
};

typedef struct {
    MeisoStreamReceiveCode code;
    MeisoStreamReceiveFlags flags;
    uint64_t missing_before;
} MeisoStreamReceiveResult;

size_t meiso_stream_alignment_required(void);
size_t meiso_stream_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoStreamConfig *config
);
MeisoStreamInitStatus meiso_stream_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoStreamConfig *config,
    MeisoStream **out_stream
);
MeisoStreamProducerStatus meiso_stream_begin_producer(
    MeisoStream *stream,
    MeisoOwnerScope producer,
    uint64_t epoch
);
MeisoStreamReceiveResult meiso_stream_receive(
    MeisoStream *stream,
    const MeisoStreamSample *sample,
    const MeisoStreamAdmission *admission
);
/* A successful mutation of the same stream invalidates a borrowed peek view. */
bool meiso_stream_peek(const MeisoStream *stream, MeisoStreamSample *out_sample);
bool meiso_stream_pop(MeisoStream *stream);
size_t meiso_stream_count(const MeisoStream *stream);
bool meiso_stream_stats(const MeisoStream *stream, MeisoStreamStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif
