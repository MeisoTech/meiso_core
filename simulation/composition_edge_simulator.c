#include "meiso_composition_edge_simulator_private.h"
#include "meiso_composition_edge_trace_private.h"

/* Simulator lifecycle owns its arena layout and embedded Composition stores. */

#include <stddef.h>
#include <string.h>

static bool meiso_composition_edge_simulator_private_layout(
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoCompositionEdgeSimulatorLayout *out_layout
)
{
    const MeisoResourceDescriptor *blob_descriptor;
    MeisoBlobConfig blob_config;
    MeisoCompositionEdgeSimulatorLayout layout;
    size_t cursor;

    if (config == NULL || out_layout == NULL) {
        return false;
    }
    memset(&layout, 0, sizeof(layout));
    cursor = sizeof(MeisoEdgeHandle);
    layout.render_offset = cursor;
    if (!meiso_composition_edge_private_checked_add_size(
            cursor,
            config->catalog.render_media_capability.max_access_unit_bytes,
            &cursor
        )) {
        return false;
    }
    layout.camera_offset = cursor;
    if (!meiso_composition_edge_private_checked_add_size(
            cursor,
            config->catalog.camera_media_capability.max_access_unit_bytes,
            &cursor
        )) {
        return false;
    }
    if (!config->composition_enabled) {
        if (config->cpu_raster_preview_enabled) {
            return false;
        }
        layout.required = cursor;
        *out_layout = layout;
        return true;
    }
    if (config->composition_blob_slot_count == 0U ||
        config->composition_blob_slot_count >
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS) {
        return false;
    }
    layout.mesh_offset = cursor;
    if (!meiso_composition_edge_private_checked_add_size(
            cursor, MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE, &cursor
        )) {
        return false;
    }
    blob_descriptor = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    blob_config.published_slot_count = config->composition_blob_slot_count;
    layout.blob_size = meiso_blob_store_memory_required(
        blob_descriptor, &blob_config
    );
    layout.scene_size = meiso_composition_scene_store_memory_required();
    layout.coordinator_size =
        meiso_composition_coordinator_memory_required();
    if (blob_descriptor == NULL || layout.blob_size == 0U ||
        layout.scene_size == 0U || layout.coordinator_size == 0U ||
        !meiso_composition_edge_private_checked_align_size(
            cursor, meiso_blob_alignment_required(), &layout.blob_offset
        ) ||
        !meiso_composition_edge_private_checked_add_size(
            layout.blob_offset, layout.blob_size, &cursor
        ) ||
        !meiso_composition_edge_private_checked_align_size(
            cursor, meiso_composition_scene_store_alignment_required(),
            &layout.scene_offset
        ) ||
        !meiso_composition_edge_private_checked_add_size(
            layout.scene_offset, layout.scene_size, &cursor
        ) ||
        !meiso_composition_edge_private_checked_align_size(
            cursor, meiso_composition_coordinator_alignment_required(),
            &layout.coordinator_offset
        ) ||
        !meiso_composition_edge_private_checked_add_size(
            layout.coordinator_offset, layout.coordinator_size, &cursor
        )) {
        return false;
    }
    if (config->cpu_raster_preview_enabled) {
        layout.renderer_size =
            meiso_composition_cpu_raster_renderer_memory_required();
        if (layout.renderer_size == 0U ||
            !meiso_composition_edge_private_checked_align_size(
                cursor,
                meiso_composition_cpu_raster_renderer_alignment_required(),
                &layout.renderer_offset
            ) ||
            !meiso_composition_edge_private_checked_add_size(
                layout.renderer_offset, layout.renderer_size, &cursor
            )) {
            return false;
        }
    }
    layout.required = cursor;
    *out_layout = layout;
    return true;
}
static bool meiso_composition_edge_simulator_private_device_sample_zero(
    const MeisoDeviceTelemetrySample *sample
)
{
    return sample != NULL && sample->selected_fields == 0U &&
        sample->present_fields == 0U && sample->temperature_millic == 0 &&
        sample->temperature_uncertainty_millic == 0U &&
        sample->cpu_load_ppm == 0U && sample->memory_total_bytes == 0U &&
        sample->memory_available_bytes == 0U;
}

static bool meiso_composition_edge_simulator_private_device_sample_within_capability(
    const MeisoDeviceTelemetryCapability *capability,
    const MeisoDeviceTelemetrySample *sample
)
{
    const bool temperature_present = sample != NULL &&
        (sample->present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U;

    return capability != NULL && sample != NULL &&
        (sample->selected_fields & ~capability->supported_fields) == 0U &&
        (!temperature_present ||
         (sample->temperature_millic >= capability->min_temperature_millic &&
          sample->temperature_millic <= capability->max_temperature_millic &&
          sample->temperature_uncertainty_millic <=
              capability->max_temperature_uncertainty_millic));
}

static bool meiso_composition_edge_simulator_private_composition_config_valid(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    const MeisoCompositionMediaCapability *render;

    if (config == NULL) {
        return false;
    }
    if (!config->composition_enabled) {
        return config->composition_blob_slot_count == 0U &&
            config->composition_clock_epoch == 0U &&
            config->presentation_production_epoch == 0U &&
            config->scene_production_epoch == 0U &&
            !config->cpu_raster_preview_enabled;
    }
    render = &config->catalog.render_media_capability;
    return config->composition_blob_slot_count != 0U &&
        config->composition_blob_slot_count <=
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS &&
        config->composition_clock_epoch != 0U &&
        config->presentation_production_epoch != 0U &&
        config->scene_production_epoch != 0U &&
        (!config->cpu_raster_preview_enabled ||
         (render->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
          render->pixel_formats ==
              MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 &&
          render->codecs == MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW &&
          render->min_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
          render->max_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
          render->min_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
          render->max_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
          render->max_access_unit_bytes >=
              MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES));
}

size_t meiso_composition_edge_simulator_alignment_required(void)
{
    size_t alignment = _Alignof(MeisoEdgeHandle);
    const size_t blob_alignment = meiso_blob_alignment_required();
    const size_t scene_alignment =
        meiso_composition_scene_store_alignment_required();
    const size_t coordinator_alignment =
        meiso_composition_coordinator_alignment_required();
    const size_t renderer_alignment =
        meiso_composition_cpu_raster_renderer_alignment_required();

    if (blob_alignment > alignment) {
        alignment = blob_alignment;
    }
    if (scene_alignment > alignment) {
        alignment = scene_alignment;
    }
    if (coordinator_alignment > alignment) {
        alignment = coordinator_alignment;
    }
    if (renderer_alignment > alignment) {
        alignment = renderer_alignment;
    }
    return alignment;
}

size_t meiso_composition_edge_simulator_memory_required(
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    MeisoCompositionEdgeSimulatorLayout layout;

    if (!meiso_composition_edge_simulator_private_layout(config, &layout)) {
        return 0U;
    }
    return layout.required;
}

MeisoCompositionEdgeSimulatorInitStatus meiso_composition_edge_simulator_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionEdgeSimulatorConfig *config,
    MeisoEdgeHandle **out_handle
)
{
    MeisoEdgeHandle candidate;
    uintptr_t arena_begin;
    uintptr_t arena_end;
    uintptr_t config_begin;
    uintptr_t config_end;
    uintptr_t output_begin;
    uintptr_t output_end;
    size_t required;
    MeisoCompositionEdgeSimulatorLayout layout;
    size_t i;
    size_t wifi_sample_wire_size = 0U;
    size_t device_sample_wire_size = 0U;
    uint8_t wifi_sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t device_sample_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t *arena_bytes;

    if (!meiso_composition_edge_simulator_private_layout(config, &layout)) {
        return config == NULL ?
            MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT :
            MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    required = layout.required;
    if (arena == NULL || config == NULL || out_handle == NULL ||
        required == 0U || config->catalog_generation == 0U ||
        config->continuity_serial == 0U ||
        !meiso_owner_scope_valid(config->desired_owner) ||
        config->wifi_production_epoch == 0U ||
        config->render_media_stream_epoch == 0U ||
        config->camera_media_stream_epoch == 0U ||
        !meiso_composition_edge_private_checked_range(arena, arena_size, &arena_begin, &arena_end) ||
        !meiso_composition_edge_private_checked_range(config, sizeof(*config), &config_begin, &config_end) ||
        !meiso_composition_edge_private_checked_range(out_handle, sizeof(*out_handle), &output_begin,
                       &output_end) ||
        meiso_composition_edge_private_ranges_overlap(config_begin, config_end, output_begin, output_end)) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT;
    }
    if (meiso_wifi_telemetry_sample_encode(
            &config->wifi_sample,
            (MeisoMutableBuffer){wifi_sample_wire, sizeof(wifi_sample_wire)},
            &wifi_sample_wire_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        wifi_sample_wire_size != MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE ||
        (config->wifi_sample.selected_fields &
         ~config->catalog.wifi_capability.supported_fields) != 0U) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    if ((!config->device_telemetry_enabled &&
         (config->device_production_epoch != 0U ||
          !meiso_composition_edge_simulator_private_device_sample_zero(&config->device_sample))) ||
        (config->device_telemetry_enabled &&
         (config->device_production_epoch == 0U ||
          meiso_device_telemetry_sample_encode(
              &config->device_sample,
              (MeisoMutableBuffer){device_sample_wire,
                                   sizeof(device_sample_wire)},
              &device_sample_wire_size
          ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
          device_sample_wire_size != MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE ||
          !meiso_composition_edge_simulator_private_device_sample_within_capability(
              &config->catalog.device_telemetry_capability,
              &config->device_sample
          ))) || !meiso_composition_edge_simulator_private_composition_config_valid(config)) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    if ((arena_begin %
         meiso_composition_edge_simulator_alignment_required()) != 0U) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < required) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_ARENA_TOO_SMALL;
    }
    if (meiso_composition_edge_private_ranges_overlap(arena_begin, arena_begin + required,
                       config_begin, config_end) ||
        meiso_composition_edge_private_ranges_overlap(arena_begin, arena_begin + required,
                       output_begin, output_end)) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_ARGUMENT;
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
        return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INVALID_CONFIG;
    }
    candidate.magic = MEISO_COMPOSITION_EDGE_HANDLE_MAGIC;
    candidate.backend_kind = MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR;
    candidate.backend_ops =
        &meiso_composition_edge_simulator_private_backend_ops;
    candidate.arena_begin = arena_begin;
    candidate.arena_end = arena_begin + required;
    candidate.catalog_generation = config->catalog_generation;
    candidate.continuity_serial = config->continuity_serial;
    candidate.profile = *meiso_composition_profile_ref();
    candidate.simulator_catalog = config->catalog;
    candidate.simulator_desired_owner = config->desired_owner;
    candidate.simulator_wifi_epoch = config->wifi_production_epoch;
    candidate.simulator_wifi_epoch_available = true;
    candidate.simulator_media_epoch[0] =
        config->render_media_stream_epoch;
    candidate.simulator_media_epoch[1] =
        config->camera_media_stream_epoch;
    candidate.simulator_media_epoch_available[0] = true;
    candidate.simulator_media_epoch_available[1] = true;
    candidate.simulator_wifi_sample = config->wifi_sample;
    candidate.simulator_device_supported = config->device_telemetry_enabled;
    candidate.simulator_device_epoch = config->device_production_epoch;
    candidate.simulator_device_epoch_available =
        config->device_telemetry_enabled;
    candidate.simulator_device_sample = config->device_sample;
    candidate.simulator_composition_supported =
        config->composition_enabled;
    candidate.simulator_cpu_raster_enabled =
        config->cpu_raster_preview_enabled;
    candidate.simulator_blob_slot_count =
        config->composition_blob_slot_count;
    candidate.simulator_composition_clock_epoch =
        config->composition_clock_epoch;
    candidate.simulator_presentation_epoch =
        config->presentation_production_epoch;
    candidate.simulator_presentation_epoch_available =
        config->composition_enabled;
    candidate.simulator_scene_epoch = config->scene_production_epoch;
    candidate.simulator_scene_epoch_available =
        config->composition_enabled;
    candidate.simulator_next_plan_generation = 1U;
    candidate.simulator_plan_generation_available =
        config->composition_enabled;
    candidate.simulator_snapshot.edge = (MeisoEdgeSnapshot){
        MEISO_HOST_EDGE_API_VERSION,
        candidate.profile,
        MEISO_EDGE_PHASE_BOOTSTRAPPING,
        MEISO_EDGE_TERMINAL_NONE,
        false,
        false,
        config->continuity_serial,
        0U,
        0U,
        0U,
        0U
    };
    arena_bytes = (uint8_t *)arena;
    candidate.simulator_media[0].buffer =
        arena_bytes + layout.render_offset;
    candidate.simulator_media[0].capacity =
        config->catalog.render_media_capability.max_access_unit_bytes;
    candidate.simulator_media[1].buffer =
        arena_bytes + layout.camera_offset;
    candidate.simulator_media[1].capacity =
        config->catalog.camera_media_capability.max_access_unit_bytes;
    if (config->composition_enabled) {
        candidate.simulator_mesh_buffer = arena_bytes + layout.mesh_offset;
        candidate.simulator_mesh_buffer_capacity =
            MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
        candidate.simulator_blob_arena = arena_bytes + layout.blob_offset;
        candidate.simulator_blob_arena_size = layout.blob_size;
        candidate.simulator_scene_arena = arena_bytes + layout.scene_offset;
        candidate.simulator_scene_arena_size = layout.scene_size;
        candidate.simulator_coordinator_arena =
            arena_bytes + layout.coordinator_offset;
        candidate.simulator_coordinator_arena_size =
            layout.coordinator_size;
        if (config->cpu_raster_preview_enabled) {
            candidate.simulator_renderer_arena =
                arena_bytes + layout.renderer_offset;
            candidate.simulator_renderer_arena_size = layout.renderer_size;
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT; ++i) {
        const uintptr_t capability_begin =
            (uintptr_t)candidate.endpoints[i].capability.data;
        const uintptr_t dynamic_begin =
            (uintptr_t)candidate.dynamic_capability;
        const uintptr_t dynamic_end =
            dynamic_begin + sizeof(candidate.dynamic_capability);
        if (candidate.endpoints[i].capability.size != 0U &&
            capability_begin >= dynamic_begin && capability_begin < dynamic_end) {
            const size_t offset = (size_t)(capability_begin - dynamic_begin);
            if (offset > sizeof(candidate.dynamic_capability) ||
                candidate.endpoints[i].capability.size >
                    sizeof(candidate.dynamic_capability) - offset) {
                return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_INTERNAL_FAILURE;
            }
            candidate.endpoints[i].capability.data =
                (const uint8_t *)arena +
                offsetof(MeisoEdgeHandle, dynamic_capability) + offset;
        }
    }
    memset(arena, 0, required);
    memmove(arena, &candidate, sizeof(candidate));
    *out_handle = (MeisoEdgeHandle *)arena;
    return MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK;
}

static bool meiso_composition_edge_simulator_private_composition_initialize(MeisoEdgeHandle *handle)
{
    const MeisoResourceDescriptor *blob_descriptor;
    MeisoBlobConfig blob_config;
    MeisoCompositionCoordinatorConfig coordinator_config;
    MeisoBlobStore *blob_store = NULL;
    MeisoCompositionSceneStore *scene_store = NULL;
    MeisoCompositionCoordinator *coordinator = NULL;
    MeisoCompositionCpuRasterRenderer *renderer = NULL;
    MeisoCompositionSceneControlScope scope;

    if (handle == NULL || !handle->simulator_composition_supported) {
        return handle != NULL;
    }
    if (handle->simulator_composition_initialized) {
        return true;
    }
    blob_descriptor = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    blob_config.published_slot_count = handle->simulator_blob_slot_count;
    if (blob_descriptor == NULL ||
        meiso_blob_store_init(
            handle->simulator_blob_arena,
            handle->simulator_blob_arena_size,
            blob_descriptor, &blob_config, &blob_store
        ) != MEISO_BLOB_INIT_OK ||
        meiso_composition_scene_store_init(
            handle->simulator_scene_arena,
            handle->simulator_scene_arena_size,
            blob_store, &scene_store
        ) != MEISO_COMPOSITION_SCENE_STORE_INIT_OK) {
        return false;
    }
    memset(&coordinator_config, 0, sizeof(coordinator_config));
    coordinator_config.scene_store = scene_store;
    coordinator_config.session_id.host_nonce = (MeisoSessionNonce){
        handle->continuity_serial,
        handle->simulator_desired_owner.incarnation
    };
    coordinator_config.session_id.edge_nonce = (MeisoSessionNonce){
        handle->catalog_generation,
        handle->simulator_catalog.presentation_incarnation
    };
    coordinator_config.edge_incarnation =
        handle->simulator_catalog.presentation_incarnation;
    coordinator_config.capability_generation = handle->catalog_generation;
    coordinator_config.edge_clock_epoch =
        handle->simulator_composition_clock_epoch;
    if (meiso_composition_coordinator_init(
            handle->simulator_coordinator_arena,
            handle->simulator_coordinator_arena_size,
            &coordinator_config, &coordinator
        ) != MEISO_COMPOSITION_COORDINATOR_INIT_OK ||
        meiso_composition_coordinator_begin_owner(
            coordinator, handle->simulator_desired_owner.incarnation
        ) != MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED) {
        return false;
    }
    if (handle->simulator_cpu_raster_enabled &&
        (meiso_composition_cpu_raster_renderer_init(
             handle->simulator_renderer_arena,
             handle->simulator_renderer_arena_size,
             blob_store, &renderer
         ) != MEISO_COMPOSITION_CPU_RASTER_INIT_OK ||
         !meiso_composition_scene_store_control_scope(scene_store, &scope) ||
         meiso_composition_cpu_raster_renderer_begin_control_scope(
             renderer, &scope
         ) != MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED)) {
        return false;
    }
    handle->backend_runtime.simulator_blob_store = blob_store;
    handle->simulator_scene_store = scene_store;
    handle->simulator_coordinator = coordinator;
    handle->simulator_renderer = renderer;
    handle->simulator_composition_initialized = true;
    return true;
}

MeisoEdgeStatus meiso_composition_edge_simulator_activate(
    MeisoEdgeHandle *handle
)
{
    MeisoEdgeSnapshot *edge;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    edge = &handle->simulator_snapshot.edge;
    if (edge->phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (edge->phase == MEISO_EDGE_PHASE_READY) {
        return MEISO_EDGE_CONFLICT;
    }
    if (!meiso_composition_edge_simulator_private_composition_initialize(handle)) {
        (void)meiso_composition_edge_simulator_terminate(
            handle, MEISO_EDGE_TERMINAL_INTERNAL_FAILURE
        );
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    edge->phase = MEISO_EDGE_PHASE_READY;
    edge->online = true;
    edge->resource_graph_live = true;
    edge->catalog_generation = handle->catalog_generation;
    edge->operations = MEISO_COMPOSITION_EDGE_SIMULATOR_OPERATIONS |
        (handle->simulator_composition_supported ?
         (MEISO_EDGE_OPERATION_BLOB_PUT |
          MEISO_EDGE_OPERATION_COMPOSITION) : 0U);
    edge->endpoint_count = MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT;
    edge->resource_count = MEISO_COMPOSITION_PROFILE_ROUTE_COUNT;
    return MEISO_EDGE_OK;
}
static bool meiso_composition_edge_simulator_private_catalog_equal(
    const MeisoCompositionProfileCatalogConfig *lhs,
    const MeisoCompositionProfileCatalogConfig *rhs
)
{
    uint8_t lhs_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    uint8_t rhs_wire[MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE];
    size_t lhs_size;
    size_t rhs_size;

    if (lhs == NULL || rhs == NULL ||
        lhs->wifi_incarnation != rhs->wifi_incarnation ||
        lhs->presentation_incarnation != rhs->presentation_incarnation ||
        lhs->scene_incarnation != rhs->scene_incarnation ||
        lhs->device_telemetry_incarnation !=
            rhs->device_telemetry_incarnation ||
        lhs->render_media_incarnation != rhs->render_media_incarnation ||
        lhs->camera_media_incarnation != rhs->camera_media_incarnation) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    if (meiso_wifi_telemetry_capability_encode(
            &lhs->wifi_capability,
            (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        meiso_wifi_telemetry_capability_encode(
            &rhs->wifi_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK || lhs_size != rhs_size ||
        memcmp(lhs_wire, rhs_wire, lhs_size) != 0) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    if (meiso_device_telemetry_capability_encode(
            &lhs->device_telemetry_capability,
            (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        meiso_device_telemetry_capability_encode(
            &rhs->device_telemetry_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK || lhs_size != rhs_size ||
        memcmp(lhs_wire, rhs_wire, lhs_size) != 0) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    if (meiso_composition_media_capability_encode(
            &lhs->render_media_capability,
            (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) != MEISO_COMPOSITION_PROFILE_OK ||
        meiso_composition_media_capability_encode(
            &rhs->render_media_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) != MEISO_COMPOSITION_PROFILE_OK || lhs_size != rhs_size ||
        memcmp(lhs_wire, rhs_wire, lhs_size) != 0) {
        return false;
    }
    lhs_size = 0U;
    rhs_size = 0U;
    return meiso_composition_media_capability_encode(
               &lhs->camera_media_capability,
               (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
           ) == MEISO_COMPOSITION_PROFILE_OK &&
        meiso_composition_media_capability_encode(
            &rhs->camera_media_capability,
            (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) == MEISO_COMPOSITION_PROFILE_OK && lhs_size == rhs_size &&
        memcmp(lhs_wire, rhs_wire, lhs_size) == 0;
}

static bool meiso_composition_edge_simulator_private_wifi_sample_equal(
    const MeisoWifiTelemetrySample *lhs,
    const MeisoWifiTelemetrySample *rhs
)
{
    uint8_t lhs_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t rhs_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t lhs_size = 0U;
    size_t rhs_size = 0U;

    return lhs != NULL && rhs != NULL &&
        meiso_wifi_telemetry_sample_encode(
            lhs, (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        meiso_wifi_telemetry_sample_encode(
            rhs, (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK && lhs_size == rhs_size &&
        memcmp(lhs_wire, rhs_wire, lhs_size) == 0;
}

static bool meiso_composition_edge_simulator_private_device_sample_equal(
    const MeisoDeviceTelemetrySample *lhs,
    const MeisoDeviceTelemetrySample *rhs
)
{
    uint8_t lhs_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t rhs_wire[MEISO_DEVICE_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t lhs_size = 0U;
    size_t rhs_size = 0U;

    return lhs != NULL && rhs != NULL &&
        meiso_device_telemetry_sample_encode(
            lhs, (MeisoMutableBuffer){lhs_wire, sizeof(lhs_wire)}, &lhs_size
        ) == MEISO_DEVICE_TELEMETRY_CODEC_OK &&
        meiso_device_telemetry_sample_encode(
            rhs, (MeisoMutableBuffer){rhs_wire, sizeof(rhs_wire)}, &rhs_size
        ) == MEISO_DEVICE_TELEMETRY_CODEC_OK && lhs_size == rhs_size &&
        memcmp(lhs_wire, rhs_wire, lhs_size) == 0;
}

bool meiso_composition_edge_internal_simulator_storage_disjoint(
    const MeisoEdgeHandle *handle,
    const void *pointer,
    size_t size
)
{
    return meiso_composition_edge_private_handle_valid(handle) &&
        handle->backend_kind == MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR &&
        meiso_composition_edge_private_storage_disjoint(
            handle, pointer, size
        );
}

bool meiso_composition_edge_internal_simulator_fresh_matches(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorConfig *config
)
{
    size_t i;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR || config == NULL ||
        handle->catalog_generation != config->catalog_generation ||
        handle->continuity_serial != config->continuity_serial ||
        !meiso_owner_scope_equal(handle->simulator_desired_owner,
                                 config->desired_owner) ||
        handle->simulator_wifi_epoch != config->wifi_production_epoch ||
        handle->simulator_device_supported !=
            config->device_telemetry_enabled ||
        handle->simulator_device_epoch != config->device_production_epoch ||
        handle->simulator_device_epoch_available !=
            config->device_telemetry_enabled ||
        handle->simulator_composition_supported !=
            config->composition_enabled ||
        handle->simulator_cpu_raster_enabled !=
            config->cpu_raster_preview_enabled ||
        handle->simulator_blob_slot_count !=
            config->composition_blob_slot_count ||
        handle->simulator_composition_clock_epoch !=
            config->composition_clock_epoch ||
        handle->simulator_presentation_epoch !=
            config->presentation_production_epoch ||
        handle->simulator_scene_epoch != config->scene_production_epoch ||
        handle->simulator_presentation_epoch_available !=
            config->composition_enabled ||
        handle->simulator_scene_epoch_available !=
            config->composition_enabled ||
        handle->simulator_next_plan_generation != 1U ||
        handle->simulator_plan_generation_available !=
            config->composition_enabled ||
        handle->simulator_composition_initialized ||
        handle->simulator_mesh_pending ||
        handle->simulator_scene_pending.pending ||
        handle->simulator_scene_retry_pending ||
        handle->simulator_scene_retry_ready ||
        handle->simulator_scene_value_present ||
        handle->simulator_presentation_pending.pending ||
        handle->simulator_presentation_plan_present ||
        handle->simulator_presentation_activation_pending ||
        handle->simulator_render_result_pending ||
        handle->simulator_raster_preview_pending ||
        handle->simulator_published_blob_bytes != 0U ||
        handle->simulator_render_result_sequence != 0U ||
        handle->simulator_media_epoch[0] !=
            config->render_media_stream_epoch ||
        handle->simulator_media_epoch[1] !=
            config->camera_media_stream_epoch ||
        !handle->simulator_wifi_epoch_available ||
        !handle->simulator_media_epoch_available[0] ||
        !handle->simulator_media_epoch_available[1] ||
        handle->simulator_next_command_id != 0U ||
        handle->simulator_wifi_pending ||
        handle->simulator_device_pending ||
        handle->simulator_snapshot.edge.phase !=
            MEISO_EDGE_PHASE_BOOTSTRAPPING ||
        handle->simulator_snapshot.edge.online ||
        handle->simulator_snapshot.edge.resource_graph_live ||
        handle->simulator_snapshot.wifi_effective_present ||
        handle->simulator_snapshot.wifi_sample_present ||
        handle->simulator_snapshot.device_effective_present ||
        handle->simulator_snapshot.device_sample_present ||
        handle->simulator_snapshot.render_media_actual_present ||
        handle->simulator_snapshot.camera_media_actual_present ||
        !meiso_composition_edge_simulator_private_catalog_equal(&handle->simulator_catalog,
                                 &config->catalog) ||
        !meiso_composition_edge_simulator_private_wifi_sample_equal(&handle->simulator_wifi_sample,
                                     &config->wifi_sample) ||
        (config->device_telemetry_enabled &&
         !meiso_composition_edge_simulator_private_device_sample_equal(&handle->simulator_device_sample,
                                        &config->device_sample))) {
        return false;
    }
    for (i = 0U; i < MEISO_COMPOSITION_EDGE_DESIRED_COUNT; ++i) {
        if (handle->simulator_snapshot.desired[i].requested ||
            handle->simulator_snapshot.actual[i].present) {
            return false;
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT; ++i) {
        if (handle->simulator_media_pending[i].pending ||
            handle->simulator_media[i].present ||
            handle->simulator_media[i].borrowed ||
            handle->simulator_snapshot.media[i].latest_present) {
            return false;
        }
    }
    return true;
}
