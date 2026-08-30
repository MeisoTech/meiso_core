#include "meiso_composition_coordinator_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Coordinator lifecycle owns its caller arena and SceneStore scope binding. */

size_t meiso_composition_coordinator_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_composition_coordinator_memory_required(void)
{
    return sizeof(MeisoCompositionCoordinator);
}

MeisoCompositionCoordinatorInitStatus meiso_composition_coordinator_init(
    void *arena, size_t arena_size,
    const MeisoCompositionCoordinatorConfig *config,
    MeisoCompositionCoordinator **out_coordinator)
{
    MeisoCompositionCoordinatorConfig config_copy;
    MeisoCompositionSceneControlScope scene_scope;
    MeisoCompositionCoordinator *coordinator;

    if (out_coordinator == NULL) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT;
    }
    if (meiso_composition_coordinator_private_ranges_overlap(
            arena, arena_size, out_coordinator, sizeof(*out_coordinator)) ||
        meiso_composition_coordinator_private_ranges_overlap(
            arena, arena_size, config, sizeof(*config)) ||
        meiso_composition_coordinator_private_ranges_overlap(
            config, sizeof(*config), out_coordinator,
            sizeof(*out_coordinator))) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT;
    }
    *out_coordinator = NULL;
    if (arena == NULL || config == NULL || config->scene_store == NULL ||
        !meiso_session_nonce_valid(config->session_id.host_nonce) ||
        !meiso_session_nonce_valid(config->session_id.edge_nonce) ||
        config->edge_incarnation == 0U || config->capability_generation == 0U ||
        config->edge_clock_epoch == 0U ||
        !meiso_composition_scene_store_control_scope(config->scene_store,
                                                     &scene_scope)) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT;
    }
    if ((uintptr_t)arena % meiso_composition_coordinator_alignment_required() !=
        0U) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(*coordinator)) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_ARENA_TOO_SMALL;
    }
    config_copy = *config;
    memset(arena, 0, sizeof(*coordinator));
    coordinator = arena;
    coordinator->magic = MEISO_COMPOSITION_COORDINATOR_MAGIC;
    coordinator->scene_store = config_copy.scene_store;
    coordinator->scene_scope = scene_scope;
    coordinator->session_id = config_copy.session_id;
    coordinator->edge_incarnation = config_copy.edge_incarnation;
    coordinator->capability_generation = config_copy.capability_generation;
    coordinator->edge_clock_epoch = config_copy.edge_clock_epoch;
    *out_coordinator = coordinator;
    return MEISO_COMPOSITION_COORDINATOR_INIT_OK;
}

MeisoCompositionCoordinatorOwnerStatus
meiso_composition_coordinator_begin_owner(
    MeisoCompositionCoordinator *coordinator,
    uint64_t desired_owner_incarnation)
{
    MeisoCompositionSceneOwnerStatus scene_status;

    if (!meiso_composition_coordinator_private_scope_valid(coordinator) ||
        desired_owner_incarnation == 0U) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT;
    }
    if (coordinator->owner_incarnation == desired_owner_incarnation) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_UNCHANGED;
    }
    scene_status = meiso_composition_scene_store_reset_control_scope(
        coordinator->scene_store, desired_owner_incarnation);
    if (scene_status == MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT;
    }
    if (scene_status == MEISO_COMPOSITION_SCENE_OWNER_SCOPE_COUNTER_EXHAUSTED) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_SCOPE_COUNTER_EXHAUSTED;
    }
    if (!meiso_composition_scene_store_control_scope(
            coordinator->scene_store, &coordinator->scene_scope)) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT;
    }
    if (coordinator->owner_incarnation == 0U) {
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.owner_starts);
    } else {
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.owner_changes);
    }
    coordinator->owner_incarnation = desired_owner_incarnation;
    coordinator->plan_generation_high_water = 0U;
    coordinator->plan_generation_identity_present = false;
    memset(coordinator->plan_generation_identity, 0,
           sizeof(coordinator->plan_generation_identity));
    coordinator->has_observation = false;
    coordinator->disable_pending = false;
    memset(&coordinator->observation, 0, sizeof(coordinator->observation));
    coordinator->observation_wire_size = 0U;
    memset(coordinator->observation_wire, 0,
           sizeof(coordinator->observation_wire));
    meiso_composition_coordinator_private_clear_plan(&coordinator->candidate);
    meiso_composition_coordinator_private_clear_plan(&coordinator->active);
    coordinator->has_last_nonce = false;
    memset(coordinator->last_nonce, 0, sizeof(coordinator->last_nonce));
    memset(coordinator->last_token, 0, sizeof(coordinator->last_token));
    return MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED;
}
