# Platform adapters, Host/Edge integrations, and long-lived service lanes.

meiso_add_c_test(
    meiso_display_ingress_host_fixture_input_test
    tests/test_display_ingress_host_fixture_input.c
    integration
    meiso_display_ingress_host_fixture_input
)


if(TARGET meiso_composition_control_session)
    meiso_add_c_test(
        meiso_composition_control_session_test
        tests/test_composition_control_session.c
        integration
        meiso_composition_control_session
    )
endif()

if(TARGET meiso_composition_edge AND
   TARGET meiso_composition_remote_edge AND
   TARGET meiso_composition_edge_simulator AND
   TARGET meiso_composition_edge_trace)
    add_library(
        meiso_composition_edge_client_fixture_test_object OBJECT
        tests/composition_edge_client_fixture.c
    )
    target_include_directories(
        meiso_composition_edge_client_fixture_test_object
        PRIVATE tests
    )
    target_link_libraries(
        meiso_composition_edge_client_fixture_test_object
        PRIVATE meiso_composition_edge
    )
    meiso_enable_warnings(
        meiso_composition_edge_client_fixture_test_object
    )
    meiso_set_target_folder(
        meiso_composition_edge_client_fixture_test_object
        "Meiso/Tests/integration"
    )

    add_executable(
        meiso_composition_host_runtime_test
        tests/test_composition_host_runtime.c
        $<TARGET_OBJECTS:meiso_composition_edge_client_fixture_test_object>
    )
    target_include_directories(
        meiso_composition_host_runtime_test
        PRIVATE
            adapters/posix
            devtools
            integrations/composition
            integrations/host
            tests
    )
    target_link_libraries(
        meiso_composition_host_runtime_test
        PRIVATE meiso_composition_remote_edge
    )
    meiso_enable_warnings(meiso_composition_host_runtime_test)
    meiso_set_target_folder(
        meiso_composition_host_runtime_test
        "Meiso/Tests/integration"
    )
    meiso_register_target_test(
        meiso_composition_host_runtime_test "integration"
    )

    add_executable(
        meiso_composition_edge_simulator_test
        tests/test_composition_edge_simulator.c
        $<TARGET_OBJECTS:meiso_composition_edge_client_fixture_test_object>
    )
    target_include_directories(
        meiso_composition_edge_simulator_test
        PRIVATE tests
    )
    target_link_libraries(
        meiso_composition_edge_simulator_test
        PRIVATE meiso_composition_edge_simulator
    )
    meiso_enable_warnings(meiso_composition_edge_simulator_test)
    meiso_set_target_folder(
        meiso_composition_edge_simulator_test
        "Meiso/Tests/integration"
    )
    meiso_register_target_test(
        meiso_composition_edge_simulator_test "integration"
    )

    meiso_add_c_test(
        meiso_composition_edge_simulator_composition_test
        tests/test_composition_edge_simulator_composition.c
        integration
        meiso_composition_edge_simulator
    )

    add_executable(
        meiso_composition_edge_trace_test
        tests/test_composition_edge_trace.c
        $<TARGET_OBJECTS:meiso_composition_edge_client_fixture_test_object>
    )
    target_include_directories(
        meiso_composition_edge_trace_test
        PRIVATE tests
    )
    target_link_libraries(
        meiso_composition_edge_trace_test
        PRIVATE meiso_composition_edge_trace
    )
    meiso_enable_warnings(meiso_composition_edge_trace_test)
    meiso_set_target_folder(
        meiso_composition_edge_trace_test
        "Meiso/Tests/integration"
    )
    meiso_register_target_test(
        meiso_composition_edge_trace_test "integration"
    )

    find_package(Threads REQUIRED)
    add_executable(
        meiso_composition_host_bridge_test
        tests/test_composition_host_bridge.c
        applications/host/composition_host_bridge.c
        applications/host/meiso_composition_host_bridge_commands.c
        applications/host/meiso_composition_host_bridge_config.c
        applications/host/meiso_composition_host_bridge_media.c
        applications/host/meiso_composition_host_bridge_process.c
        applications/host/meiso_composition_host_bridge_truth.c
    )
    target_compile_definitions(
        meiso_composition_host_bridge_test
        PRIVATE MEISO_COMPOSITION_HOST_BRIDGE_NO_MAIN=1
    )
    target_include_directories(
        meiso_composition_host_bridge_test
        PRIVATE devtools
    )
    target_link_libraries(
        meiso_composition_host_bridge_test
        PRIVATE
            meiso_composition_remote_edge
            meiso_composition_edge_simulator
            Threads::Threads
    )
    meiso_enable_warnings(meiso_composition_host_bridge_test)
    meiso_set_target_folder(
        meiso_composition_host_bridge_test
        "Meiso/Tests/service"
    )
    meiso_register_target_test(meiso_composition_host_bridge_test "service")
    set_tests_properties(
        meiso_composition_host_bridge_test
        PROPERTIES TIMEOUT 90
    )
endif()

