#ifndef MEISO_CORE_BLOB_H
#define MEISO_CORE_BLOB_H

#include <meiso/core/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoBlobStore MeisoBlobStore;

typedef struct {
    uint32_t published_slot_count;
} MeisoBlobConfig;

typedef struct {
    MeisoContentId content_id;
    MeisoByteView content;
} MeisoBlobView;

/*
 * The verifier receives an immutable verification snapshot, not the mutable
 * staging storage. For non-empty content the snapshot does not alias staging
 * or published storage. The same snapshot bytes are used for the publication
 * decision and any resulting publication.
 *
 * The expected ContentId is canonicalized by Core. Both borrowed arguments
 * are valid only for the duration of the callback. The callback must treat
 * staged_content as const, must not retain or access either borrow after it
 * returns, and must not call a mutating or staging-access Blob API for the
 * same store. Writing through a cast of staged_content.data is outside the
 * verifier contract.
 */
typedef bool (*MeisoBlobVerifier)(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
);

typedef uint8_t MeisoBlobAbortReason;
enum {
    MEISO_BLOB_ABORT_CALLER_REQUEST = 1,
    MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY = 2,
    MEISO_BLOB_ABORT_STORE_SHUTDOWN = 3
};

typedef struct {
    uint64_t stages_started;
    uint64_t stage_busy;
    uint64_t publications;
    uint64_t duplicates;
    uint64_t conflicts;
    uint64_t invalid_content_ids;
    uint64_t length_mismatches;
    uint64_t integrity_failures;
    uint64_t capacity_failures;
    uint64_t aborts;
    uint64_t caller_aborts;
    uint64_t transfer_aborts;
    uint64_t shutdown_aborts;
    uint64_t reentrant_rejections;
} MeisoBlobStats;

typedef uint8_t MeisoBlobInitStatus;
enum {
    MEISO_BLOB_INIT_OK = 0,
    MEISO_BLOB_INIT_INVALID_ARGUMENT = 1,
    MEISO_BLOB_INIT_INVALID_DESCRIPTOR = 2,
    MEISO_BLOB_INIT_INVALID_CONFIG = 3,
    MEISO_BLOB_INIT_MISALIGNED_ARENA = 4,
    MEISO_BLOB_INIT_ARENA_TOO_SMALL = 5
};

typedef uint8_t MeisoBlobStageStatus;
enum {
    MEISO_BLOB_STAGE_STARTED = 0,
    MEISO_BLOB_STAGE_UNCHANGED = 1,
    MEISO_BLOB_STAGE_BUSY = 2,
    MEISO_BLOB_STAGE_CONFLICT = 3,
    MEISO_BLOB_STAGE_INVALID_CONTENT_ID = 4,
    MEISO_BLOB_STAGE_CAPACITY = 5,
    MEISO_BLOB_STAGE_REENTRANT = 6,
    MEISO_BLOB_STAGE_INVALID = 7
};

typedef uint8_t MeisoBlobCommitStatus;
enum {
    MEISO_BLOB_COMMIT_PUBLISHED = 0,
    MEISO_BLOB_COMMIT_DUPLICATE = 1,
    MEISO_BLOB_COMMIT_CONFLICT = 2,
    MEISO_BLOB_COMMIT_NO_STAGING = 3,
    MEISO_BLOB_COMMIT_LENGTH_MISMATCH = 4,
    MEISO_BLOB_COMMIT_INTEGRITY_FAILED = 5,
    MEISO_BLOB_COMMIT_CAPACITY = 6,
    MEISO_BLOB_COMMIT_REENTRANT = 7,
    MEISO_BLOB_COMMIT_INVALID = 8
};

typedef uint8_t MeisoBlobAbortStatus;
enum {
    MEISO_BLOB_ABORTED = 0,
    MEISO_BLOB_ABORT_NO_STAGING = 1,
    MEISO_BLOB_ABORT_REENTRANT = 2,
    MEISO_BLOB_ABORT_INVALID_REASON = 3,
    MEISO_BLOB_ABORT_INVALID = 4
};

size_t meiso_blob_alignment_required(void);
/*
 * Required storage includes independent max-payload staging and verification
 * regions in addition to the configured immutable publication slots.
 */
size_t meiso_blob_store_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoBlobConfig *config
);
MeisoBlobInitStatus meiso_blob_store_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoBlobConfig *config,
    MeisoBlobStore **out_store
);
MeisoBlobStageStatus meiso_blob_store_stage_begin(
    MeisoBlobStore *store,
    const MeisoContentId *content_id,
    size_t content_length
);
bool meiso_blob_store_staging_buffer(
    MeisoBlobStore *store,
    MeisoMutableBuffer *out_buffer
);
/*
 * A staging buffer borrow remains valid until the next successful publish,
 * duplicate commit, or abort on this store. Length mismatch, integrity
 * failure, conflict, capacity, invalid input, and reentrant rejection retain
 * the staging candidate and do not invalidate that borrow. During verifier
 * execution, writes through a previously obtained staging borrow do not alter
 * that commit's verification snapshot; they remain in staging for a later
 * retry if the current commit does not consume the candidate.
 */
MeisoBlobCommitStatus meiso_blob_store_commit_verified(
    MeisoBlobStore *store,
    size_t written_size,
    MeisoBlobVerifier verifier,
    void *verifier_context
);
MeisoBlobAbortStatus meiso_blob_store_abort_staging(
    MeisoBlobStore *store,
    MeisoBlobAbortReason reason
);
/*
 * A successful lookup borrows immutable published storage for the lifetime
 * of this store's arena. v0 never releases or evicts a published slot.
 */
bool meiso_blob_store_lookup(
    const MeisoBlobStore *store,
    const MeisoContentId *content_id,
    MeisoBlobView *out_view
);
size_t meiso_blob_store_count(const MeisoBlobStore *store);
bool meiso_blob_store_stats(
    const MeisoBlobStore *store,
    MeisoBlobStats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif
