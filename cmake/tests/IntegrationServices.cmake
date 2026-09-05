# Platform adapters, Host/Edge integrations, and long-lived service lanes.

meiso_add_c_test(
    meiso_display_ingress_host_fixture_input_test
    tests/test_display_ingress_host_fixture_input.c
    integration
    meiso_display_ingress_host_fixture_input
)

if(TARGET meiso_device_telemetry_linux_adapter)
    meiso_add_c_test(
        meiso_device_telemetry_linux_adapter_test
        tests/test_device_telemetry_linux_adapter.c
        adapter
        meiso_device_telemetry_linux_adapter
    )
endif()
if(TARGET meiso_wifi_linux_adapter)
    meiso_add_c_test(
        meiso_wifi_linux_adapter_test
        tests/test_wifi_linux_adapter.c
        adapter
        meiso_wifi_linux_adapter
    )
endif()
if(TARGET meiso_camera_v4l2_adapter)
    meiso_add_c_test(
        meiso_camera_v4l2_adapter_test
        tests/test_camera_v4l2_adapter.c
        adapter
        meiso_camera_v4l2_adapter
    )

    add_executable(
        meiso_camera_v4l2_capture_once_test
        tests/test_camera_v4l2_capture_once.c
        targets/k230d-linux/camera_v4l2_capture_once.c
    )
    target_compile_definitions(
        meiso_camera_v4l2_capture_once_test
        PRIVATE MEISO_CAMERA_V4L2_CAPTURE_ONCE_NO_MAIN=1
    )
    target_link_libraries(
        meiso_camera_v4l2_capture_once_test
        PRIVATE meiso_camera_v4l2_adapter
    )
    meiso_enable_warnings(meiso_camera_v4l2_capture_once_test)
    meiso_set_target_folder(
        meiso_camera_v4l2_capture_once_test
        "Meiso/Tests/target-policy"
    )
    meiso_register_target_test(
        meiso_camera_v4l2_capture_once_test "target-policy"
    )
endif()

if(TARGET meiso_k230d_drm_scanout)
    meiso_add_c_test(
        meiso_k230d_drm_scanout_test
        tests/test_k230d_drm_scanout.c
        adapter
        meiso_k230d_drm_scanout
    )
    target_include_directories(
        meiso_k230d_drm_scanout_test
        PRIVATE targets/k230d-linux/adapters
    )

    meiso_add_c_test(
        meiso_k230d_drm_single_plane_test_only_test
        tests/test_k230d_drm_single_plane_test_only.c
        adapter
        meiso_k230d_drm_scanout
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_test
        PRIVATE targets/k230d-linux/adapters
    )

    add_executable(
        meiso_k230d_drm_topology_admission_once_test
        tests/test_k230d_drm_topology_admission_once.c
        targets/k230d-linux/drm_topology_admission_once.c
    )
    target_compile_definitions(
        meiso_k230d_drm_topology_admission_once_test
        PRIVATE MEISO_K230D_DRM_TOPOLOGY_ADMISSION_ONCE_NO_MAIN=1
    )
    target_include_directories(
        meiso_k230d_drm_topology_admission_once_test
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_topology_admission_once_test
        PRIVATE meiso_k230d_drm_scanout
    )
    meiso_enable_warnings(meiso_k230d_drm_topology_admission_once_test)
    meiso_set_target_folder(
        meiso_k230d_drm_topology_admission_once_test
        "Meiso/Tests/target-policy"
    )
    meiso_register_target_test(
        meiso_k230d_drm_topology_admission_once_test "target-policy"
    )

    add_executable(
        meiso_k230d_drm_single_plane_test_only_once_test
        tests/test_k230d_drm_single_plane_test_only_once.c
        targets/k230d-linux/drm_single_plane_test_only_once.c
    )
    target_compile_definitions(
        meiso_k230d_drm_single_plane_test_only_once_test
        PRIVATE MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_ONCE_NO_MAIN=1
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_once_test
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_single_plane_test_only_once_test
        PRIVATE meiso_k230d_drm_scanout
    )
    meiso_enable_warnings(meiso_k230d_drm_single_plane_test_only_once_test)
    meiso_set_target_folder(
        meiso_k230d_drm_single_plane_test_only_once_test
        "Meiso/Tests/target-policy"
    )
    meiso_register_target_test(
        meiso_k230d_drm_single_plane_test_only_once_test "target-policy"
    )
endif()

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

if(TARGET meiso_k230d_composition_edge_runtime)
    meiso_add_c_test(
        meiso_k230d_composition_edge_runtime_test
        tests/test_k230d_composition_edge_runtime.c
        integration
        meiso_k230d_composition_edge_runtime
    )
    target_sources(
        meiso_k230d_composition_edge_runtime_test
        PRIVATE tests/k230d_composition_edge_cadence_fixture.c
    )
    target_include_directories(
        meiso_k230d_composition_edge_runtime_test
        PRIVATE targets/k230d-linux
    )

    find_package(Threads REQUIRED)
    add_executable(
        meiso_k230d_composition_edge_service_lifecycle_test
        targets/k230d-linux/composition_edge_service.c
        targets/k230d-linux/composition_edge_service_config.c
        targets/k230d-linux/composition_edge_service_lifecycle.c
        targets/k230d-linux/composition_edge_service_run.c
        targets/k230d-linux/composition_edge_service_run_loop.c
    )
    target_compile_definitions(
        meiso_k230d_composition_edge_service_lifecycle_test
        PRIVATE MEISO_K230D_COMPOSITION_EDGE_SERVICE_LIFECYCLE_TEST=1
    )
    target_link_libraries(
        meiso_k230d_composition_edge_service_lifecycle_test
        PRIVATE meiso_k230d_composition_edge_runtime Threads::Threads
    )
    meiso_enable_warnings(meiso_k230d_composition_edge_service_lifecycle_test)
    meiso_set_target_folder(
        meiso_k230d_composition_edge_service_lifecycle_test
        "Meiso/Tests/service"
    )
    meiso_register_target_test(
        meiso_k230d_composition_edge_service_lifecycle_test
        "service"
        --lifecycle-self-test
    )
endif()
