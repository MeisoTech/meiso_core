cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledClientConsumer LANGUAGES C)

find_package(MeisoGlass 0.1 REQUIRED CONFIG COMPONENTS Client)

foreach(required_target
        Meiso::Core
        Meiso::Runtime
        Meiso::CompositionProfile
        Meiso::CompositionEdge)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Client dependency closure omitted ${required_target}")
    endif()
endforeach()
if(TARGET Meiso::CompositionHostRuntime OR
   TARGET Meiso::CompositionRemoteEdge OR
   TARGET Meiso::CompositionSceneStore OR
   TARGET Meiso::CompositionEdgeSimulator)
    message(FATAL_ERROR "Client component leaked backend targets")
endif()

add_executable(meiso_installed_client_consumer main.c)
target_link_libraries(
    meiso_installed_client_consumer
    PRIVATE Meiso::CompositionEdge
)
set_property(TARGET meiso_installed_client_consumer PROPERTY C_STANDARD 11)

enable_testing()
add_test(
    NAME meiso_installed_client_consumer
    COMMAND meiso_installed_client_consumer
)
