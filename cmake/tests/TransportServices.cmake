# Media/POSIX transport, runtime TCP integration, and lab service lanes.

meiso_add_c_test(
    meiso_runtime_v4_test
    tests/test_runtime_v4.c
    runtime
    meiso_composition_profile
)
meiso_add_c_test(
    meiso_composition_media_transport_test
    tests/test_composition_media_transport.c
    profile
    meiso_composition_media_transport
)
meiso_add_c_test(
    meiso_display_ingress_media_transport_test
    tests/test_display_ingress_media_transport.c
    profile
    meiso_display_ingress_media_transport
)

if(UNIX)
    if(TARGET meiso_display_ingress_simulated_service)
        add_executable(
            meiso_display_ingress_simulated_service_test
            tests/test_display_ingress_simulated_service.c
        )
        add_dependencies(
            meiso_display_ingress_simulated_service_test
            meiso_display_ingress_simulated_service
        )
        meiso_enable_warnings(meiso_display_ingress_simulated_service_test)
        meiso_set_target_folder(
            meiso_display_ingress_simulated_service_test
            "Meiso/Tests/service"
        )
        meiso_register_target_test(
            meiso_display_ingress_simulated_service_test
            "service"
            $<TARGET_FILE:meiso_display_ingress_simulated_service>
        )
        set_tests_properties(
            meiso_display_ingress_simulated_service_test
            PROPERTIES TIMEOUT 60
        )
    endif()

    add_executable(
        meiso_composition_media_posix_tcp_test
        tests/test_composition_media_posix_tcp.c
        adapters/posix/composition_media_posix_tcp.c
    )
    target_compile_definitions(
        meiso_composition_media_posix_tcp_test
        PRIVATE send=meiso_test_send recv=meiso_test_recv
    )
    target_link_libraries(
        meiso_composition_media_posix_tcp_test
        PRIVATE meiso_composition_media_transport
    )
    target_include_directories(
        meiso_composition_media_posix_tcp_test
        PRIVATE adapters/posix
    )
    meiso_enable_warnings(meiso_composition_media_posix_tcp_test)
    meiso_set_target_folder(
        meiso_composition_media_posix_tcp_test
        "Meiso/Tests/adapter"
    )
    meiso_register_target_test(
        meiso_composition_media_posix_tcp_test "adapter"
    )

    add_executable(
        meiso_display_ingress_media_posix_udp_test
        tests/test_display_ingress_media_posix_udp.c
        adapters/posix/display_ingress_media_posix_udp.c
    )
    target_compile_definitions(
        meiso_display_ingress_media_posix_udp_test
        PRIVATE
            send=meiso_test_send
            recvmsg=meiso_test_recvmsg
    )
    target_link_libraries(
        meiso_display_ingress_media_posix_udp_test
        PRIVATE meiso_display_ingress_media_transport
    )
    meiso_enable_warnings(meiso_display_ingress_media_posix_udp_test)
    meiso_set_target_folder(
        meiso_display_ingress_media_posix_udp_test
        "Meiso/Tests/adapter"
    )
    meiso_register_target_test(
        meiso_display_ingress_media_posix_udp_test "adapter"
    )
endif()

if(TARGET meiso_posix_tcp_adapter)
    meiso_add_c_test(
        meiso_posix_tcp_test
        tests/test_posix_tcp.c
        adapter
        meiso_posix_tcp_adapter
    )
    meiso_add_c_test(
        meiso_wifi_runtime_tcp_test
        tests/test_wifi_runtime_tcp.c
        integration
        meiso_posix_tcp_adapter
        meiso_wifi_telemetry_profile
    )
    meiso_add_c_test(
        meiso_composition_runtime_tcp_test
        tests/test_composition_runtime_tcp.c
        integration
        meiso_posix_tcp_adapter
        meiso_runtime_scheduler
        meiso_composition_session_v2_profile
        meiso_composition_coordinator
    )
    meiso_add_c_test(
        meiso_composition_runtime_tcp_adversarial_test
        tests/test_composition_runtime_tcp_adversarial.c
        integration
        meiso_posix_tcp_adapter
        meiso_runtime_scheduler
        meiso_composition_session_v2_profile
    )
    target_include_directories(
        meiso_composition_runtime_tcp_test
        PRIVATE profiles/internal/composition_v2
    )
    target_include_directories(
        meiso_composition_runtime_tcp_adversarial_test
        PRIVATE profiles/internal/composition_v2
    )
endif()

if(TARGET meiso_four_kind_lab)
    add_executable(
        meiso_four_kind_lab_internal_test
        ${MEISO_FOUR_KIND_LAB_APPLICATION_SOURCES}
    )
    target_compile_definitions(
        meiso_four_kind_lab_internal_test
        PRIVATE MEISO_LAB_INTERNAL_TEST=1
    )
    target_link_libraries(
        meiso_four_kind_lab_internal_test
        PRIVATE meiso_posix_tcp_adapter meiso_four_kind_lab_protocol
    )
    meiso_enable_warnings(meiso_four_kind_lab_internal_test)
    meiso_set_target_folder(
        meiso_four_kind_lab_internal_test
        "Meiso/Tests/service"
    )
    meiso_register_target_test(
        meiso_four_kind_lab_internal_test "service" --self-test
    )
endif()
