#ifndef MEISO_ACTION_PRIVATE_H
#define MEISO_ACTION_PRIVATE_H

#include <meiso/core/action.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_ACTION_PRIVATE_API __attribute__((visibility("hidden")))
#else
#define MEISO_ACTION_PRIVATE_API
#endif

/* One arena-resident request/result record; payload bytes follow the header. */
typedef struct {
    MeisoActionId id;
    MeisoOwnerScope expected_executor;
    MeisoResourceId target;
    MeisoSchemaRef operation;
    MeisoDeadline sender_deadline;
    MeisoTimePoint mapped_local_deadline;
    MeisoDeliveryGeneration delivery;
    size_t argument_size;
    size_t result_size;
    MeisoActionRetryPolicy retry_policy;
    MeisoActionState state;
    bool delivery_accepted;
    bool has_mapped_deadline;
} MeisoActionPrivateSlot;

/*
 * Build-only Action representations. Each handle owns records and scratch in
 * one caller-owned arena. Request/result payloads are copied on successful
 * mutation; query/offer/application views borrow arena bytes until the next
 * successful mutation. Field order is the arena ABI and must not move.
 */
struct MeisoActionCaller {
    MeisoResourceDescriptor descriptor;
    MeisoActionConfig config;
    MeisoDeliveryGeneration delivery;
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    uint32_t count;
    MeisoActionCallerStats stats;
    bool delivery_active;
};

struct MeisoActionExecutor {
    MeisoResourceDescriptor descriptor;
    MeisoActionConfig config;
    MeisoOwnerScope executor;
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    uint32_t count;
    MeisoActionExecutorStats stats;
};

MEISO_ACTION_PRIVATE_API bool meiso_action_private_output_overlaps_arena(
    const void *arena,
    size_t arena_size,
    const void *output,
    size_t output_size
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_descriptor_valid(
    const MeisoResourceDescriptor *descriptor
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_config_valid(
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_terminal_state(
    MeisoActionState state
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_request_valid(
    const MeisoActionRequest *request
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_result_valid(
    const MeisoActionResult *result
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_bytes_equal(
    const uint8_t *stored,
    size_t stored_size,
    MeisoByteView candidate
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_layout_required(
    size_t handle_size,
    const MeisoActionConfig *config,
    size_t *out_storage_offset,
    size_t *out_slot_stride,
    size_t *out_scratch_offset,
    size_t *out_total
);
static inline bool meiso_action_private_arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % meiso_action_alignment_required() == 0U;
}
MEISO_ACTION_PRIVATE_API MeisoActionPrivateSlot *meiso_action_private_slot_at(
    const void *handle,
    size_t storage_offset,
    size_t slot_stride,
    uint32_t index
);
MEISO_ACTION_PRIVATE_API uint8_t *meiso_action_private_slot_arguments(
    MeisoActionPrivateSlot *slot
);
MEISO_ACTION_PRIVATE_API uint8_t *meiso_action_private_slot_result(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot
);
MEISO_ACTION_PRIVATE_API bool meiso_action_private_request_equal(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot,
    const MeisoActionRequest *request
);
MEISO_ACTION_PRIVATE_API void meiso_action_private_fill_slot_from_staged_request(
    const MeisoActionConfig *config,
    size_t slot_stride,
    MeisoActionPrivateSlot *slot,
    const MeisoActionRequest *request,
    const uint8_t *staged,
    MeisoDeliveryGeneration delivery,
    MeisoActionState state
);
MEISO_ACTION_PRIVATE_API void meiso_action_private_request_view(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot,
    MeisoActionRequest *out_request
);
MEISO_ACTION_PRIVATE_API void meiso_action_private_entry_view(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot,
    MeisoActionEntryView *out_view
);

#endif
