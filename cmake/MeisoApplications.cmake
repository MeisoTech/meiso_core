# Executables and evidence runners. Reusable SDK behavior belongs in a library;
# process entry points should remain thin and depend on Integration targets.

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

if(UNIX)
    if(MEISO_BUILD_LEGACY_COMPOSITION)
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
        meiso_enable_warnings(meiso_four_kind_lab)
        meiso_set_target_folder(meiso_four_kind_lab "Meiso/Applications/Lab")
    endif()

endif()

if(APPLE AND TARGET meiso_posix_tcp_adapter AND MEISO_BUILD_LEGACY_COMPOSITION)
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

# The Display Ingress service, the K230D target applications and the desktop Edge
# moved to meiso_edge (S-027); they build against this source from there.
