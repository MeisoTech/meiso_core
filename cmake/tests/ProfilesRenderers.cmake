meiso_add_c_test(meiso_display_ingress_application_test
    tests/test_display_ingress_application.c profile meiso_display_ingress_profile)
meiso_add_c_test(meiso_display_ingress_planes_test
    tests/test_display_ingress_planes.c profile meiso_display_ingress_profile)
meiso_add_c_test(meiso_display_ingress_tracking_test
    tests/test_display_ingress_tracking.c profile meiso_display_ingress_profile)
meiso_add_c_test(meiso_display_ingress_input_test
    tests/test_display_ingress_input.c profile meiso_display_ingress_profile)
meiso_add_c_test(meiso_display_ingress_edge_extensions_test
    tests/test_display_ingress_edge_extensions.c profile meiso_display_ingress_profile)
meiso_add_c_test(meiso_display_ingress_camera_test
    tests/test_display_ingress_camera.c profile meiso_display_ingress_profile)
# Profile, renderer, offscreen target-policy, and Composition scheduling lanes.

meiso_add_c_test(
    meiso_wifi_telemetry_profile_test
    tests/test_wifi_telemetry_profile.c
    profile
    meiso_wifi_telemetry_profile
)
if(MEISO_BUILD_LEGACY_COMPOSITION)
    meiso_add_c_test(
        meiso_four_kind_lab_protocol_test
        tests/test_four_kind_lab_protocol.c
        profile
        meiso_four_kind_lab_protocol
    )
endif()

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
if(MEISO_BUILD_LEGACY_COMPOSITION)
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
endif()

meiso_add_c_test(
    meiso_composition_media_wire_test
    tests/test_composition_media_wire.c
    profile
    meiso_composition_profile
)
