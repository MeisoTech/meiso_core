#include "meiso_blob.h"

#include <stdio.h>
#include <string.h>

typedef union {
    max_align_t alignment;
    uint8_t bytes[16384];
} TestArena;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                         \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct {
    MeisoContentId expected_id;
    const uint8_t *expected_content;
    size_t expected_size;
    size_t calls;
    bool require_canonical_id;
} VerifyContext;

typedef struct {
    MeisoBlobStore *store;
    MeisoContentId content_id;
    MeisoBlobStageStatus stage_status;
    MeisoBlobCommitStatus commit_status;
    MeisoBlobAbortStatus abort_status;
    bool staging_visible;
    size_t calls;
} ReentrantContext;

typedef struct {
    ReentrantContext reentrant;
    bool result;
} ReentrantResultContext;

typedef struct {
    uint8_t *staging_alias;
    const uint8_t *expected_snapshot;
    const uint8_t *staging_replacement;
    size_t content_size;
    size_t calls;
    bool saw_distinct_snapshot;
    bool snapshot_remained_stable;
} StagingAliasMutationContext;

static MeisoResourceDescriptor blob_descriptor(size_t max_payload_size)
{
    MeisoResourceDescriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id.high = 0xB10BU;
    descriptor.id.low = 1U;
    descriptor.kind = MEISO_RESOURCE_KIND_BLOB;
    descriptor.schema.id.high = 0x5C4EU;
    descriptor.schema.id.low = 2U;
    descriptor.schema.version = 1U;
    descriptor.owner = MEISO_OWNER_ROLE_HOST;
    descriptor.max_payload_size = max_payload_size;
    return descriptor;
}

static MeisoContentId content_id(uint8_t seed)
{
    MeisoContentId value;
    size_t index;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        value.digest[index] = (uint8_t)(seed + (uint8_t)index);
    }
    return value;
}

static bool verify_expected(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    VerifyContext *verify = context;
    size_t index;

    verify->calls += 1U;
    if (!meiso_content_id_equal(expected_content_id, &verify->expected_id) ||
        staged_content.size != verify->expected_size ||
        (staged_content.size != 0U &&
         memcmp(
             staged_content.data,
             verify->expected_content,
             staged_content.size
         ) != 0)) {
        return false;
    }
    if (verify->require_canonical_id) {
        for (index = MEISO_SHA256_DIGEST_SIZE;
             index < MEISO_CONTENT_DIGEST_MAX_SIZE;
             ++index) {
            if (expected_content_id->digest[index] != 0U) {
                return false;
            }
        }
    }
    return true;
}

static bool verify_always(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    size_t *calls = context;

    (void)expected_content_id;
    (void)staged_content;
    if (calls != NULL) {
        *calls += 1U;
    }
    return true;
}

static bool verify_reentrant(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    ReentrantContext *reentrant = context;
    MeisoMutableBuffer buffer;

    (void)expected_content_id;
    reentrant->calls += 1U;
    reentrant->stage_status = meiso_blob_store_stage_begin(
        reentrant->store, &reentrant->content_id, staged_content.size
    );
    reentrant->abort_status = meiso_blob_store_abort_staging(
        reentrant->store, MEISO_BLOB_ABORT_CALLER_REQUEST
    );
    reentrant->staging_visible = meiso_blob_store_staging_buffer(
        reentrant->store, &buffer
    );
    reentrant->commit_status = meiso_blob_store_commit_verified(
        reentrant->store, staged_content.size, verify_always, NULL
    );
    return true;
}

static bool verify_reentrant_with_result(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    ReentrantResultContext *result_context = context;

    (void)verify_reentrant(
        expected_content_id, staged_content, &result_context->reentrant
    );
    return result_context->result;
}

static bool verify_mutates_expected_copy(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    MeisoContentId *mutable_id = (MeisoContentId *)(uintptr_t)
        (const void *)expected_content_id;
    size_t *calls = context;

    (void)staged_content;
    *calls += 1U;
    memset(mutable_id, 0, sizeof(*mutable_id));
    return true;
}

static bool verify_while_mutating_saved_staging_alias(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    StagingAliasMutationContext *mutation = context;

    (void)expected_content_id;
    mutation->calls += 1U;
    if (staged_content.size != mutation->content_size ||
        (staged_content.size != 0U &&
         memcmp(
             staged_content.data,
             mutation->expected_snapshot,
             staged_content.size
         ) != 0)) {
        return false;
    }
    mutation->saw_distinct_snapshot =
        staged_content.size == 0U ||
        staged_content.data != mutation->staging_alias;
    if (mutation->content_size != 0U) {
        memmove(
            mutation->staging_alias,
            mutation->staging_replacement,
            mutation->content_size
        );
    }
    mutation->snapshot_remained_stable =
        staged_content.size == 0U ||
        memcmp(
            staged_content.data,
            mutation->expected_snapshot,
            staged_content.size
        ) == 0;
    return mutation->saw_distinct_snapshot &&
        mutation->snapshot_remained_stable;
}

static bool init_store(
    TestArena *arena,
    const MeisoResourceDescriptor *descriptor,
    const MeisoBlobConfig *config,
    MeisoBlobStore **out_store
)
{
    const size_t required = meiso_blob_store_memory_required(descriptor, config);

    return required != 0U && required <= sizeof(arena->bytes) &&
        meiso_blob_store_init(
            arena->bytes, required, descriptor, config, out_store
        ) == MEISO_BLOB_INIT_OK;
}

static bool test_memory_and_init_contract(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(8U);
    MeisoBlobConfig config = {2U};
    MeisoBlobStore *store = (MeisoBlobStore *)(uintptr_t)1U;
    size_t required = meiso_blob_store_memory_required(&descriptor, &config);
    MeisoResourceDescriptor invalid_descriptor = descriptor;
    MeisoBlobConfig invalid_config = {0U};
    MeisoBlobConfig one_slot_config = {1U};
    size_t index;

    CHECK(required != 0U);
    CHECK(required <= sizeof(arena.bytes));
    CHECK(meiso_blob_alignment_required() == _Alignof(max_align_t));
    invalid_descriptor.kind = MEISO_RESOURCE_KIND_STATE;
    CHECK(meiso_blob_store_memory_required(&invalid_descriptor, &config) == 0U);
    CHECK(meiso_blob_store_memory_required(&descriptor, &invalid_config) == 0U);
    invalid_descriptor = descriptor;
    invalid_descriptor.max_payload_size = SIZE_MAX;
    CHECK(meiso_blob_store_memory_required(&invalid_descriptor, &config) == 0U);
    invalid_descriptor.max_payload_size = SIZE_MAX / 3U;
    CHECK(meiso_blob_store_memory_required(
        &invalid_descriptor, &one_slot_config
    ) == 0U);

    CHECK(meiso_blob_store_init(
        arena.bytes, required, &descriptor, &config, NULL
    ) == MEISO_BLOB_INIT_INVALID_ARGUMENT);
    CHECK(meiso_blob_store_init(
        arena.bytes, required, &invalid_descriptor, &config, &store
    ) == MEISO_BLOB_INIT_ARENA_TOO_SMALL || store == NULL);
    invalid_descriptor.kind = MEISO_RESOURCE_KIND_STATE;
    CHECK(meiso_blob_store_init(
        arena.bytes, required, &invalid_descriptor, &config, &store
    ) == MEISO_BLOB_INIT_INVALID_DESCRIPTOR);
    CHECK(store == NULL);
    CHECK(meiso_blob_store_init(
        arena.bytes, required, &descriptor, &invalid_config, &store
    ) == MEISO_BLOB_INIT_INVALID_CONFIG);
    CHECK(store == NULL);
    CHECK(meiso_blob_store_init(
        arena.bytes + 1U, required, &descriptor, &config, &store
    ) == MEISO_BLOB_INIT_MISALIGNED_ARENA);
    CHECK(store == NULL);
    CHECK(meiso_blob_store_init(
        arena.bytes, required - 1U, &descriptor, &config, &store
    ) == MEISO_BLOB_INIT_ARENA_TOO_SMALL);
    CHECK(store == NULL);

    memset(arena.bytes, 0xA5, sizeof(arena.bytes));
    CHECK(meiso_blob_store_init(
        arena.bytes,
        required,
        &descriptor,
        &config,
        (MeisoBlobStore **)(void *)arena.bytes
    ) == MEISO_BLOB_INIT_INVALID_ARGUMENT);
    for (index = 0U; index < sizeof(MeisoBlobStore *); ++index) {
        CHECK(arena.bytes[index] == 0xA5U);
    }

    {
        MeisoResourceDescriptor *inside_descriptor =
            (MeisoResourceDescriptor *)(void *)arena.bytes;
        const size_t config_offset =
            (sizeof(*inside_descriptor) + _Alignof(MeisoBlobConfig) - 1U) /
            _Alignof(MeisoBlobConfig) * _Alignof(MeisoBlobConfig);
        MeisoBlobConfig *inside_config =
            (MeisoBlobConfig *)(void *)(arena.bytes + config_offset);

        *inside_descriptor = descriptor;
        *inside_config = config;
        required = meiso_blob_store_memory_required(
            inside_descriptor, inside_config
        );
        CHECK(required != 0U);
        CHECK(meiso_blob_store_init(
            arena.bytes,
            required,
            inside_descriptor,
            inside_config,
            &store
        ) == MEISO_BLOB_INIT_OK);
        CHECK(store != NULL);
        CHECK(meiso_blob_store_count(store) == 0U);
    }
    return true;
}

static bool test_incomplete_integrity_and_atomic_publish(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(4U);
    MeisoBlobConfig config = {2U};
    MeisoBlobStore *store;
    MeisoContentId id = content_id(1U);
    MeisoContentId wrong_id = content_id(2U);
    const uint8_t content[] = {'a', 'b', 'c'};
    MeisoMutableBuffer staging;
    MeisoBlobView view;
    VerifyContext wrong_verify = {wrong_id, content, sizeof(content), 0U, false};
    VerifyContext good_verify = {id, content, sizeof(content), 0U, false};
    MeisoBlobStats stats;
    uint8_t *staging_address;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(store, &id, sizeof(content)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(!meiso_blob_store_lookup(store, &id, &view));
    CHECK(meiso_blob_store_count(store) == 0U);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(staging.capacity == sizeof(content));
    staging_address = staging.data;
    memmove(staging.data, content, sizeof(content));

    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(content) - 1U, verify_expected, &good_verify
    ) == MEISO_BLOB_COMMIT_LENGTH_MISMATCH);
    CHECK(good_verify.calls == 0U);
    CHECK(!meiso_blob_store_lookup(store, &id, &view));
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(staging.data == staging_address);
    CHECK(memcmp(staging.data, content, sizeof(content)) == 0);

    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(content), verify_expected, &wrong_verify
    ) == MEISO_BLOB_COMMIT_INTEGRITY_FAILED);
    CHECK(wrong_verify.calls == 1U);
    CHECK(!meiso_blob_store_lookup(store, &id, &view));
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(memcmp(staging.data, content, sizeof(content)) == 0);

    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(content), verify_expected, &good_verify
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(good_verify.calls == 1U);
    CHECK(!meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_count(store) == 1U);
    CHECK(meiso_blob_store_lookup(store, &id, &view));
    CHECK(meiso_content_id_equal(&view.content_id, &id));
    CHECK(view.content.size == sizeof(content));
    CHECK(memcmp(view.content.data, content, sizeof(content)) == 0);
    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(stats.stages_started == 1U);
    CHECK(stats.publications == 1U);
    CHECK(stats.length_mismatches == 1U);
    CHECK(stats.integrity_failures == 1U);
    return true;
}

static bool test_duplicate_conflict_capacity_and_abort(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(4U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId first_id = content_id(10U);
    MeisoContentId second_id = content_id(20U);
    const uint8_t first[] = {'a', 'b', 'c'};
    const uint8_t conflict[] = {'a', 'b', 'd'};
    const uint8_t second[] = {'x', 'y', 'z'};
    MeisoMutableBuffer staging;
    MeisoBlobView original;
    MeisoBlobView after;
    MeisoBlobStats stats;
    size_t verifier_calls = 0U;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(store, &first_id, sizeof(first)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    memmove(staging.data, first, sizeof(first));
    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(first), verify_always, &verifier_calls
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(meiso_blob_store_lookup(store, &first_id, &original));

    CHECK(meiso_blob_store_stage_begin(store, &first_id, sizeof(first)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    memmove(staging.data, first, sizeof(first));
    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(first), verify_always, &verifier_calls
    ) == MEISO_BLOB_COMMIT_DUPLICATE);
    CHECK(meiso_blob_store_count(store) == 1U);
    CHECK(!meiso_blob_store_staging_buffer(store, &staging));

    CHECK(meiso_blob_store_stage_begin(store, &first_id, sizeof(first) - 1U) ==
        MEISO_BLOB_STAGE_CONFLICT);
    CHECK(!meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_count(store) == 1U);

    CHECK(meiso_blob_store_stage_begin(store, &first_id, sizeof(conflict)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    memmove(staging.data, conflict, sizeof(conflict));
    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(conflict), verify_always, &verifier_calls
    ) == MEISO_BLOB_COMMIT_CONFLICT);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_lookup(store, &first_id, &after));
    CHECK(after.content.data == original.content.data);
    CHECK(after.content.size == original.content.size);
    CHECK(memcmp(after.content.data, first, sizeof(first)) == 0);
    CHECK(meiso_blob_store_abort_staging(store, 0U) ==
        MEISO_BLOB_ABORT_INVALID_REASON);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_abort_staging(
        store, MEISO_BLOB_ABORT_CALLER_REQUEST
    ) == MEISO_BLOB_ABORTED);
    CHECK(original.content.size == sizeof(first));
    CHECK(memcmp(original.content.data, first, sizeof(first)) == 0);

    CHECK(meiso_blob_store_stage_begin(store, &second_id, sizeof(second)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    memmove(staging.data, second, sizeof(second));
    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(second), verify_always, &verifier_calls
    ) == MEISO_BLOB_COMMIT_CAPACITY);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_lookup(store, &first_id, &after));
    CHECK(after.content.data == original.content.data);
    CHECK(memcmp(after.content.data, first, sizeof(first)) == 0);
    CHECK(meiso_blob_store_abort_staging(
        store, MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY
    ) == MEISO_BLOB_ABORTED);
    CHECK(original.content.size == sizeof(first));
    CHECK(memcmp(original.content.data, first, sizeof(first)) == 0);
    CHECK(meiso_blob_store_lookup(store, &first_id, &after));
    CHECK(memcmp(after.content.data, first, sizeof(first)) == 0);
    CHECK(!meiso_blob_store_lookup(store, &second_id, &after));
    CHECK(meiso_blob_store_count(store) == 1U);
    CHECK(verifier_calls == 4U);

    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(stats.publications == 1U);
    CHECK(stats.duplicates == 1U);
    CHECK(stats.conflicts == 2U);
    CHECK(stats.capacity_failures == 1U);
    CHECK(stats.aborts == 2U);
    CHECK(stats.caller_aborts == 1U);
    CHECK(stats.transfer_aborts == 1U);
    CHECK(stats.shutdown_aborts == 0U);
    return true;
}

static bool test_invalid_inputs_and_busy_staging(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(4U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId valid_id = content_id(30U);
    MeisoContentId other_id = content_id(31U);
    MeisoContentId invalid_algorithm = valid_id;
    MeisoContentId invalid_digest = valid_id;
    MeisoMutableBuffer staging;
    MeisoBlobStats stats;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    invalid_algorithm.algorithm = 99U;
    invalid_digest.digest_size = MEISO_SHA256_DIGEST_SIZE - 1U;
    CHECK(meiso_blob_store_stage_begin(store, &invalid_algorithm, 1U) ==
        MEISO_BLOB_STAGE_INVALID_CONTENT_ID);
    CHECK(meiso_blob_store_stage_begin(store, &invalid_digest, 1U) ==
        MEISO_BLOB_STAGE_INVALID_CONTENT_ID);
    CHECK(meiso_blob_store_stage_begin(store, NULL, 1U) ==
        MEISO_BLOB_STAGE_INVALID_CONTENT_ID);
    CHECK(meiso_blob_store_stage_begin(store, &valid_id, 5U) ==
        MEISO_BLOB_STAGE_CAPACITY);
    CHECK(!meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_commit_verified(
        store, 0U, verify_always, NULL
    ) == MEISO_BLOB_COMMIT_NO_STAGING);
    CHECK(meiso_blob_store_abort_staging(
        store, MEISO_BLOB_ABORT_STORE_SHUTDOWN
    ) == MEISO_BLOB_ABORT_NO_STAGING);

    CHECK(meiso_blob_store_stage_begin(store, &valid_id, 1U) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_stage_begin(store, &valid_id, 1U) ==
        MEISO_BLOB_STAGE_UNCHANGED);
    CHECK(meiso_blob_store_stage_begin(store, &other_id, 1U) ==
        MEISO_BLOB_STAGE_BUSY);
    CHECK(meiso_blob_store_stage_begin(store, &valid_id, 2U) ==
        MEISO_BLOB_STAGE_BUSY);
    CHECK(meiso_blob_store_commit_verified(store, 1U, NULL, NULL) ==
        MEISO_BLOB_COMMIT_INVALID);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(meiso_blob_store_abort_staging(
        store, MEISO_BLOB_ABORT_STORE_SHUTDOWN
    ) == MEISO_BLOB_ABORTED);
    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(stats.invalid_content_ids == 3U);
    CHECK(stats.capacity_failures == 1U);
    CHECK(stats.stage_busy == 2U);
    CHECK(stats.stages_started == 1U);
    CHECK(stats.aborts == 1U);
    CHECK(stats.shutdown_aborts == 1U);
    CHECK(meiso_blob_store_count(NULL) == 0U);
    CHECK(!meiso_blob_store_stats(NULL, &stats));
    CHECK(!meiso_blob_store_stats(store, NULL));
    return true;
}

static bool test_zero_length_and_canonical_identity(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(0U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId id = content_id(40U);
    MeisoContentId equivalent;
    MeisoMutableBuffer staging;
    MeisoBlobView view;
    VerifyContext verify;
    size_t index;

    for (index = MEISO_SHA256_DIGEST_SIZE;
         index < MEISO_CONTENT_DIGEST_MAX_SIZE;
         ++index) {
        id.digest[index] = 0xA5U;
    }
    equivalent = id;
    for (index = MEISO_SHA256_DIGEST_SIZE;
         index < MEISO_CONTENT_DIGEST_MAX_SIZE;
         ++index) {
        equivalent.digest[index] = 0x5AU;
    }
    memset(&verify, 0, sizeof(verify));
    verify.expected_id = equivalent;
    verify.expected_content = NULL;
    verify.expected_size = 0U;
    verify.require_canonical_id = true;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(store, &id, 0U) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    CHECK(staging.capacity == 0U);
    CHECK(meiso_mutable_buffer_valid(staging));
    CHECK(meiso_blob_store_commit_verified(
        store, 0U, verify_expected, &verify
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(verify.calls == 1U);
    CHECK(meiso_blob_store_lookup(store, &equivalent, &view));
    CHECK(view.content.size == 0U);
    CHECK(meiso_byte_view_valid(view.content));
    for (index = MEISO_SHA256_DIGEST_SIZE;
         index < MEISO_CONTENT_DIGEST_MAX_SIZE;
         ++index) {
        CHECK(view.content_id.digest[index] == 0U);
    }
    return true;
}

static bool test_verifier_reentrancy_guard(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(3U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId id = content_id(50U);
    const uint8_t content[] = {'r', 'a', 'i'};
    MeisoMutableBuffer staging;
    MeisoBlobView view;
    MeisoBlobStats stats;
    ReentrantContext reentrant;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(store, &id, sizeof(content)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    memmove(staging.data, content, sizeof(content));
    memset(&reentrant, 0, sizeof(reentrant));
    reentrant.store = store;
    reentrant.content_id = id;

    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(content), verify_reentrant, &reentrant
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(reentrant.calls == 1U);
    CHECK(reentrant.stage_status == MEISO_BLOB_STAGE_REENTRANT);
    CHECK(reentrant.commit_status == MEISO_BLOB_COMMIT_REENTRANT);
    CHECK(reentrant.abort_status == MEISO_BLOB_ABORT_REENTRANT);
    CHECK(!reentrant.staging_visible);
    CHECK(meiso_blob_store_lookup(store, &id, &view));
    CHECK(memcmp(view.content.data, content, sizeof(content)) == 0);
    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(stats.reentrant_rejections == 4U);
    return true;
}

static bool test_failed_reentrant_verifier_retains_staging(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(2U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId id = content_id(55U);
    const uint8_t content[] = {'n', 'o'};
    MeisoMutableBuffer before;
    MeisoMutableBuffer after;
    MeisoBlobStats stats;
    ReentrantResultContext context;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(store, &id, sizeof(content)) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &before));
    memmove(before.data, content, sizeof(content));
    memset(&context, 0, sizeof(context));
    context.reentrant.store = store;
    context.reentrant.content_id = id;
    context.result = false;
    CHECK(meiso_blob_store_commit_verified(
        store, sizeof(content), verify_reentrant_with_result, &context
    ) == MEISO_BLOB_COMMIT_INTEGRITY_FAILED);
    CHECK(meiso_blob_store_staging_buffer(store, &after));
    CHECK(after.data == before.data && after.capacity == before.capacity);
    CHECK(memcmp(after.data, content, sizeof(content)) == 0);
    CHECK(meiso_blob_store_count(store) == 0U);
    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(stats.integrity_failures == 1U);
    CHECK(stats.reentrant_rejections == 4U);
    return true;
}

static bool test_verifier_cannot_rewrite_staged_identity(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(1U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId id = content_id(60U);
    MeisoContentId zero_id;
    MeisoMutableBuffer staging;
    MeisoBlobView view;
    size_t calls = 0U;

    memset(&zero_id, 0, sizeof(zero_id));
    zero_id.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    zero_id.digest_size = MEISO_SHA256_DIGEST_SIZE;
    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(store, &id, 1U) ==
        MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    staging.data[0] = 0xA5U;
    CHECK(meiso_blob_store_commit_verified(
        store, 1U, verify_mutates_expected_copy, &calls
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(calls == 1U);
    CHECK(meiso_blob_store_lookup(store, &id, &view));
    CHECK(view.content.size == 1U && view.content.data[0] == 0xA5U);
    CHECK(!meiso_blob_store_lookup(store, &zero_id, &view));
    return true;
}

static bool test_verifier_snapshot_isolated_from_saved_staging_alias(void)
{
    TestArena arena;
    MeisoResourceDescriptor descriptor = blob_descriptor(4U);
    MeisoBlobConfig config = {1U};
    MeisoBlobStore *store;
    MeisoContentId id = content_id(70U);
    const uint8_t verified_content[] = {'g', 'o', 'o', 'd'};
    const uint8_t staging_replacement[] = {'e', 'v', 'i', 'l'};
    MeisoMutableBuffer staging;
    MeisoBlobView published;
    StagingAliasMutationContext mutation;

    CHECK(init_store(&arena, &descriptor, &config, &store));
    CHECK(meiso_blob_store_stage_begin(
        store, &id, sizeof(verified_content)
    ) == MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(store, &staging));
    memmove(staging.data, verified_content, sizeof(verified_content));

    memset(&mutation, 0, sizeof(mutation));
    mutation.staging_alias = staging.data;
    mutation.expected_snapshot = verified_content;
    mutation.staging_replacement = staging_replacement;
    mutation.content_size = sizeof(verified_content);
    CHECK(meiso_blob_store_commit_verified(
        store,
        sizeof(verified_content),
        verify_while_mutating_saved_staging_alias,
        &mutation
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(mutation.calls == 1U);
    CHECK(mutation.saw_distinct_snapshot);
    CHECK(mutation.snapshot_remained_stable);
    CHECK(meiso_blob_store_lookup(store, &id, &published));
    CHECK(published.content.size == sizeof(verified_content));
    CHECK(memcmp(
        published.content.data,
        verified_content,
        sizeof(verified_content)
    ) == 0);
    CHECK(memcmp(
        published.content.data,
        staging_replacement,
        sizeof(staging_replacement)
    ) != 0);
    return true;
}

int main(void)
{
    if (!test_memory_and_init_contract() ||
        !test_incomplete_integrity_and_atomic_publish() ||
        !test_duplicate_conflict_capacity_and_abort() ||
        !test_invalid_inputs_and_busy_staging() ||
        !test_zero_length_and_canonical_identity() ||
        !test_verifier_reentrancy_guard() ||
        !test_failed_reentrant_verifier_retains_staging() ||
        !test_verifier_cannot_rewrite_staged_identity() ||
        !test_verifier_snapshot_isolated_from_saved_staging_alias()) {
        return 1;
    }
    (void)puts("blob tests passed");
    return 0;
}
