#ifndef MEISO_CORE_ACTION_H
#define MEISO_CORE_ACTION_H

#include <meiso/core/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoActionCaller MeisoActionCaller;
typedef struct MeisoActionExecutor MeisoActionExecutor;

typedef uint8_t MeisoActionRetryPolicy;
enum {
    MEISO_ACTION_RETRY_NONE = 1,
    MEISO_ACTION_RETRY_IDEMPOTENT = 2
};

typedef uint8_t MeisoActionState;
enum {
    MEISO_ACTION_STATE_PENDING = 1,
    MEISO_ACTION_STATE_ACCEPTED = 2,
    MEISO_ACTION_STATE_SUCCEEDED = 3,
    MEISO_ACTION_STATE_FAILED = 4,
    MEISO_ACTION_STATE_REJECTED = 5,
    MEISO_ACTION_STATE_EXPIRED = 6,
    MEISO_ACTION_STATE_INDETERMINATE = 7
};

typedef struct {
    uint32_t slot_count;
    size_t max_argument_size;
    size_t max_result_size;
} MeisoActionConfig;

typedef struct {
    MeisoActionId id;
    MeisoOwnerScope expected_executor;
    MeisoResourceId target;
    MeisoSchemaRef operation;
    MeisoByteView arguments;
    MeisoDeadline sender_deadline;
    MeisoActionRetryPolicy retry_policy;
} MeisoActionRequest;

typedef struct {
    uint8_t comparable;
    uint8_t uncertainty_acceptable;
    MeisoTimePoint local_deadline;
    MeisoTimePoint local_now;
} MeisoActionAdmission;

typedef struct {
    MeisoActionId id;
    MeisoOwnerScope executor;
    MeisoActionState state;
    MeisoByteView payload;
} MeisoActionResult;

typedef struct {
    MeisoDeliveryGeneration generation;
    MeisoActionRequest request;
} MeisoActionOffer;

typedef struct {
    MeisoActionRequest request;
    MeisoDeliveryGeneration delivery;
    MeisoActionState state;
    uint8_t delivery_accepted;
    MeisoByteView result;
} MeisoActionEntryView;

/*
 * Request arguments and observation payloads are copied on successful
 * mutation. Views returned by offer/query/application-request borrow arena
 * bytes until the next successful mutation of that same handle.
 */

typedef struct {
    uint64_t delivery_starts;
    uint64_t delivery_ends;
    uint64_t submissions;
    uint64_t duplicates;
    uint64_t conflicts;
    uint64_t argument_capacity_failures;
    uint64_t table_full_failures;
    uint64_t scope_mismatches;
    uint64_t delivery_accepts;
    uint64_t results_recorded;
    uint64_t result_duplicates;
    uint64_t terminal_rejections;
    uint64_t result_capacity_failures;
    uint64_t indeterminate;
} MeisoActionCallerStats;

typedef struct {
    uint64_t ingests;
    uint64_t replays;
    uint64_t conflicts;
    uint64_t expired;
    uint64_t argument_capacity_failures;
    uint64_t table_full_failures;
    uint64_t scope_mismatches;
    uint64_t clock_scope_mismatches;
    uint64_t accepts;
    uint64_t succeeded;
    uint64_t failed;
    uint64_t rejected;
    uint64_t indeterminate;
    uint64_t result_duplicates;
    uint64_t terminal_rejections;
    uint64_t result_capacity_failures;
} MeisoActionExecutorStats;

typedef uint8_t MeisoActionInitStatus;
enum {
    MEISO_ACTION_INIT_OK = 0,
    MEISO_ACTION_INIT_INVALID_ARGUMENT = 1,
    MEISO_ACTION_INIT_INVALID_DESCRIPTOR = 2,
    MEISO_ACTION_INIT_INVALID_CONFIG = 3,
    MEISO_ACTION_INIT_INVALID_EXECUTOR = 4,
    MEISO_ACTION_INIT_MISALIGNED_ARENA = 5,
    MEISO_ACTION_INIT_ARENA_TOO_SMALL = 6
};

typedef uint8_t MeisoActionDeliveryBeginStatus;
enum {
    MEISO_ACTION_DELIVERY_STARTED = 0,
    MEISO_ACTION_DELIVERY_ALREADY_ACTIVE = 1,
    MEISO_ACTION_DELIVERY_STALE_GENERATION = 2,
    MEISO_ACTION_DELIVERY_GENERATION_WRAP = 3,
    MEISO_ACTION_DELIVERY_BEGIN_INVALID = 4
};

typedef uint8_t MeisoActionDeliveryEndStatus;
enum {
    MEISO_ACTION_DELIVERY_ENDED = 0,
    MEISO_ACTION_DELIVERY_ENDED_WITH_UNRESOLVED = 1,
    MEISO_ACTION_DELIVERY_END_MISMATCH = 2,
    MEISO_ACTION_DELIVERY_END_NOT_ACTIVE = 3,
    MEISO_ACTION_DELIVERY_END_INVALID = 4
};

typedef uint8_t MeisoActionSubmitStatus;
enum {
    MEISO_ACTION_SUBMIT_NEW = 0,
    MEISO_ACTION_SUBMIT_DUPLICATE = 1,
    MEISO_ACTION_SUBMIT_CONFLICT = 2,
    MEISO_ACTION_SUBMIT_ARGUMENT_CAPACITY = 3,
    MEISO_ACTION_SUBMIT_TABLE_FULL = 4,
    MEISO_ACTION_SUBMIT_SCOPE_MISMATCH = 5,
    MEISO_ACTION_SUBMIT_NO_ACTIVE_DELIVERY = 6,
    MEISO_ACTION_SUBMIT_INVALID = 7
};

typedef uint8_t MeisoActionDeliveryAcceptStatus;
enum {
    MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED = 0,
    MEISO_ACTION_DELIVERY_ACCEPT_DUPLICATE = 1,
    MEISO_ACTION_DELIVERY_ACCEPT_UNKNOWN = 2,
    MEISO_ACTION_DELIVERY_ACCEPT_DELIVERY_MISMATCH = 3,
    MEISO_ACTION_DELIVERY_ACCEPT_SCOPE_MISMATCH = 4,
    MEISO_ACTION_DELIVERY_ACCEPT_INVALID = 5
};

typedef uint8_t MeisoActionObserveResultStatus;
enum {
    MEISO_ACTION_RESULT_RECORDED = 0,
    MEISO_ACTION_RESULT_DUPLICATE = 1,
    MEISO_ACTION_RESULT_TERMINAL_ALREADY_SET = 2,
    MEISO_ACTION_RESULT_UNKNOWN = 3,
    MEISO_ACTION_RESULT_SCOPE_MISMATCH = 4,
    MEISO_ACTION_RESULT_CAPACITY = 5,
    MEISO_ACTION_RESULT_WRONG_STATE = 6,
    MEISO_ACTION_RESULT_INVALID = 7
};

typedef uint8_t MeisoActionIndeterminateStatus;
enum {
    MEISO_ACTION_INDETERMINATE_MARKED = 0,
    MEISO_ACTION_INDETERMINATE_DUPLICATE = 1,
    MEISO_ACTION_INDETERMINATE_TERMINAL_ALREADY_SET = 2,
    MEISO_ACTION_INDETERMINATE_NOT_ACCEPTED = 3,
    MEISO_ACTION_INDETERMINATE_UNKNOWN = 4,
    MEISO_ACTION_INDETERMINATE_SCOPE_MISMATCH = 5,
    MEISO_ACTION_INDETERMINATE_INVALID = 6
};

typedef uint8_t MeisoActionIngestStatus;
enum {
    MEISO_ACTION_INGEST_NEW = 0,
    MEISO_ACTION_INGEST_REPLAY = 1,
    MEISO_ACTION_INGEST_CONFLICT = 2,
    MEISO_ACTION_INGEST_EXPIRED = 3,
    MEISO_ACTION_INGEST_SCOPE_MISMATCH = 4,
    MEISO_ACTION_INGEST_CLOCK_SCOPE_MISMATCH = 5,
    MEISO_ACTION_INGEST_ARGUMENT_CAPACITY = 6,
    MEISO_ACTION_INGEST_TABLE_FULL = 7,
    MEISO_ACTION_INGEST_INVALID = 8
};

typedef uint8_t MeisoActionExecutorAcceptStatus;
enum {
    MEISO_ACTION_EXECUTOR_ACCEPTED = 0,
    MEISO_ACTION_EXECUTOR_ACCEPT_DUPLICATE = 1,
    MEISO_ACTION_EXECUTOR_ACCEPT_TERMINAL_ALREADY_SET = 2,
    MEISO_ACTION_EXECUTOR_ACCEPT_UNKNOWN = 3,
    MEISO_ACTION_EXECUTOR_ACCEPT_EXPIRED = 4,
    MEISO_ACTION_EXECUTOR_ACCEPT_SCOPE_MISMATCH = 5,
    MEISO_ACTION_EXECUTOR_ACCEPT_CLOCK_SCOPE_MISMATCH = 6,
    MEISO_ACTION_EXECUTOR_ACCEPT_INVALID = 7
};

typedef uint8_t MeisoActionRecordResultStatus;
enum {
    MEISO_ACTION_RECORD_RECORDED = 0,
    MEISO_ACTION_RECORD_DUPLICATE = 1,
    MEISO_ACTION_RECORD_NOT_ACCEPTED = 2,
    MEISO_ACTION_RECORD_WRONG_STATE = 3,
    MEISO_ACTION_RECORD_TERMINAL_ALREADY_SET = 4,
    MEISO_ACTION_RECORD_UNKNOWN = 5,
    MEISO_ACTION_RECORD_SCOPE_MISMATCH = 6,
    MEISO_ACTION_RECORD_CAPACITY = 7,
    MEISO_ACTION_RECORD_INVALID = 8
};

size_t meiso_action_alignment_required(void);

size_t meiso_action_caller_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config
);
MeisoActionInitStatus meiso_action_caller_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config,
    MeisoActionCaller **out_caller
);
MeisoActionDeliveryBeginStatus meiso_action_caller_begin_delivery(
    MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery
);
MeisoActionDeliveryEndStatus meiso_action_caller_end_delivery(
    MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery
);
MeisoActionSubmitStatus meiso_action_caller_submit(
    MeisoActionCaller *caller,
    const MeisoActionRequest *request
);
bool meiso_action_caller_next_offer(
    const MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery,
    size_t *in_out_cursor,
    MeisoActionOffer *out_offer
);
MeisoActionDeliveryAcceptStatus meiso_action_caller_mark_delivery_accepted(
    MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery,
    MeisoActionId id,
    MeisoOwnerScope executor
);
MeisoActionObserveResultStatus meiso_action_caller_observe_result(
    MeisoActionCaller *caller,
    const MeisoActionResult *result
);
MeisoActionIndeterminateStatus meiso_action_caller_mark_indeterminate(
    MeisoActionCaller *caller,
    MeisoActionId id,
    MeisoOwnerScope executor
);
bool meiso_action_caller_query(
    const MeisoActionCaller *caller,
    MeisoActionId id,
    MeisoActionEntryView *out_view
);
bool meiso_action_caller_stats(
    const MeisoActionCaller *caller,
    MeisoActionCallerStats *out_stats
);

size_t meiso_action_executor_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config
);
MeisoActionInitStatus meiso_action_executor_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config,
    MeisoOwnerScope executor,
    MeisoActionExecutor **out_executor
);
MeisoActionIngestStatus meiso_action_executor_ingest(
    MeisoActionExecutor *executor,
    const MeisoActionRequest *request,
    const MeisoActionAdmission *admission
);
bool meiso_action_executor_next_application_request(
    const MeisoActionExecutor *executor,
    size_t *in_out_cursor,
    MeisoActionRequest *out_request
);
/*
 * next_application_request is observation only and may return the same
 * pending request through independent cursors. Exactly one serialized caller
 * may begin side effects, and only after this function returns EXECUTOR_ACCEPTED.
 * ACCEPT_DUPLICATE never grants execution authority.
 */
MeisoActionExecutorAcceptStatus meiso_action_executor_accept(
    MeisoActionExecutor *executor,
    MeisoOwnerScope executor_scope,
    MeisoActionId id,
    MeisoTimePoint comparable_now
);
MeisoActionRecordResultStatus meiso_action_executor_record_result(
    MeisoActionExecutor *executor,
    const MeisoActionResult *result
);
MeisoActionIndeterminateStatus meiso_action_executor_mark_indeterminate(
    MeisoActionExecutor *executor,
    MeisoOwnerScope executor_scope,
    MeisoActionId id
);
bool meiso_action_executor_query(
    const MeisoActionExecutor *executor,
    MeisoActionId id,
    MeisoActionEntryView *out_view
);
bool meiso_action_executor_stats(
    const MeisoActionExecutor *executor,
    MeisoActionExecutorStats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif
