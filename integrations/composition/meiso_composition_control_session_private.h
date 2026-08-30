#ifndef MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_H
#define MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_H

#include <meiso/composition/control_session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
#endif

#define MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_MAGIC \
    UINT64_C(0x4344504545525630) /* CMPCTRL_ private sentinel */

enum {
    MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_ENDPOINTS =
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
    MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_ROUTES =
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT,
    MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_CAPABILITY_BYTES =
        MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES,
    MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_DESIRED_PROFILE_BYTES =
        MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE,
    MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_ACTUAL_PROFILE_BYTES =
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE,
    MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_PROFILE_MAX_BLOB_TRANSFER_BYTES =
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE
};

typedef struct {
    MeisoCompositionControlSession *owner;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerCompletion completion;
    MeisoRuntimeSchedulerSourceId source_id;
    MeisoRuntimeSchedulerWorkKind work_kind;
    uint8_t route_index;
    uint8_t source_index;
    bool pending;
    bool completion_pending;
    bool internal_catalog;
} MeisoCompositionControlSessionPrivateTxSource;

/*
 * The ControlSession is installed in a caller-owned arena and owns the
 * lifecycle of its embedded Session, adapter, Runtime, Scheduler, and twenty
 * fixed source records. The adapter borrows connected_fd and never closes it;
 * queued work byte views remain caller-owned borrows until completion,
 * boundary, or terminal. Runtime/Scheduler are uninstalled while their
 * *_present flags are false, and their pointers then remain NULL.
 */
struct MeisoCompositionControlSession {
    uint64_t magic;
    size_t arena_size;
    size_t session_offset;
    size_t session_bytes;
    size_t adapter_offset;
    size_t adapter_reserve_bytes;
    size_t adapter_bytes;
    size_t runtime_offset;
    size_t runtime_reserve_bytes;
    size_t runtime_bytes;
    size_t scheduler_offset;
    size_t scheduler_reserve_bytes;
    size_t scheduler_bytes;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
    MeisoRuntimeScheduler *scheduler;
    MeisoSessionRole local_role;
    uint64_t catalog_generation;
    uint64_t next_ticket;
    uint64_t boundary_serial;
    uint64_t last_boundary_generation;
    MeisoRuntimeSchedulerBoundary last_boundary;
    MeisoCompositionControlSessionPhase phase;
    uint8_t catalog_source_index;
    bool runtime_present;
    bool scheduler_present;
    bool terminal;
    bool callback_active;
    uint8_t pump_cursor;
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT
    ];
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
    uint8_t dynamic_capability[
        MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES
    ];
    MeisoCompositionControlSessionPrivateTxSource sources[
        MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT
    ];
};

typedef struct {
    size_t required;
    size_t session_offset;
    size_t session_bytes;
    size_t adapter_offset;
    size_t runtime_offset;
    size_t scheduler_offset;
} MeisoCompositionControlSessionPrivateArenaLayout;

_Static_assert(
    MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT == 20U,
    "ControlSession source topology"
);

MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_kind_is_data(
    MeisoRuntimeSchedulerWorkKind kind
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_kind_has_route(
    MeisoRuntimeSchedulerWorkKind kind
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_source_route_semantics_valid(
    MeisoSessionRole role,
    MeisoRuntimeSchedulerWorkKind kind,
    const MeisoCompositionProfileRouteDescriptor *route
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
void *meiso_composition_control_session_private_arena_at(
    MeisoCompositionControlSession *control_session,
    size_t offset
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_valid(
    const MeisoCompositionControlSession *control_session
);

MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
void meiso_composition_control_session_private_mark_terminal(
    MeisoCompositionControlSession *control_session
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
void meiso_composition_control_session_private_terminate(
    MeisoCompositionControlSession *control_session,
    int error_number
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_queue_internal_catalog(
    MeisoCompositionControlSession *control_session
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_activate_runtime(
    MeisoCompositionControlSession *control_session
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_rebind_sources(
    MeisoCompositionControlSession *control_session
);
MEISO_COMPOSITION_CONTROL_SESSION_PRIVATE_API
bool meiso_composition_control_session_private_staged_catalog_exact(
    MeisoCompositionControlSession *control_session
);

#endif
