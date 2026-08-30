cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledHostConsumer LANGUAGES C)

find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Host)

foreach(required_target
        Meiso::Core
        Meiso::Runtime
        Meiso::CompositionProfile
        Meiso::PosixTcpAdapter
        Meiso::CompositionControlSession
        Meiso::CompositionHostRuntime
        Meiso::CompositionEdge
        Meiso::CompositionRemoteEdge)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Host dependency closure omitted ${required_target}")
    endif()
endforeach()
if(TARGET Meiso::CompositionSceneStore OR
   TARGET Meiso::CompositionEdgeSimulator OR
   TARGET Meiso::CompositionEdgeTrace)
    message(FATAL_ERROR "Host component leaked Composition or Simulation targets")
endif()

add_executable(meiso_installed_host_consumer main.c)
target_link_libraries(
    meiso_installed_host_consumer
    PRIVATE Meiso::CompositionRemoteEdge
)
set_property(TARGET meiso_installed_host_consumer PROPERTY C_STANDARD 11)

enable_testing()
add_test(NAME meiso_installed_host_consumer COMMAND meiso_installed_host_consumer)
