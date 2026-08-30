cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledSimulationConsumer LANGUAGES C)

find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Simulation)

foreach(required_target
        Meiso::Core
        Meiso::Runtime
        Meiso::CompositionProfile
        Meiso::CompositionEdge
        Meiso::CompositionSceneStore
        Meiso::CompositionCoordinator
        Meiso::CompositionCpuRasterRenderer
        Meiso::CompositionEdgeSimulator
        Meiso::CompositionEdgeTrace)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Simulation dependency closure omitted ${required_target}")
    endif()
endforeach()
if(TARGET Meiso::PosixTcpAdapter OR
   TARGET Meiso::CompositionControlSession OR
   TARGET Meiso::CompositionHostRuntime OR
   TARGET Meiso::CompositionRemoteEdge)
    message(FATAL_ERROR "Simulation component leaked POSIX or Host targets")
endif()

add_executable(meiso_installed_simulation_consumer main.c)
target_link_libraries(
    meiso_installed_simulation_consumer
    PRIVATE Meiso::CompositionEdgeSimulator Meiso::CompositionEdgeTrace
)
set_property(TARGET meiso_installed_simulation_consumer PROPERTY C_STANDARD 11)

enable_testing()
add_test(
    NAME meiso_installed_simulation_consumer
    COMMAND meiso_installed_simulation_consumer
)
