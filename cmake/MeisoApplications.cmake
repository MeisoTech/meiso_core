# Executables and evidence runners. Reusable SDK behavior belongs in a library;
# process entry points should remain thin and depend on Integration targets.

option(
    MEISO_BUILD_K230D_LINUX_APPS
    "Expose the K230D Linux cross-application target set"
    OFF
)

if(MEISO_BUILD_K230D_LINUX_APPS)
    if(NOT MEISO_K230_LINUX_TARGET)
        message(
            FATAL_ERROR
            "K230D applications require the K230D Linux toolchain contract"
        )
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
        message(
            FATAL_ERROR
            "K230D applications require a Linux/riscv64 target platform"
        )
    endif()

    add_library(meiso_k230d_linux_target_contract INTERFACE)
    target_compile_definitions(
        meiso_k230d_linux_target_contract
        INTERFACE MEISO_K230_LINUX_TARGET=1
    )
    meiso_set_target_folder(
        meiso_k230d_linux_target_contract
        "Meiso/Targets/K230D Linux"
    )
endif()

if(TARGET meiso_composition_remote_edge AND
   TARGET meiso_composition_edge_simulator)
    add_executable(
        meiso_composition_host_bridge
        applications/host/composition_host_bridge.c
        applications/host/meiso_composition_host_bridge_commands.c
        applications/host/meiso_composition_host_bridge_config.c
        applications/host/meiso_composition_host_bridge_media.c
        applications/host/meiso_composition_host_bridge_process.c
        applications/host/meiso_composition_host_bridge_truth.c
    )
    target_link_libraries(
        meiso_composition_host_bridge
        PRIVATE
            meiso_composition_remote_edge
            meiso_composition_edge_simulator
    )
    meiso_enable_warnings(meiso_composition_host_bridge)
    meiso_set_target_folder(meiso_composition_host_bridge "Meiso/Applications")
endif()

if(MEISO_BUILD_K230D_LINUX_APPS AND
   TARGET meiso_k230d_composition_edge_runtime)
    find_package(Threads REQUIRED)
    add_executable(
        meiso_k230d_composition_edge_service
        targets/k230d-linux/composition_edge_service.c
        targets/k230d-linux/composition_edge_service_config.c
        targets/k230d-linux/composition_edge_service_lifecycle.c
        targets/k230d-linux/composition_edge_service_run.c
        targets/k230d-linux/composition_edge_service_run_loop.c
    )
    target_link_libraries(
        meiso_k230d_composition_edge_service
        PRIVATE meiso_k230d_composition_edge_runtime Threads::Threads
    )
    target_link_libraries(
        meiso_k230d_composition_edge_service
        PRIVATE meiso_k230d_linux_target_contract
    )
    # FindThreads cannot link-probe while the cross toolchain deliberately
    # uses STATIC_LIBRARY try-compiles. Keep the exact service requirement on
    # its owning target instead of relying on a false libc-pthread probe.
    target_compile_options(
        meiso_k230d_composition_edge_service
        PRIVATE -pthread
    )
    target_link_options(
        meiso_k230d_composition_edge_service
        PRIVATE -pthread
    )
    meiso_enable_warnings(meiso_k230d_composition_edge_service)
    meiso_set_target_folder(
        meiso_k230d_composition_edge_service
        "Meiso/Applications/Edge"
    )
endif()

if(UNIX)
    add_executable(
        meiso_display_ingress_simulated_service
        devtools/display_ingress_simulated_service.c
        devtools/display_ingress_simulated_service_bootstrap.c
        devtools/display_ingress_simulated_service_control.c
        devtools/display_ingress_simulated_service_edge.c
        devtools/display_ingress_simulated_service_frame_input.c
        devtools/display_ingress_simulated_service_host.c
        devtools/display_ingress_simulated_service_loop.c
        devtools/display_ingress_simulated_service_receipt.c
    )
    target_link_libraries(
        meiso_display_ingress_simulated_service
        PRIVATE
            meiso_posix_tcp_adapter
            meiso_display_ingress_media_posix_udp_adapter
            meiso_display_ingress_host_fixture_input
    )
    meiso_enable_warnings(meiso_display_ingress_simulated_service)
    meiso_set_target_folder(
        meiso_display_ingress_simulated_service
        "Meiso/Applications/Lab"
    )

    set(
        MEISO_FOUR_KIND_LAB_APPLICATION_SOURCES
        devtools/four_kind_lab/four_kind_lab.c
        devtools/four_kind_lab/four_kind_lab_system.c
        devtools/four_kind_lab/four_kind_lab_thermal.c
        devtools/four_kind_lab/four_kind_lab_network.c
        devtools/four_kind_lab/four_kind_lab_connection.c
        devtools/four_kind_lab/four_kind_lab_output.c
        devtools/four_kind_lab/four_kind_lab_host.c
        devtools/four_kind_lab/four_kind_lab_edge.c
        devtools/four_kind_lab/four_kind_lab_selftest.c
    )
    add_executable(
        meiso_four_kind_lab
        ${MEISO_FOUR_KIND_LAB_APPLICATION_SOURCES}
    )
    target_link_libraries(
        meiso_four_kind_lab
        PRIVATE meiso_posix_tcp_adapter meiso_four_kind_lab_protocol
    )
    if(MEISO_BUILD_K230D_LINUX_APPS)
        target_link_libraries(
            meiso_four_kind_lab
            PRIVATE meiso_k230d_linux_target_contract
        )
    endif()
    meiso_enable_warnings(meiso_four_kind_lab)
    meiso_set_target_folder(meiso_four_kind_lab "Meiso/Applications/Lab")
endif()

add_executable(
    meiso_composition_offscreen_runner
    targets/k230d-linux/composition_offscreen_runner.c
)
target_link_libraries(
    meiso_composition_offscreen_runner
    PRIVATE
        meiso_composition_coordinator
        meiso_composition_reference_renderer
)
if(MEISO_BUILD_K230D_LINUX_APPS)
    target_link_libraries(
        meiso_composition_offscreen_runner
        PRIVATE meiso_k230d_linux_target_contract
    )
endif()
meiso_enable_warnings(meiso_composition_offscreen_runner)
meiso_set_target_folder(
    meiso_composition_offscreen_runner
    "Meiso/Applications/Evidence"
)

if(MEISO_BUILD_K230D_LINUX_APPS)
    add_executable(
        meiso_k230d_camera_v4l2_capture_once
        targets/k230d-linux/camera_v4l2_capture_once.c
    )
    target_link_libraries(
        meiso_k230d_camera_v4l2_capture_once
        PRIVATE
            meiso_camera_v4l2_adapter
            meiso_k230d_linux_target_contract
    )
    meiso_enable_warnings(meiso_k230d_camera_v4l2_capture_once)
    meiso_set_target_folder(
        meiso_k230d_camera_v4l2_capture_once
        "Meiso/Applications/Edge"
    )

    add_executable(
        meiso_k230d_wifi_runtime_edge
        targets/k230d-linux/wifi_runtime_edge.c
        targets/k230d-linux/wifi_runtime_edge_config.c
        targets/k230d-linux/wifi_runtime_edge_bootstrap.c
        targets/k230d-linux/wifi_runtime_edge_truth.c
        targets/k230d-linux/wifi_runtime_edge_telemetry.c
        targets/k230d-linux/wifi_runtime_edge_pump.c
        targets/k230d-linux/wifi_runtime_edge_lifecycle.c
    )
    target_link_libraries(
        meiso_k230d_wifi_runtime_edge
        PRIVATE
            meiso_wifi_linux_adapter
            meiso_posix_tcp_adapter
            meiso_k230d_linux_target_contract
    )
    meiso_enable_warnings(meiso_k230d_wifi_runtime_edge)
    meiso_set_target_folder(
        meiso_k230d_wifi_runtime_edge
        "Meiso/Applications/Edge"
    )

    add_custom_target(
        meiso_k230d_linux_applications
        DEPENDS
            meiso_k230d_camera_v4l2_capture_once
            meiso_k230d_composition_edge_service
            meiso_composition_offscreen_runner
            meiso_four_kind_lab
            meiso_k230d_wifi_runtime_edge
    )
    meiso_set_target_folder(
        meiso_k230d_linux_applications
        "Meiso/Targets/K230D Linux"
    )
endif()

if(APPLE AND TARGET meiso_posix_tcp_adapter)
    add_executable(
        meiso_session_tcp_loopback
        devtools/macos/session_tcp_loopback.c
    )
    target_link_libraries(
        meiso_session_tcp_loopback
        PRIVATE meiso_posix_tcp_adapter
    )
    meiso_enable_warnings(meiso_session_tcp_loopback)
    meiso_set_target_folder(meiso_session_tcp_loopback "Meiso/Applications/Lab")

    add_executable(
        meiso_wifi_runtime_host
        devtools/macos/wifi_runtime_host.c
        devtools/macos/meiso_wifi_runtime_host_config.c
        devtools/macos/meiso_wifi_runtime_host_bootstrap.c
        devtools/macos/meiso_wifi_runtime_host_pump.c
        devtools/macos/meiso_wifi_runtime_host_catalog.c
        devtools/macos/meiso_wifi_runtime_host_truth.c
        devtools/macos/meiso_wifi_runtime_host_commands.c
        devtools/macos/meiso_wifi_runtime_host_lifecycle.c
    )
    target_link_libraries(
        meiso_wifi_runtime_host
        PRIVATE meiso_posix_tcp_adapter meiso_wifi_telemetry_profile
    )
    meiso_enable_warnings(meiso_wifi_runtime_host)
    meiso_set_target_folder(meiso_wifi_runtime_host "Meiso/Applications/Lab")
endif()
