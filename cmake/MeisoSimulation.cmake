# Simulation backends implement the client facade without introducing Host or
# platform-adapter dependencies into the portable composition path.

add_library(
    meiso_composition_edge_simulator
    simulation/composition_edge_simulator.c
    simulation/composition_edge_simulator_commands.c
    simulation/composition_edge_simulator_composition.c
    simulation/composition_edge_simulator_render.c
    simulation/composition_edge_simulator_step.c
    simulation/composition_edge_simulator_telemetry.c
)
meiso_configure_public_target(
    meiso_composition_edge_simulator
    CompositionEdgeSimulator
)
target_include_directories(
    meiso_composition_edge_simulator
    PRIVATE
        integrations/composition
        simulation
)
target_link_libraries(
    meiso_composition_edge_simulator
    PUBLIC
        meiso_composition_edge
        meiso_composition_scene_store
        meiso_composition_coordinator
        meiso_composition_cpu_raster_renderer
)
meiso_enable_warnings(meiso_composition_edge_simulator)
meiso_set_target_folder(
    meiso_composition_edge_simulator
    "Meiso/Simulation"
)
add_library(
    Meiso::CompositionEdgeSimulator
    ALIAS meiso_composition_edge_simulator
)

add_library(
    meiso_composition_edge_trace
    simulation/composition_edge_trace_codec.c
    simulation/composition_edge_trace_reader.c
    simulation/composition_edge_trace_writer.c
)
meiso_configure_public_target(
    meiso_composition_edge_trace
    CompositionEdgeTrace
)
target_include_directories(
    meiso_composition_edge_trace
    PRIVATE simulation
)
target_link_libraries(
    meiso_composition_edge_trace
    PUBLIC meiso_composition_edge_simulator
)
meiso_enable_warnings(meiso_composition_edge_trace)
meiso_set_target_folder(
    meiso_composition_edge_trace
    "Meiso/Simulation"
)
add_library(
    Meiso::CompositionEdgeTrace
    ALIAS meiso_composition_edge_trace
)
