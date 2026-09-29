#include <meiso/core/stream.h>

#include "meiso_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    MeisoStreamKey key;
    MeisoTimePoint source_time;
    MeisoDeadline sender_deadline;
    size_t dependency_size;
    size_t payload_size;
} MeisoStreamSlot;

struct MeisoStream {
    MeisoResourceDescriptor descriptor;
    MeisoStreamConfig config;
    MeisoOwnerScope producer;
    uint64_t epoch;
    uint64_t highest_sequence;
    size_t storage_offset;
    size_t slot_stride;
    uint32_t queue_head;
    uint32_t queue_count;
    uint32_t history_head;
    uint32_t history_count;
    MeisoStreamStats stats;
    bool producer_active;
};

static bool checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out)
{
    if (lhs != 0U && rhs > SIZE_MAX / lhs) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

static bool align_up(size_t value, size_t alignment, size_t *out)
{
    size_t remainder;

    if (alignment == 0U) {
        return false;
    }
    remainder = value % alignment;
    if (remainder == 0U) {
        *out = value;
        return true;
    }
    return checked_add(value, alignment - remainder, out);
}

static bool output_overlaps_arena(
    const void *arena,
    size_t arena_size,
    const void *output,
    size_t output_size
)
{
    const uintptr_t arena_start = (uintptr_t)arena;
    const uintptr_t output_start = (uintptr_t)output;
    uintptr_t arena_end;
    uintptr_t output_end;

    if (arena_size > UINTPTR_MAX - arena_start ||
        output_size > UINTPTR_MAX - output_start) {
        return true;
    }
    arena_end = arena_start + arena_size;
    output_end = output_start + output_size;
    return arena_start < output_end && output_start < arena_end;
}

static bool descriptor_valid(const MeisoResourceDescriptor *descriptor)
{
    return meiso_resource_descriptor_validate(descriptor) == MEISO_DESCRIPTOR_VALID &&
        descriptor->kind == MEISO_RESOURCE_KIND_STREAM;
}

static bool config_valid(const MeisoStreamConfig *config)
{
    return config != NULL &&
        config->ordering == MEISO_STREAM_ORDER_STRICT_MONOTONIC &&
        (config->overflow == MEISO_STREAM_OVERFLOW_DROP_OLDEST ||
         config->overflow == MEISO_STREAM_OVERFLOW_REJECT_NEWEST) &&
        (config->deadline_policy == MEISO_STREAM_DEADLINE_UNSUPPORTED ||
         config->deadline_policy == MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE) &&
        config->queue_slots > 0U;
}

static bool calculate_layout(
    const MeisoResourceDescriptor *descriptor,
    const MeisoStreamConfig *config,
    size_t *out_storage_offset,
    size_t *out_slot_stride,
    size_t *out_required
)
{
    size_t slot_size;
    size_t slot_count;
    size_t slots_size;

    return descriptor_valid(descriptor) && config_valid(config) &&
        checked_add(
            sizeof(MeisoStreamSlot), config->max_dependency_size, &slot_size
        ) &&
        checked_add(slot_size, descriptor->max_payload_size, &slot_size) &&
        align_up(slot_size, meiso_stream_alignment_required(), out_slot_stride) &&
        checked_add(
            (size_t)config->queue_slots,
            (size_t)config->history_slots,
            &slot_count
        ) &&
        checked_add(slot_count, 1U, &slot_count) &&
        checked_multiply(slot_count, *out_slot_stride, &slots_size) &&
        align_up(
            sizeof(MeisoStream),
            meiso_stream_alignment_required(),
            out_storage_offset
        ) &&
        checked_add(*out_storage_offset, slots_size, out_required);
}

static bool stream_valid(const MeisoStream *stream)
{
    return stream != NULL && descriptor_valid(&stream->descriptor) &&
        config_valid(&stream->config) && stream->slot_stride >= sizeof(MeisoStreamSlot);
}

static bool arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % meiso_stream_alignment_required() == 0U;
}

static bool time_equal(MeisoTimePoint lhs, MeisoTimePoint rhs)
{
    return lhs.clock_epoch == rhs.clock_epoch &&
        lhs.monotonic_ns == rhs.monotonic_ns;
}

static bool deadline_equal(MeisoDeadline lhs, MeisoDeadline rhs)
{
    if (lhs.present != rhs.present) {
        return false;
    }
    return !lhs.present || time_equal(lhs.value, rhs.value);
}

static bool bytes_equal(const uint8_t *stored, size_t size, MeisoByteView value)
{
    return size == value.size &&
        (size == 0U || memcmp(stored, value.data, size) == 0);
}

static uint32_t ring_index(uint32_t head, uint32_t offset, uint32_t capacity)
{
    return (uint32_t)(
        ((uint64_t)head + (uint64_t)offset) % (uint64_t)capacity
    );
}

static uint8_t *storage_base(const MeisoStream *stream)
{
    return (uint8_t *)(void *)stream + stream->storage_offset;
}

static MeisoStreamSlot *queue_slot(const MeisoStream *stream, uint32_t index)
{
    return (MeisoStreamSlot *)(void *)(storage_base(stream) +
        (size_t)index * stream->slot_stride);
}

static MeisoStreamSlot *history_slot(const MeisoStream *stream, uint32_t index)
{
    return (MeisoStreamSlot *)(void *)(storage_base(stream) +
        ((size_t)stream->config.queue_slots + (size_t)index) * stream->slot_stride);
}

static MeisoStreamSlot *scratch_slot(const MeisoStream *stream)
{
    return (MeisoStreamSlot *)(void *)(storage_base(stream) +
        ((size_t)stream->config.queue_slots +
         (size_t)stream->config.history_slots) * stream->slot_stride);
}

static uint8_t *slot_dependency(const MeisoStream *stream, MeisoStreamSlot *slot)
{
    (void)stream;
    return (uint8_t *)(void *)(slot + 1);
}

static uint8_t *slot_payload(const MeisoStream *stream, MeisoStreamSlot *slot)
{
    return slot_dependency(stream, slot) + stream->config.max_dependency_size;
}

static void slot_view(
    const MeisoStream *stream,
    MeisoStreamSlot *slot,
    MeisoStreamSample *out_sample
)
{
    out_sample->key = slot->key;
    out_sample->source_time = slot->source_time;
    out_sample->sender_deadline = slot->sender_deadline;
    out_sample->dependency.data = slot_dependency(stream, slot);
    out_sample->dependency.size = slot->dependency_size;
    out_sample->payload.data = slot_payload(stream, slot);
    out_sample->payload.size = slot->payload_size;
}

static bool slot_sample_equal(
    const MeisoStream *stream,
    MeisoStreamSlot *slot,
    const MeisoStreamSample *sample
)
{
    return time_equal(slot->source_time, sample->source_time) &&
        deadline_equal(slot->sender_deadline, sample->sender_deadline) &&
        bytes_equal(
            slot_dependency(stream, slot), slot->dependency_size, sample->dependency
        ) &&
        bytes_equal(slot_payload(stream, slot), slot->payload_size, sample->payload);
}

static MeisoStreamSlot *find_sequence(
    const MeisoStream *stream,
    uint64_t sequence
)
{
    uint32_t offset;

    for (offset = 0U; offset < stream->queue_count; ++offset) {
        const uint32_t index = ring_index(
            stream->queue_head, offset, stream->config.queue_slots
        );
        MeisoStreamSlot *slot = queue_slot(stream, index);
        if (slot->key.sequence == sequence) {
            return slot;
        }
    }
    for (offset = 0U; offset < stream->history_count; ++offset) {
        const uint32_t index = ring_index(
            stream->history_head, offset, stream->config.history_slots
        );
        MeisoStreamSlot *slot = history_slot(stream, index);
        if (slot->key.sequence == sequence) {
            return slot;
        }
    }
    return NULL;
}

static void retain_slot(MeisoStream *stream, MeisoStreamSlot *source)
{
    uint32_t index;

    if (stream->config.history_slots == 0U) {
        return;
    }
    if (stream->history_count < stream->config.history_slots) {
        index = ring_index(
            stream->history_head,
            stream->history_count,
            stream->config.history_slots
        );
        stream->history_count += 1U;
    } else {
        index = stream->history_head;
        stream->history_head = ring_index(
            stream->history_head, 1U, stream->config.history_slots
        );
        meiso_increment_saturated(&stream->stats.history_evictions);
    }
    memmove(history_slot(stream, index), source, stream->slot_stride);
}

static void remove_queue_head(MeisoStream *stream, bool overflow)
{
    stream->queue_head = ring_index(
        stream->queue_head, 1U, stream->config.queue_slots
    );
    stream->queue_count -= 1U;
    if (overflow) {
        meiso_increment_saturated(&stream->stats.overflow_drops);
    } else {
        meiso_increment_saturated(&stream->stats.pops);
    }
}

static void fill_scratch(MeisoStream *stream, const MeisoStreamSample *sample)
{
    MeisoStreamSlot *slot = scratch_slot(stream);

    memset(slot, 0, stream->slot_stride);
    slot->key = sample->key;
    slot->source_time = sample->source_time;
    slot->sender_deadline = sample->sender_deadline;
    slot->dependency_size = sample->dependency.size;
    slot->payload_size = sample->payload.size;
    if (sample->dependency.size != 0U) {
        memmove(
            slot_dependency(stream, slot),
            sample->dependency.data,
            sample->dependency.size
        );
    }
    if (sample->payload.size != 0U) {
        memmove(slot_payload(stream, slot), sample->payload.data, sample->payload.size);
    }
}

static bool admission_comparable(const MeisoStreamAdmission *admission)
{
    return admission != NULL && admission->comparable != 0U &&
        admission->uncertainty_acceptable != 0U &&
        meiso_time_point_valid(admission->local_deadline) &&
        meiso_time_point_valid(admission->local_now) &&
        admission->local_deadline.clock_epoch == admission->local_now.clock_epoch;
}

size_t meiso_stream_alignment_required(void)
{
    return _Alignof(MeisoMaxAlign);
}

size_t meiso_stream_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoStreamConfig *config
)
{
    size_t storage_offset;
    size_t slot_stride;
    size_t required;

    if (!calculate_layout(
            descriptor,
            config,
            &storage_offset,
            &slot_stride,
            &required
        )) {
        return 0U;
    }
    return required;
}

MeisoStreamInitStatus meiso_stream_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoStreamConfig *config,
    MeisoStream **out_stream
)
{
    size_t required;
    size_t storage_offset;
    size_t slot_stride;
    MeisoResourceDescriptor descriptor_copy;
    MeisoStreamConfig config_copy;
    MeisoStream *stream;

    if (out_stream == NULL) {
        return MEISO_STREAM_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL &&
        output_overlaps_arena(arena, arena_size, out_stream, sizeof(*out_stream))) {
        return MEISO_STREAM_INIT_INVALID_ARGUMENT;
    }
    *out_stream = NULL;
    if (!descriptor_valid(descriptor)) {
        return MEISO_STREAM_INIT_INVALID_DESCRIPTOR;
    }
    if (!config_valid(config)) {
        return MEISO_STREAM_INIT_INVALID_CONFIG;
    }
    if (!arena_aligned(arena)) {
        return MEISO_STREAM_INIT_MISALIGNED_ARENA;
    }
    if (!calculate_layout(
            descriptor,
            config,
            &storage_offset,
            &slot_stride,
            &required
        ) || arena_size < required) {
        return MEISO_STREAM_INIT_ARENA_TOO_SMALL;
    }
    descriptor_copy = *descriptor;
    config_copy = *config;
    memset(arena, 0, required);
    stream = arena;
    stream->descriptor = descriptor_copy;
    stream->config = config_copy;
    stream->storage_offset = storage_offset;
    stream->slot_stride = slot_stride;
    *out_stream = stream;
    return MEISO_STREAM_INIT_OK;
}

MeisoStreamProducerStatus meiso_stream_begin_producer(
    MeisoStream *stream,
    MeisoOwnerScope producer,
    uint64_t epoch
)
{
    bool same_producer;
    bool was_active;

    if (!stream_valid(stream) || !meiso_owner_scope_valid(producer) || epoch == 0U) {
        return MEISO_STREAM_PRODUCER_INVALID;
    }
    if (producer.role != stream->descriptor.owner) {
        meiso_increment_saturated(&stream->stats.scope_mismatches);
        return MEISO_STREAM_PRODUCER_SCOPE_MISMATCH;
    }
    same_producer = stream->producer_active &&
        meiso_owner_scope_equal(stream->producer, producer);
    if (same_producer && epoch == stream->epoch) {
        return MEISO_STREAM_PRODUCER_UNCHANGED;
    }
    was_active = stream->producer_active;
    meiso_add_saturated(&stream->stats.reset_drops, stream->queue_count);
    stream->producer = producer;
    stream->epoch = epoch;
    stream->highest_sequence = 0U;
    stream->queue_head = 0U;
    stream->queue_count = 0U;
    stream->history_head = 0U;
    stream->history_count = 0U;
    stream->producer_active = true;
    meiso_increment_saturated(&stream->stats.producer_resets);
    return was_active ? MEISO_STREAM_PRODUCER_SWITCHED :
        MEISO_STREAM_PRODUCER_STARTED;
}

static MeisoStreamReceiveResult receive_result(
    MeisoStreamReceiveCode code,
    MeisoStreamReceiveFlags flags,
    uint64_t missing_before
)
{
    MeisoStreamReceiveResult result = {code, flags, missing_before};
    return result;
}

MeisoStreamReceiveResult meiso_stream_receive(
    MeisoStream *stream,
    const MeisoStreamSample *sample,
    const MeisoStreamAdmission *admission
)
{
    MeisoStreamSlot *existing;
    MeisoStreamSlot *target;
    uint64_t missing = 0U;
    uint32_t tail;
    bool dropped = false;

    if (!stream_valid(stream) || sample == NULL ||
        !meiso_owner_scope_valid(sample->key.producer) ||
        sample->key.epoch == 0U || sample->key.sequence == 0U ||
        !meiso_time_point_valid(sample->source_time) ||
        !meiso_deadline_valid(sample->sender_deadline) ||
        (sample->sender_deadline.present &&
         sample->sender_deadline.value.clock_epoch !=
            sample->source_time.clock_epoch) ||
        !meiso_byte_view_valid(sample->dependency) ||
        !meiso_byte_view_valid(sample->payload)) {
        return receive_result(
            MEISO_STREAM_RECEIVE_INVALID, MEISO_STREAM_RECEIVE_FLAG_NONE, 0U
        );
    }
    if (!stream->producer_active) {
        return receive_result(
            MEISO_STREAM_RECEIVE_NO_PRODUCER, MEISO_STREAM_RECEIVE_FLAG_NONE, 0U
        );
    }
    if (!meiso_owner_scope_equal(sample->key.producer, stream->producer)) {
        meiso_increment_saturated(&stream->stats.scope_mismatches);
        return receive_result(
            MEISO_STREAM_RECEIVE_SCOPE_MISMATCH,
            MEISO_STREAM_RECEIVE_FLAG_NONE,
            0U
        );
    }
    if (sample->key.epoch != stream->epoch) {
        meiso_increment_saturated(&stream->stats.epoch_mismatches);
        return receive_result(
            MEISO_STREAM_RECEIVE_EPOCH_MISMATCH,
            MEISO_STREAM_RECEIVE_FLAG_NONE,
            0U
        );
    }

    existing = find_sequence(stream, sample->key.sequence);
    if (existing != NULL) {
        if (slot_sample_equal(stream, existing, sample)) {
            meiso_increment_saturated(&stream->stats.duplicates);
            return receive_result(
                MEISO_STREAM_RECEIVE_DUPLICATE,
                MEISO_STREAM_RECEIVE_FLAG_NONE,
                0U
            );
        }
        meiso_increment_saturated(&stream->stats.conflicts);
        return receive_result(
            MEISO_STREAM_RECEIVE_CONFLICT, MEISO_STREAM_RECEIVE_FLAG_NONE, 0U
        );
    }
    if (stream->highest_sequence != 0U &&
        sample->key.sequence <= stream->highest_sequence) {
        if (stream->highest_sequence == UINT64_MAX) {
            meiso_increment_saturated(&stream->stats.sequence_wraps);
            return receive_result(
                MEISO_STREAM_RECEIVE_SEQUENCE_WRAP,
                MEISO_STREAM_RECEIVE_FLAG_NONE,
                0U
            );
        }
        meiso_increment_saturated(&stream->stats.stale);
        return receive_result(
            MEISO_STREAM_RECEIVE_STALE, MEISO_STREAM_RECEIVE_FLAG_NONE, 0U
        );
    }
    if (sample->dependency.size > stream->config.max_dependency_size ||
        sample->payload.size > stream->descriptor.max_payload_size) {
        meiso_increment_saturated(&stream->stats.capacity_failures);
        return receive_result(
            MEISO_STREAM_RECEIVE_CAPACITY, MEISO_STREAM_RECEIVE_FLAG_NONE, 0U
        );
    }
    if (sample->sender_deadline.present &&
        stream->config.deadline_policy == MEISO_STREAM_DEADLINE_UNSUPPORTED) {
        meiso_increment_saturated(&stream->stats.deadline_unsupported);
        return receive_result(
            MEISO_STREAM_RECEIVE_DEADLINE_UNSUPPORTED,
            MEISO_STREAM_RECEIVE_FLAG_NONE,
            0U
        );
    }
    if (sample->sender_deadline.present &&
        stream->config.deadline_policy ==
            MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE) {
        if (!admission_comparable(admission)) {
            meiso_increment_saturated(&stream->stats.clock_scope_mismatches);
            return receive_result(
                MEISO_STREAM_RECEIVE_CLOCK_SCOPE_MISMATCH,
                MEISO_STREAM_RECEIVE_FLAG_NONE,
                0U
            );
        }
        if (admission->local_now.monotonic_ns >
            admission->local_deadline.monotonic_ns) {
            if (stream->highest_sequence == 0U) {
                missing = sample->key.sequence - 1U;
            } else {
                missing = sample->key.sequence - stream->highest_sequence - 1U;
            }
            fill_scratch(stream, sample);
            retain_slot(stream, scratch_slot(stream));
            stream->highest_sequence = sample->key.sequence;
            meiso_increment_saturated(&stream->stats.expired);
            if (missing != 0U) {
                meiso_increment_saturated(&stream->stats.gaps);
                meiso_add_saturated(&stream->stats.missing_samples, missing);
            }
            return receive_result(
                MEISO_STREAM_RECEIVE_DROPPED_EXPIRED,
                missing == 0U ? MEISO_STREAM_RECEIVE_FLAG_NONE :
                    MEISO_STREAM_RECEIVE_FLAG_GAP,
                missing
            );
        }
    }
    if (stream->queue_count == stream->config.queue_slots &&
        stream->config.overflow == MEISO_STREAM_OVERFLOW_REJECT_NEWEST) {
        meiso_increment_saturated(&stream->stats.overflow_rejections);
        return receive_result(
            MEISO_STREAM_RECEIVE_OVERFLOW_REJECTED,
            MEISO_STREAM_RECEIVE_FLAG_NONE,
            0U
        );
    }

    if (stream->highest_sequence == 0U) {
        missing = sample->key.sequence - 1U;
    } else {
        missing = sample->key.sequence - stream->highest_sequence - 1U;
    }
    fill_scratch(stream, sample);
    if (stream->queue_count == stream->config.queue_slots) {
        remove_queue_head(stream, true);
        dropped = true;
    }
    tail = ring_index(
        stream->queue_head, stream->queue_count, stream->config.queue_slots
    );
    target = queue_slot(stream, tail);
    memmove(target, scratch_slot(stream), stream->slot_stride);
    retain_slot(stream, scratch_slot(stream));
    stream->queue_count += 1U;
    stream->highest_sequence = sample->key.sequence;
    meiso_increment_saturated(&stream->stats.accepted);
    if (missing != 0U) {
        meiso_increment_saturated(&stream->stats.gaps);
        meiso_add_saturated(&stream->stats.missing_samples, missing);
    }
    return receive_result(
        MEISO_STREAM_RECEIVE_ACCEPTED,
        (MeisoStreamReceiveFlags)(
            (missing == 0U ? MEISO_STREAM_RECEIVE_FLAG_NONE :
             MEISO_STREAM_RECEIVE_FLAG_GAP) |
            (dropped ? MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST :
             MEISO_STREAM_RECEIVE_FLAG_NONE)
        ),
        missing
    );
}

bool meiso_stream_peek(const MeisoStream *stream, MeisoStreamSample *out_sample)
{
    if (!stream_valid(stream) || out_sample == NULL || stream->queue_count == 0U) {
        return false;
    }
    slot_view(stream, queue_slot(stream, stream->queue_head), out_sample);
    return true;
}

bool meiso_stream_pop(MeisoStream *stream)
{
    if (!stream_valid(stream) || stream->queue_count == 0U) {
        return false;
    }
    remove_queue_head(stream, false);
    return true;
}

size_t meiso_stream_count(const MeisoStream *stream)
{
    return stream_valid(stream) ? stream->queue_count : 0U;
}

bool meiso_stream_stats(const MeisoStream *stream, MeisoStreamStats *out_stats)
{
    if (!stream_valid(stream) || out_stats == NULL) {
        return false;
    }
    *out_stats = stream->stats;
    return true;
}
