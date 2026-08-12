#include "meiso_blob.h"

#include "meiso_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    MeisoContentId content_id;
    size_t content_size;
    bool occupied;
} MeisoBlobSlot;

struct MeisoBlobStore {
    MeisoResourceDescriptor descriptor;
    MeisoBlobConfig config;
    size_t storage_offset;
    size_t slot_stride;
    size_t staging_offset;
    size_t verification_offset;
    uint32_t published_count;
    MeisoContentId staging_content_id;
    size_t staging_length;
    MeisoBlobStats stats;
    bool staging_active;
    bool verifier_active;
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

static bool descriptor_valid(const MeisoResourceDescriptor *descriptor)
{
    return meiso_resource_descriptor_validate(descriptor) ==
            MEISO_DESCRIPTOR_VALID &&
        descriptor->kind == MEISO_RESOURCE_KIND_BLOB;
}

static bool config_valid(const MeisoBlobConfig *config)
{
    return config != NULL && config->published_slot_count > 0U;
}

static bool calculate_layout(
    const MeisoResourceDescriptor *descriptor,
    const MeisoBlobConfig *config,
    size_t *out_storage_offset,
    size_t *out_slot_stride,
    size_t *out_staging_offset,
    size_t *out_verification_offset,
    size_t *out_required
)
{
    size_t slot_size;
    size_t slots_size;

    if (!descriptor_valid(descriptor) || !config_valid(config) ||
        !checked_add(
            sizeof(MeisoBlobSlot), descriptor->max_payload_size, &slot_size
        ) ||
        !align_up(slot_size, meiso_blob_alignment_required(), out_slot_stride) ||
        !align_up(
            sizeof(MeisoBlobStore),
            meiso_blob_alignment_required(),
            out_storage_offset
        ) ||
        !checked_multiply(
            (size_t)config->published_slot_count, *out_slot_stride, &slots_size
        ) ||
        !checked_add(*out_storage_offset, slots_size, out_staging_offset) ||
        !checked_add(
            *out_staging_offset,
            descriptor->max_payload_size,
            out_verification_offset
        ) ||
        !checked_add(
            *out_verification_offset,
            descriptor->max_payload_size,
            out_required
        )) {
        return false;
    }
    return true;
}

static bool store_valid(const MeisoBlobStore *store)
{
    size_t storage_offset;
    size_t slot_stride;
    size_t staging_offset;
    size_t verification_offset;
    size_t required;

    return store != NULL &&
        calculate_layout(
            &store->descriptor,
            &store->config,
            &storage_offset,
            &slot_stride,
            &staging_offset,
            &verification_offset,
            &required
        ) &&
        store->storage_offset == storage_offset &&
        store->slot_stride == slot_stride &&
        store->staging_offset == staging_offset &&
        store->verification_offset == verification_offset &&
        store->published_count <= store->config.published_slot_count;
}

static bool arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % meiso_blob_alignment_required() == 0U;
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

static bool abort_reason_valid(MeisoBlobAbortReason reason)
{
    return reason == MEISO_BLOB_ABORT_CALLER_REQUEST ||
        reason == MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY ||
        reason == MEISO_BLOB_ABORT_STORE_SHUTDOWN;
}

static void canonicalize_content_id(
    const MeisoContentId *source,
    MeisoContentId *target
)
{
    memset(target, 0, sizeof(*target));
    target->algorithm = source->algorithm;
    target->digest_size = source->digest_size;
    memmove(target->digest, source->digest, source->digest_size);
}

static uint8_t *storage_base(MeisoBlobStore *store)
{
    return (uint8_t *)(void *)store + store->storage_offset;
}

static const uint8_t *const_storage_base(const MeisoBlobStore *store)
{
    return (const uint8_t *)(const void *)store + store->storage_offset;
}

static MeisoBlobSlot *slot_at(MeisoBlobStore *store, uint32_t index)
{
    return (MeisoBlobSlot *)(void *)(
        storage_base(store) + (size_t)index * store->slot_stride
    );
}

static const MeisoBlobSlot *const_slot_at(
    const MeisoBlobStore *store,
    uint32_t index
)
{
    return (const MeisoBlobSlot *)(const void *)(
        const_storage_base(store) + (size_t)index * store->slot_stride
    );
}

static uint8_t *slot_content(MeisoBlobSlot *slot)
{
    return (uint8_t *)(void *)(slot + 1);
}

static const uint8_t *const_slot_content(const MeisoBlobSlot *slot)
{
    return (const uint8_t *)(const void *)(slot + 1);
}

static uint8_t *staging_content(MeisoBlobStore *store)
{
    return (uint8_t *)(void *)store + store->staging_offset;
}

static uint8_t *verification_content(MeisoBlobStore *store)
{
    return (uint8_t *)(void *)store + store->verification_offset;
}

static const MeisoBlobSlot *find_slot(
    const MeisoBlobStore *store,
    const MeisoContentId *content_id
)
{
    uint32_t index;

    for (index = 0U; index < store->config.published_slot_count; ++index) {
        const MeisoBlobSlot *slot = const_slot_at(store, index);
        if (slot->occupied &&
            meiso_content_id_equal(&slot->content_id, content_id)) {
            return slot;
        }
    }
    return NULL;
}

static MeisoBlobSlot *find_empty_slot(MeisoBlobStore *store)
{
    uint32_t index;

    for (index = 0U; index < store->config.published_slot_count; ++index) {
        MeisoBlobSlot *slot = slot_at(store, index);
        if (!slot->occupied) {
            return slot;
        }
    }
    return NULL;
}

static bool slot_content_equal(
    const MeisoBlobSlot *slot,
    const uint8_t *candidate,
    size_t candidate_size
)
{
    return slot->content_size == candidate_size &&
        (candidate_size == 0U ||
         memcmp(const_slot_content(slot), candidate, candidate_size) == 0);
}

static void clear_staging(MeisoBlobStore *store)
{
    if (store->staging_length != 0U) {
        memset(staging_content(store), 0, store->staging_length);
    }
    memset(&store->staging_content_id, 0, sizeof(store->staging_content_id));
    store->staging_length = 0U;
    store->staging_active = false;
}

static void clear_verification_snapshot(
    MeisoBlobStore *store,
    size_t snapshot_size
)
{
    if (snapshot_size != 0U) {
        memset(verification_content(store), 0, snapshot_size);
    }
}

static void record_reentrant_rejection(MeisoBlobStore *store)
{
    meiso_increment_saturated(&store->stats.reentrant_rejections);
}

size_t meiso_blob_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_blob_store_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoBlobConfig *config
)
{
    size_t storage_offset;
    size_t slot_stride;
    size_t staging_offset;
    size_t verification_offset;
    size_t required;

    if (!calculate_layout(
        descriptor,
        config,
        &storage_offset,
        &slot_stride,
        &staging_offset,
        &verification_offset,
        &required
    )) {
        return 0U;
    }
    return required;
}

MeisoBlobInitStatus meiso_blob_store_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoBlobConfig *config,
    MeisoBlobStore **out_store
)
{
    MeisoResourceDescriptor descriptor_copy;
    MeisoBlobConfig config_copy;
    size_t storage_offset;
    size_t slot_stride;
    size_t staging_offset;
    size_t verification_offset;
    size_t required;
    MeisoBlobStore *store;

    if (out_store == NULL) {
        return MEISO_BLOB_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL && output_overlaps_arena(
        arena, arena_size, out_store, sizeof(*out_store)
    )) {
        return MEISO_BLOB_INIT_INVALID_ARGUMENT;
    }
    *out_store = NULL;
    if (!descriptor_valid(descriptor)) {
        return MEISO_BLOB_INIT_INVALID_DESCRIPTOR;
    }
    if (!config_valid(config)) {
        return MEISO_BLOB_INIT_INVALID_CONFIG;
    }
    if (!arena_aligned(arena)) {
        return MEISO_BLOB_INIT_MISALIGNED_ARENA;
    }
    if (!calculate_layout(
        descriptor,
        config,
        &storage_offset,
        &slot_stride,
        &staging_offset,
        &verification_offset,
        &required
    ) || arena_size < required) {
        return MEISO_BLOB_INIT_ARENA_TOO_SMALL;
    }

    descriptor_copy = *descriptor;
    config_copy = *config;
    memset(arena, 0, required);
    store = arena;
    store->descriptor = descriptor_copy;
    store->config = config_copy;
    store->storage_offset = storage_offset;
    store->slot_stride = slot_stride;
    store->staging_offset = staging_offset;
    store->verification_offset = verification_offset;
    *out_store = store;
    return MEISO_BLOB_INIT_OK;
}

MeisoBlobStageStatus meiso_blob_store_stage_begin(
    MeisoBlobStore *store,
    const MeisoContentId *content_id,
    size_t content_length
)
{
    MeisoContentId canonical_id;
    const MeisoBlobSlot *published;

    if (!store_valid(store)) {
        return MEISO_BLOB_STAGE_INVALID;
    }
    if (store->verifier_active) {
        record_reentrant_rejection(store);
        return MEISO_BLOB_STAGE_REENTRANT;
    }
    if (!meiso_content_id_valid(content_id)) {
        meiso_increment_saturated(&store->stats.invalid_content_ids);
        return MEISO_BLOB_STAGE_INVALID_CONTENT_ID;
    }
    canonicalize_content_id(content_id, &canonical_id);
    if (store->staging_active) {
        if (store->staging_length == content_length &&
            meiso_content_id_equal(
                &store->staging_content_id, &canonical_id
            )) {
            return MEISO_BLOB_STAGE_UNCHANGED;
        }
        meiso_increment_saturated(&store->stats.stage_busy);
        return MEISO_BLOB_STAGE_BUSY;
    }
    if (content_length > store->descriptor.max_payload_size) {
        meiso_increment_saturated(&store->stats.capacity_failures);
        return MEISO_BLOB_STAGE_CAPACITY;
    }
    published = find_slot(store, &canonical_id);
    if (published != NULL && published->content_size != content_length) {
        meiso_increment_saturated(&store->stats.conflicts);
        return MEISO_BLOB_STAGE_CONFLICT;
    }

    if (content_length != 0U) {
        memset(staging_content(store), 0, content_length);
    }
    store->staging_content_id = canonical_id;
    store->staging_length = content_length;
    store->staging_active = true;
    meiso_increment_saturated(&store->stats.stages_started);
    return MEISO_BLOB_STAGE_STARTED;
}

bool meiso_blob_store_staging_buffer(
    MeisoBlobStore *store,
    MeisoMutableBuffer *out_buffer
)
{
    if (!store_valid(store) || out_buffer == NULL) {
        return false;
    }
    if (store->verifier_active) {
        record_reentrant_rejection(store);
        return false;
    }
    if (!store->staging_active) {
        return false;
    }
    out_buffer->data = staging_content(store);
    out_buffer->capacity = store->staging_length;
    return true;
}

MeisoBlobCommitStatus meiso_blob_store_commit_verified(
    MeisoBlobStore *store,
    size_t written_size,
    MeisoBlobVerifier verifier,
    void *verifier_context
)
{
    MeisoByteView verification_view;
    MeisoContentId expected_content_id;
    MeisoContentId publication_content_id;
    const MeisoBlobSlot *existing;
    MeisoBlobSlot *target;
    size_t candidate_size;
    bool verified;

    if (!store_valid(store) || verifier == NULL) {
        return MEISO_BLOB_COMMIT_INVALID;
    }
    if (store->verifier_active) {
        record_reentrant_rejection(store);
        return MEISO_BLOB_COMMIT_REENTRANT;
    }
    if (!store->staging_active) {
        return MEISO_BLOB_COMMIT_NO_STAGING;
    }
    if (written_size != store->staging_length) {
        meiso_increment_saturated(&store->stats.length_mismatches);
        return MEISO_BLOB_COMMIT_LENGTH_MISMATCH;
    }

    candidate_size = store->staging_length;
    if (candidate_size != 0U) {
        memmove(
            verification_content(store),
            staging_content(store),
            candidate_size
        );
    }
    verification_view.data = verification_content(store);
    verification_view.size = candidate_size;
    expected_content_id = store->staging_content_id;
    publication_content_id = store->staging_content_id;
    store->verifier_active = true;
    verified = verifier(
        &expected_content_id, verification_view, verifier_context
    );
    store->verifier_active = false;
    if (!verified) {
        clear_verification_snapshot(store, candidate_size);
        meiso_increment_saturated(&store->stats.integrity_failures);
        return MEISO_BLOB_COMMIT_INTEGRITY_FAILED;
    }

    existing = find_slot(store, &publication_content_id);
    if (existing != NULL) {
        if (slot_content_equal(
            existing, verification_content(store), candidate_size
        )) {
            clear_verification_snapshot(store, candidate_size);
            clear_staging(store);
            meiso_increment_saturated(&store->stats.duplicates);
            return MEISO_BLOB_COMMIT_DUPLICATE;
        }
        clear_verification_snapshot(store, candidate_size);
        meiso_increment_saturated(&store->stats.conflicts);
        return MEISO_BLOB_COMMIT_CONFLICT;
    }

    target = find_empty_slot(store);
    if (target == NULL) {
        clear_verification_snapshot(store, candidate_size);
        meiso_increment_saturated(&store->stats.capacity_failures);
        return MEISO_BLOB_COMMIT_CAPACITY;
    }

    target->content_id = publication_content_id;
    target->content_size = candidate_size;
    if (candidate_size != 0U) {
        memmove(
            slot_content(target),
            verification_content(store),
            candidate_size
        );
    }
    clear_verification_snapshot(store, candidate_size);
    target->occupied = true;
    store->published_count += 1U;
    clear_staging(store);
    meiso_increment_saturated(&store->stats.publications);
    return MEISO_BLOB_COMMIT_PUBLISHED;
}

MeisoBlobAbortStatus meiso_blob_store_abort_staging(
    MeisoBlobStore *store,
    MeisoBlobAbortReason reason
)
{
    if (!store_valid(store)) {
        return MEISO_BLOB_ABORT_INVALID;
    }
    if (store->verifier_active) {
        record_reentrant_rejection(store);
        return MEISO_BLOB_ABORT_REENTRANT;
    }
    if (!abort_reason_valid(reason)) {
        return MEISO_BLOB_ABORT_INVALID_REASON;
    }
    if (!store->staging_active) {
        return MEISO_BLOB_ABORT_NO_STAGING;
    }

    clear_staging(store);
    meiso_increment_saturated(&store->stats.aborts);
    if (reason == MEISO_BLOB_ABORT_CALLER_REQUEST) {
        meiso_increment_saturated(&store->stats.caller_aborts);
    } else if (reason == MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY) {
        meiso_increment_saturated(&store->stats.transfer_aborts);
    } else {
        meiso_increment_saturated(&store->stats.shutdown_aborts);
    }
    return MEISO_BLOB_ABORTED;
}

bool meiso_blob_store_lookup(
    const MeisoBlobStore *store,
    const MeisoContentId *content_id,
    MeisoBlobView *out_view
)
{
    const MeisoBlobSlot *slot;

    if (!store_valid(store) || !meiso_content_id_valid(content_id) ||
        out_view == NULL) {
        return false;
    }
    slot = find_slot(store, content_id);
    if (slot == NULL) {
        return false;
    }
    out_view->content_id = slot->content_id;
    out_view->content.data = const_slot_content(slot);
    out_view->content.size = slot->content_size;
    return true;
}

size_t meiso_blob_store_count(const MeisoBlobStore *store)
{
    if (!store_valid(store)) {
        return 0U;
    }
    return store->published_count;
}

bool meiso_blob_store_stats(
    const MeisoBlobStore *store,
    MeisoBlobStats *out_stats
)
{
    if (!store_valid(store) || out_stats == NULL) {
        return false;
    }
    *out_stats = store->stats;
    return true;
}
