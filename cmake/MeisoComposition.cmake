# Composition owns Edge-side scene state, coordination and reference rendering.
# The versioned Scene schema and media wire contracts remain Profile targets.

add_library(
    meiso_composition_scene_store
    composition/scene_store.c
    composition/scene_store_queries.c
    composition/scene_store_render.c
)
meiso_configure_public_target(
    meiso_composition_scene_store
    CompositionSceneStore
)
target_link_libraries(
    meiso_composition_scene_store
    PUBLIC meiso_composition_scene_schema meiso_core_resources
)
target_include_directories(
    meiso_composition_scene_store
    PRIVATE "${PROJECT_SOURCE_DIR}/profiles/composition"
)
meiso_enable_warnings(meiso_composition_scene_store)
meiso_set_target_folder(
    meiso_composition_scene_store
    "Meiso/Composition"
)
add_library(
    Meiso::CompositionSceneStore
    ALIAS meiso_composition_scene_store
)

add_library(
    meiso_composition_coordinator
    composition/composition_coordinator.c
    composition/composition_coordinator_desired.c
    composition/composition_coordinator_frame.c
    composition/composition_coordinator_private.c
)
meiso_configure_public_target(
    meiso_composition_coordinator
    CompositionCoordinator
)
target_link_libraries(
    meiso_composition_coordinator
    PUBLIC meiso_composition_scene_store
)
meiso_enable_warnings(meiso_composition_coordinator)
meiso_set_target_folder(
    meiso_composition_coordinator
    "Meiso/Composition"
)
add_library(
    Meiso::CompositionCoordinator
    ALIAS meiso_composition_coordinator
)

add_library(
    meiso_composition_reference_renderer
    composition/composition_reference_renderer.c
    composition/composition_reference_renderer_render.c
)
meiso_configure_public_target(
    meiso_composition_reference_renderer
    CompositionReferenceRenderer
)
target_link_libraries(
    meiso_composition_reference_renderer
    PUBLIC meiso_composition_scene_store
)
meiso_enable_warnings(meiso_composition_reference_renderer)
meiso_set_target_folder(
    meiso_composition_reference_renderer
    "Meiso/Composition/Renderers"
)
add_library(
    Meiso::CompositionReferenceRenderer
    ALIAS meiso_composition_reference_renderer
)

add_library(
    meiso_composition_cpu_raster_renderer
    composition/composition_cpu_raster_renderer.c
    composition/composition_cpu_raster_renderer_codec.c
    composition/composition_cpu_raster_renderer_geometry.c
    composition/composition_cpu_raster_renderer_render.c
)
meiso_configure_public_target(
    meiso_composition_cpu_raster_renderer
    CompositionCpuRasterRenderer
)
target_link_libraries(
    meiso_composition_cpu_raster_renderer
    PUBLIC meiso_composition_scene_store
)
if(UNIX)
    target_link_libraries(meiso_composition_cpu_raster_renderer PRIVATE m)
endif()
meiso_enable_warnings(meiso_composition_cpu_raster_renderer)
meiso_set_target_folder(
    meiso_composition_cpu_raster_renderer
    "Meiso/Composition/Renderers"
)
add_library(
    Meiso::CompositionCpuRasterRenderer
    ALIAS meiso_composition_cpu_raster_renderer
)
