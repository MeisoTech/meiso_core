cmake_minimum_required(VERSION 3.21)

project(MeisoGlassInstalledDisplayIngressHostFixtureInputConsumer
    LANGUAGES C CXX)

find_package(
    MeisoGlass 0.1 REQUIRED CONFIG
    COMPONENTS DisplayIngressHostFixtureInput
)

foreach(required_target
        Meiso::Core
        Meiso::Runtime
        Meiso::DisplayIngressProfile
        Meiso::DisplayIngressMediaTransport
        Meiso::DisplayIngressHostFixtureInput)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR
            "DisplayIngressHostFixtureInput closure omitted ${required_target}")
    endif()
endforeach()
foreach(forbidden_target
        Meiso::PosixTcpAdapter
        Meiso::DisplayIngressMediaPosixUdpAdapter
        Meiso::CompositionControlSession
        Meiso::CompositionEdge
        Meiso::CompositionHostRuntime
        Meiso::CompositionRemoteEdge
        Meiso::CompositionSceneStore)
    if(TARGET ${forbidden_target})
        message(FATAL_ERROR
            "DisplayIngressHostFixtureInput leaked ${forbidden_target}")
    endif()
endforeach()

add_executable(
    meiso_installed_display_ingress_host_fixture_input_consumer
    main.c
    header_cpp17.cpp
)
target_link_libraries(
    meiso_installed_display_ingress_host_fixture_input_consumer
    PRIVATE Meiso::DisplayIngressHostFixtureInput
)
set_property(
    TARGET meiso_installed_display_ingress_host_fixture_input_consumer
    PROPERTY C_STANDARD 11
)
set_property(
    TARGET meiso_installed_display_ingress_host_fixture_input_consumer
    PROPERTY CXX_STANDARD 17
)
set_property(
    TARGET meiso_installed_display_ingress_host_fixture_input_consumer
    PROPERTY CXX_STANDARD_REQUIRED YES
)

enable_testing()
add_test(
    NAME meiso_installed_display_ingress_host_fixture_input_consumer
    COMMAND meiso_installed_display_ingress_host_fixture_input_consumer
)
