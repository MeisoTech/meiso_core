#include "meiso_composition_remote_edge_private.h"

/* Remote initializer binds one borrowed HostRuntime to a caller-owned arena. */

#include <stddef.h>
#include <string.h>

size_t meiso_composition_remote_edge_alignment_required(void)
{
    return _Alignof(MeisoEdgeHandle);
}

size_t meiso_composition_remote_edge_memory_required(void)
{
    return sizeof(MeisoEdgeHandle);
}

MeisoCompositionRemoteEdgeInitStatus meiso_composition_remote_edge_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionRemoteEdgeConfig *config,
    MeisoEdgeHandle **out_handle
)
{
    MeisoEdgeHandle candidate;
    MeisoCompositionHostRuntimeSnapshot host;
    MeisoEdgeSnapshot edge;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t owned_arena_end;
    uintptr_t host_begin;
    uintptr_t host_end;
    uintptr_t config_begin;
    uintptr_t config_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    uintptr_t runtime_begin;
    uintptr_t runtime_end;
    size_t index;

    if (arena == NULL || config == NULL || out_handle == NULL ||
        config->host_runtime == NULL || config->host_runtime_arena == NULL ||
        config->host_runtime_arena_size == 0U ||
        config->catalog_generation == 0U ||
        config->continuity_serial == 0U ||
        !meiso_composition_edge_private_checked_range(
            arena, arena_size, &arena_begin, &arena_end
        ) ||
        !meiso_composition_edge_private_checked_range(
            config->host_runtime_arena,
            config->host_runtime_arena_size,
            &host_begin,
            &host_end
        ) ||
        !meiso_composition_edge_private_checked_range(
            config, sizeof(*config), &config_begin, &config_end
        ) ||
        !meiso_composition_edge_private_checked_range(
            out_handle, sizeof(*out_handle), &output_begin, &output_end
        ) ||
        !meiso_composition_edge_private_checked_range(
            config->host_runtime, 1U, &runtime_begin, &runtime_end
        ) ||
        !meiso_composition_edge_private_range_inside(
            host_begin, host_end, runtime_begin, runtime_end
        ) ||
        meiso_composition_edge_private_ranges_overlap(
            host_begin, host_end, config_begin, config_end
        ) ||
        meiso_composition_edge_private_ranges_overlap(
            host_begin, host_end, output_begin, output_end
        ) ||
        meiso_composition_edge_private_ranges_overlap(
            config_begin, config_end, output_begin, output_end
        )) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_ARGUMENT;
    }
    if (((uintptr_t)arena % _Alignof(MeisoEdgeHandle)) != 0U) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(MeisoEdgeHandle)) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_ARENA_TOO_SMALL;
    }
    owned_arena_end = arena_begin + sizeof(MeisoEdgeHandle);
    if (meiso_composition_edge_private_ranges_overlap(
            arena_begin, owned_arena_end, host_begin, host_end
        ) ||
        meiso_composition_edge_private_ranges_overlap(
            arena_begin, owned_arena_end, config_begin, config_end
        ) ||
        meiso_composition_edge_private_ranges_overlap(
            arena_begin, owned_arena_end, output_begin, output_end
        )) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_ARGUMENT;
    }

    memset(&candidate, 0, sizeof(candidate));
    if (meiso_composition_profile_build_catalog(
            &config->catalog,
            (MeisoMutableBuffer){
                candidate.dynamic_capability,
                sizeof(candidate.dynamic_capability)
            },
            candidate.endpoints,
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            candidate.routes,
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) != MEISO_COMPOSITION_PROFILE_OK) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_CONFIG;
    }
    if (!meiso_composition_host_runtime_snapshot(
            config->host_runtime, &host
        )) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_INTERNAL_FAILURE;
    }
    candidate.magic = MEISO_COMPOSITION_EDGE_HANDLE_MAGIC;
    candidate.backend_kind = MEISO_COMPOSITION_EDGE_BACKEND_HOST;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = owned_arena_end;
    candidate.host_arena_begin = host_begin;
    candidate.host_arena_end = host_end;
    candidate.backend_ops =
        &meiso_composition_remote_edge_private_backend_ops;
    candidate.backend_runtime.remote_host_runtime = config->host_runtime;
    candidate.catalog_generation = config->catalog_generation;
    candidate.continuity_serial = config->continuity_serial;
    candidate.profile = *meiso_composition_profile_ref();
    if (!meiso_composition_remote_edge_private_base_snapshot(
            &candidate, &host, &edge
        )) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_INTERNAL_FAILURE;
    }
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_REMOTE_EDGE_INIT_BACKEND_TERMINAL;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT;
         ++index) {
        const uintptr_t capability_begin =
            (uintptr_t)candidate.endpoints[index].capability.data;
        const uintptr_t dynamic_begin =
            (uintptr_t)candidate.dynamic_capability;
        const uintptr_t dynamic_end =
            dynamic_begin + sizeof(candidate.dynamic_capability);
        if (candidate.endpoints[index].capability.size != 0U &&
            capability_begin >= dynamic_begin &&
            capability_begin < dynamic_end) {
            const size_t offset =
                (size_t)(capability_begin - dynamic_begin);
            if (offset > sizeof(candidate.dynamic_capability) ||
                candidate.endpoints[index].capability.size >
                    sizeof(candidate.dynamic_capability) - offset) {
                return MEISO_COMPOSITION_REMOTE_EDGE_INIT_INTERNAL_FAILURE;
            }
            candidate.endpoints[index].capability.data =
                (const uint8_t *)arena +
                offsetof(MeisoEdgeHandle, dynamic_capability) + offset;
        }
    }
    memset(arena, 0, sizeof(candidate));
    memmove(arena, &candidate, sizeof(candidate));
    *out_handle = (MeisoEdgeHandle *)arena;
    return MEISO_COMPOSITION_REMOTE_EDGE_INIT_OK;
}
