meiso_add_c_test(meiso_display_ingress_camera_test
    tests/test_display_ingress_camera.c profile meiso_display_ingress_profile)
# Profile, renderer, offscreen target-policy, and Composition scheduling lanes.
meiso_add_c_test(meiso_display_ingress_overlay_test
    tests/test_display_ingress_overlay.c profile meiso_display_ingress_profile)
target_sources(meiso_display_ingress_overlay_test PRIVATE
    targets/k230d-linux/display_ingress_overlay_raster.c)

meiso_add_c_test(
    meiso_wifi_telemetry_profile_test
    tests/test_wifi_telemetry_profile.c
    profile
    meiso_wifi_telemetry_profile
)
meiso_add_c_test(
    meiso_four_kind_lab_protocol_test
    tests/test_four_kind_lab_protocol.c
    profile
    meiso_four_kind_lab_protocol
)
meiso_add_c_test(
    meiso_display_ingress_profile_test
    tests/test_display_ingress_profile.c
    profile
    meiso_display_ingress_profile
)
meiso_add_c_test(meiso_display_projection_test
    tests/test_display_projection.c profile meiso_display_ingress_profile)
meiso_add_c_test(
    meiso_composition_scene_test
    tests/test_composition_scene.c
    profile
    meiso_composition_scene_schema
    meiso_wifi_telemetry_profile
)
meiso_add_c_test(
    meiso_composition_scene_resource_test
    tests/test_composition_scene_resource.c
    profile
    meiso_composition_scene_store
)
meiso_add_c_test(
    meiso_composition_reference_renderer_test
    tests/test_composition_reference_renderer.c
    renderer
    meiso_composition_reference_renderer
)
meiso_add_c_test(
    meiso_composition_cpu_raster_renderer_test
    tests/test_composition_cpu_raster_renderer.c
    renderer
    meiso_composition_cpu_raster_renderer
)

add_executable(
    meiso_composition_offscreen_runner_k230d_contract
    targets/k230d-linux/composition_offscreen_runner.c
)
target_compile_definitions(
    meiso_composition_offscreen_runner_k230d_contract
    PRIVATE MEISO_K230_LINUX_TARGET=1
)
target_link_libraries(
    meiso_composition_offscreen_runner_k230d_contract
    PRIVATE
        meiso_composition_coordinator
        meiso_composition_reference_renderer
)
meiso_enable_warnings(meiso_composition_offscreen_runner_k230d_contract)
meiso_set_target_folder(
    meiso_composition_offscreen_runner_k230d_contract
    "Meiso/Tests/target-policy"
)

add_executable(
    meiso_composition_offscreen_runner_test
    tests/test_composition_offscreen_runner.c
    targets/k230d-linux/composition_offscreen_runner.c
)
target_compile_definitions(
    meiso_composition_offscreen_runner_test
    PRIVATE
        MEISO_COMPOSITION_OFFSCREEN_RUNNER_NO_MAIN=1
        MEISO_OFFSCREEN_RUNNER_SOURCE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/targets/k230d-linux/composition_offscreen_runner.c"
        MEISO_OFFSCREEN_CROSS_SCRIPT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/targets/k230d-linux/build_composition_offscreen_runner.sh"
        MEISO_K230D_CMAKE_HELPER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/targets/k230d-linux/_build_cmake_target.sh"
        MEISO_K230D_XUANTIE_TOOLCHAIN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/k230d-linux-xuantie.cmake"
)
target_link_libraries(
    meiso_composition_offscreen_runner_test
    PRIVATE
        meiso_composition_coordinator
        meiso_composition_reference_renderer
)
meiso_enable_warnings(meiso_composition_offscreen_runner_test)
meiso_set_target_folder(
    meiso_composition_offscreen_runner_test
    "Meiso/Tests/target-policy"
)
meiso_register_target_test(
    meiso_composition_offscreen_runner_test "target-policy"
)

if(TARGET meiso_composition_offscreen_runner)
    add_test(
        NAME meiso_composition_offscreen_runner_native
        COMMAND meiso_composition_offscreen_runner
    )
    set_tests_properties(
        meiso_composition_offscreen_runner_native
        PROPERTIES LABELS "target-policy" TIMEOUT 60
    )
    set_tests_properties(
        meiso_composition_offscreen_runner_native
        PROPERTIES
            PASS_REGULAR_EXPRESSION "HOST_OFFSCREEN_REFERENCE_VERIFIED"
            FAIL_REGULAR_EXPRESSION "TARGET_OFFSCREEN_REFERENCE_VERIFIED"
    )
endif()

meiso_register_target_test(
    meiso_composition_offscreen_runner_k230d_contract "target-policy"
)
set_tests_properties(
    meiso_composition_offscreen_runner_k230d_contract
    PROPERTIES
        PASS_REGULAR_EXPRESSION
            "OFFSCREEN_REFERENCE_VECTOR_PASSED.*k230d_linux"
        FAIL_REGULAR_EXPRESSION "TARGET_.*VERIFIED"
)

find_program(MEISO_SH_EXECUTABLE sh)
if(MEISO_SH_EXECUTABLE)
    add_test(
        NAME meiso_composition_offscreen_cross_script_syntax
        COMMAND
            "${MEISO_SH_EXECUTABLE}" -n
            "${CMAKE_CURRENT_SOURCE_DIR}/targets/k230d-linux/build_composition_offscreen_runner.sh"
    )
    set_tests_properties(
        meiso_composition_offscreen_cross_script_syntax
        PROPERTIES LABELS "target-policy" TIMEOUT 30
    )
endif()

meiso_add_c_test(
    meiso_composition_coordinator_test
    tests/test_composition_coordinator.c
    profile
    meiso_composition_coordinator
)
meiso_add_c_test(
    meiso_composition_tx_scheduler_test
    tests/test_composition_tx_scheduler.c
    runtime
    meiso_runtime_scheduler
    meiso_composition_session_v2_profile
)
target_include_directories(
    meiso_composition_tx_scheduler_test
    PRIVATE profiles/internal/composition_v2
)
meiso_add_c_test(
    meiso_composition_media_wire_test
    tests/test_composition_media_wire.c
    profile
    meiso_composition_profile
)
