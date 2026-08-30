#ifndef MEISO_RUNTIME_SCHEDULER_PRIVATE_H
#define MEISO_RUNTIME_SCHEDULER_PRIVATE_H

#include <meiso/runtime/scheduler.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Build-only representation; field order preserves the original arena ABI. */
#define MEISO_RUNTIME_SCHEDULER_MAGIC UINT64_C(0x4d52545343484432)

enum {
    MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_CONTROL = 1,
    MEISO_RUNTIME_SCHEDULER_SOURCE_CLASS_DATA = 2
};

typedef struct {
    MeisoRuntimeSchedulerPoll poll;
    MeisoRuntimeSchedulerComplete complete;
    MeisoRuntimeSchedulerBoundaryNotify boundary;
    void *context;
    MeisoRuntimeRouteKey route;
    uint64_t round_ticket;
    MeisoRuntimeSchedulerSourceId id;
    MeisoRuntimeSchedulerWorkKind work_kind;
    uint8_t source_class;
    bool route_binding_required;
    bool route_bound;
    bool round_member;
} MeisoRuntimeSchedulerSource;

struct MeisoRuntimeScheduler {
    uint64_t magic;
    size_t arena_size;
    MeisoRuntime *runtime;
    MeisoSession *session;
    MeisoSessionId session_id;
    size_t control_offset;
    size_t data_offset;
    uint64_t catalog_generation;
    uint64_t successful_slots;
    uint64_t successful_control_slots;
    uint64_t successful_data_slots;
    uint64_t data_rounds;
    uint16_t max_control_sources;
    uint16_t max_data_sources;
    uint16_t control_source_count;
    uint16_t data_source_count;
    uint16_t control_cursor;
    uint16_t data_cursor;
    uint16_t data_round_member_count;
    uint8_t successful_control_burst;
    MeisoSessionRole local_role;
    MeisoRuntimeSchedulerPhase phase;
    MeisoRuntimeSchedulerRxObligation rx_obligation;
    bool active_catalog_present;
    bool sealed;
    bool callback_active;
    bool data_probe_due;
    uint8_t wire_version;
};

typedef struct {
    size_t required;
    size_t control_offset;
    size_t data_offset;
} MeisoRuntimeSchedulerArenaLayout;

/* Arena, source registry and route-binding services. */
bool meiso_runtime_scheduler_private_ranges_overlap(
    const void *lhs, size_t lhs_size, const void *rhs, size_t rhs_size
);
MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_control_sources(
    MeisoRuntimeScheduler *scheduler
);
const MeisoRuntimeSchedulerSource *
meiso_runtime_scheduler_private_control_sources_const(
    const MeisoRuntimeScheduler *scheduler
);
MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_data_sources(
    MeisoRuntimeScheduler *scheduler
);
const MeisoRuntimeSchedulerSource *
meiso_runtime_scheduler_private_data_sources_const(
    const MeisoRuntimeScheduler *scheduler
);
bool meiso_runtime_scheduler_private_layout_valid(
    const MeisoRuntimeScheduler *scheduler
);
bool meiso_runtime_scheduler_private_output_overlaps(
    const MeisoRuntimeScheduler *scheduler,
    const void *output,
    size_t output_size
);
bool meiso_runtime_scheduler_private_route_equal(
    MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs
);
bool meiso_runtime_scheduler_private_route_valid(MeisoRuntimeRouteKey route);
bool meiso_runtime_scheduler_private_session_runtime_ready(
    MeisoRuntime *runtime,
    MeisoSession *session,
    MeisoRuntimeSnapshot *out_runtime,
    MeisoSessionSnapshot *out_session
);
MeisoRuntimeSchedulerSource *meiso_runtime_scheduler_private_source_for_id(
    MeisoRuntimeScheduler *scheduler, MeisoRuntimeSchedulerSourceId source_id
);
void meiso_runtime_scheduler_private_clear_data_round(
    MeisoRuntimeScheduler *scheduler
);
void meiso_runtime_scheduler_private_clear_route_bindings(
    MeisoRuntimeScheduler *scheduler
);

/* Session/Runtime reconciliation and scheduler work selection. */
MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_private_refresh_boundary(
    MeisoRuntimeScheduler *scheduler, MeisoSessionSnapshot *out_session
);
bool meiso_runtime_scheduler_private_work_kind_valid(
    MeisoRuntimeSchedulerWorkKind kind
);
uint8_t meiso_runtime_scheduler_private_class_for_work_kind(
    MeisoRuntimeSchedulerWorkKind kind
);
bool meiso_runtime_scheduler_private_work_kind_has_route(
    MeisoRuntimeSchedulerWorkKind kind
);
bool meiso_runtime_scheduler_private_counters_available(
    const MeisoRuntimeScheduler *scheduler,
    uint8_t source_class,
    bool new_data_round
);
void meiso_runtime_scheduler_private_rollback_new_data_round(
    MeisoRuntimeScheduler *scheduler
);
MeisoRuntimeSchedulerStepStatus meiso_runtime_scheduler_private_finish_attempt(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource *source,
    uint16_t source_index,
    const MeisoRuntimeSchedulerWork *work,
    bool new_data_round,
    MeisoRuntimeStatus runtime_status,
    const MeisoRuntimeBlobTransferRef *generated,
    bool generated_present,
    bool data_probe_performed,
    MeisoRuntimeSchedulerStepResult *out_result
);
bool meiso_runtime_scheduler_private_find_control_work(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource **out_source,
    uint16_t *out_index,
    MeisoRuntimeSchedulerWork *out_work,
    bool *out_source_invalid
);
bool meiso_runtime_scheduler_private_begin_data_round(
    MeisoRuntimeScheduler *scheduler, bool *out_source_invalid
);
bool meiso_runtime_scheduler_private_find_data_work(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSource **out_source,
    uint16_t *out_index,
    MeisoRuntimeSchedulerWork *out_work,
    bool *out_source_invalid
);
MeisoRuntimeStatus meiso_runtime_scheduler_private_dispatch_work(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeBlobTransferRef *out_generated,
    bool *out_generated_present
);

#endif
