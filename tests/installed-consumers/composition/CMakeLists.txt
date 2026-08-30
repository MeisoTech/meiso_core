cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledCompositionConsumer LANGUAGES C)

find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Composition)

foreach(required_target
        Meiso::Core
        Meiso::Runtime
        Meiso::CompositionSceneSchema
        Meiso::CompositionSceneStore
        Meiso::CompositionCoordinator
        Meiso::CompositionReferenceRenderer
        Meiso::CompositionCpuRasterRenderer)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Composition dependency closure omitted ${required_target}")
    endif()
endforeach()
if(TARGET Meiso::PosixTcpAdapter OR
   TARGET Meiso::CompositionControlSession OR
   TARGET Meiso::CompositionHostRuntime)
    message(FATAL_ERROR "Portable Composition component leaked POSIX or Host targets")
endif()

add_executable(meiso_installed_composition_consumer main.c)
target_link_libraries(
    meiso_installed_composition_consumer
    PRIVATE Meiso::CompositionCoordinator
)
set_property(TARGET meiso_installed_composition_consumer PROPERTY C_STANDARD 11)

enable_testing()
add_test(
    NAME meiso_installed_composition_consumer
    COMMAND meiso_installed_composition_consumer
)
