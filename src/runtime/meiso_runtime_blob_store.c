#include "meiso_runtime_private.h"

#include <string.h>

/* SHA-256 verification and the synchronous BlobStore publication bridge. */

typedef struct {
    uint32_t state[8];
    uint8_t block[64];
    uint64_t total_bytes;
    size_t block_size;
} MeisoRuntimeSha256Context;

static uint32_t runtime_rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static void runtime_sha256_transform(
    MeisoRuntimeSha256Context *sha,
    const uint8_t block[64]
)
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = meiso_runtime_private_get_u32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t s0 =
            runtime_rotate_right(words[index - 15U], 7U) ^
            runtime_rotate_right(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t s1 =
            runtime_rotate_right(words[index - 2U], 17U) ^
            runtime_rotate_right(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 +
            words[index - 7U] + s1;
    }
    a = sha->state[0];
    b = sha->state[1];
    c = sha->state[2];
    d = sha->state[3];
    e = sha->state[4];
    f = sha->state[5];
    g = sha->state[6];
    h = sha->state[7];
    for (index = 0U; index < 64U; ++index) {
        const uint32_t s1 = runtime_rotate_right(e, 6U) ^
            runtime_rotate_right(e, 11U) ^ runtime_rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choice + constants[index] + words[index];
        const uint32_t s0 = runtime_rotate_right(a, 2U) ^
            runtime_rotate_right(a, 13U) ^ runtime_rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void runtime_sha256_init(MeisoRuntimeSha256Context *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memmove(sha->state, initial, sizeof(initial));
}

static void runtime_sha256_update(
    MeisoRuntimeSha256Context *sha,
    const uint8_t *data,
    size_t size
)
{
    while (size != 0U) {
        const size_t available = sizeof(sha->block) - sha->block_size;
        const size_t take = size < available ? size : available;

        memmove(sha->block + sha->block_size, data, take);
        sha->block_size += take;
        sha->total_bytes += take;
        data += take;
        size -= take;
        if (sha->block_size == sizeof(sha->block)) {
            runtime_sha256_transform(sha, sha->block);
            sha->block_size = 0U;
        }
    }
}

static void runtime_sha256_final(
    MeisoRuntimeSha256Context *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    const uint64_t total_bits = sha->total_bytes * 8U;
    size_t index;

    sha->block[sha->block_size++] = 0x80U;
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        runtime_sha256_transform(sha, sha->block);
        sha->block_size = 0U;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (index = 0U; index < 8U; ++index) {
        sha->block[56U + index] =
            (uint8_t)(total_bits >> (56U - index * 8U));
    }
    runtime_sha256_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        meiso_runtime_private_put_u32(digest + index * 4U, sha->state[index]);
    }
}

bool meiso_runtime_blob_sha256_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    MeisoRuntimeSha256Context sha;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    (void)context;
    if (!meiso_content_id_valid(expected_content_id) ||
        !meiso_byte_view_valid(content)) {
        return false;
    }
    runtime_sha256_init(&sha);
    runtime_sha256_update(&sha, content.data, content.size);
    runtime_sha256_final(&sha, digest);
    return memcmp(
        digest,
        expected_content_id->digest,
        MEISO_SHA256_DIGEST_SIZE
    ) == 0;
}

static MeisoRuntimeBlobResult blob_stage_result(MeisoBlobStageStatus status)
{
    switch (status) {
    case MEISO_BLOB_STAGE_UNCHANGED:
    case MEISO_BLOB_STAGE_BUSY:
        return MEISO_RUNTIME_BLOB_RESULT_BUSY;
    case MEISO_BLOB_STAGE_CONFLICT:
        return MEISO_RUNTIME_BLOB_RESULT_CONFLICT;
    case MEISO_BLOB_STAGE_INVALID_CONTENT_ID:
        return MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID;
    case MEISO_BLOB_STAGE_CAPACITY:
        return MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
    default:
        return MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
    }
}

static MeisoRuntimeBlobResult blob_commit_result(MeisoBlobCommitStatus status)
{
    switch (status) {
    case MEISO_BLOB_COMMIT_PUBLISHED:
        return MEISO_RUNTIME_BLOB_RESULT_PUBLISHED;
    case MEISO_BLOB_COMMIT_DUPLICATE:
        return MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
    case MEISO_BLOB_COMMIT_CONFLICT:
        return MEISO_RUNTIME_BLOB_RESULT_CONFLICT;
    case MEISO_BLOB_COMMIT_LENGTH_MISMATCH:
        return MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH;
    case MEISO_BLOB_COMMIT_INTEGRITY_FAILED:
        return MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED;
    case MEISO_BLOB_COMMIT_CAPACITY:
        return MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
    default:
        return MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
    }
}

typedef struct {
    MeisoBlobVerifier verifier;
    void *context;
} MeisoRuntimeBlobVerifierContext;

static bool runtime_blob_combined_verifier(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    MeisoRuntimeBlobVerifierContext *combined = context;

    return combined != NULL && combined->verifier != NULL &&
        meiso_runtime_blob_sha256_verify(
            expected_content_id, content, NULL
        ) && combined->verifier(
            expected_content_id, content, combined->context
        );
}

MeisoRuntimeStatus meiso_runtime_publish_blob_receive(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable_internal(runtime, true);
    MeisoRuntimeBlobPublishReport report;
    MeisoRuntimeBlobVerifierContext combined;
    MeisoMutableBuffer staging;
    MeisoBlobStageStatus stage_status;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!meiso_runtime_private_blob_wire_supported(runtime)) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (expected_transfer == NULL || store == NULL || verifier == NULL ||
        out_report == NULL || meiso_runtime_private_ranges_overlap(
            runtime,
            runtime->arena_size,
            expected_transfer,
            sizeof(*expected_transfer)
        ) || meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_report, sizeof(*out_report)
        ) || meiso_runtime_private_ranges_overlap(
            expected_transfer,
            sizeof(*expected_transfer),
            out_report,
            sizeof(*out_report)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->blob_transfer_active || !runtime->blob_publish_pending) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (runtime->blob_result_pending ||
        !meiso_runtime_private_blob_transfer_equal(expected_transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    memset(&report, 0, sizeof(report));
    runtime->blob_bridge_active = true;
    report.stage_attempted = true;
    stage_status = meiso_blob_store_stage_begin(
        store,
        &runtime->blob_transfer.content_id,
        (size_t)runtime->blob_transfer.total_size
    );
    report.stage_status = stage_status;
    if (stage_status != MEISO_BLOB_STAGE_STARTED) {
        report.result = blob_stage_result(stage_status);
    } else {
        if (!meiso_blob_store_staging_buffer(store, &staging) ||
            staging.capacity != (size_t)runtime->blob_transfer.total_size) {
            report.commit_status = MEISO_BLOB_COMMIT_INVALID;
            report.result = MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
        } else {
            if (staging.capacity != 0U) {
                memmove(
                    staging.data,
                    meiso_runtime_private_blob_scratch(runtime),
                    staging.capacity
                );
            }
            report.commit_attempted = true;
            combined.verifier = verifier;
            combined.context = verifier_context;
            report.commit_status = meiso_blob_store_commit_verified(
                store,
                staging.capacity,
                runtime_blob_combined_verifier,
                &combined
            );
            report.result = blob_commit_result(report.commit_status);
        }
        if (report.result != MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
            report.result != MEISO_RUNTIME_BLOB_RESULT_DUPLICATE) {
            report.forced_abort_attempted = true;
            report.forced_abort_status = meiso_blob_store_abort_staging(
                store, MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY
            );
            if (report.forced_abort_status != MEISO_BLOB_ABORTED &&
                report.forced_abort_status != MEISO_BLOB_ABORT_NO_STAGING) {
                report.result =
                    MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
            }
        }
    }
    runtime->blob_bridge_active = false;
    runtime->blob_publish_pending = false;
    meiso_runtime_private_set_blob_result(
        runtime,
        &runtime->blob_transfer,
        MEISO_RUNTIME_BLOB_COMMIT,
        report.result,
        runtime->blob_next_offset
    );
    *out_report = report;
    return MEISO_RUNTIME_OK;
}
