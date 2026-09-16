#include "meiso_runtime_private.h"
#include "../core/meiso_sha256_private.h"

#include <string.h>

/* SHA-256 verification and the synchronous BlobStore publication bridge. */

bool meiso_runtime_blob_sha256_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    MeisoSha256 sha;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    (void)context;
    if (!meiso_content_id_valid(expected_content_id) ||
        !meiso_byte_view_valid(content)) {
        return false;
    }
    meiso_sha256_init(&sha);
    meiso_sha256_update(&sha, content.data, content.size);
    meiso_sha256_final(&sha, digest);
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
