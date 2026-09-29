# Adapters translate a platform I/O boundary. They do not own Host/Edge role
# policy and must remain below the Integration layer.

if(UNIX)
    add_library(meiso_posix_tcp_adapter adapters/posix/meiso_posix_tcp.c)
    meiso_configure_public_target(meiso_posix_tcp_adapter PosixTcpAdapter)
    target_link_libraries(meiso_posix_tcp_adapter PUBLIC meiso_core_session)
    meiso_enable_warnings(meiso_posix_tcp_adapter)
    meiso_set_target_folder(meiso_posix_tcp_adapter "Meiso/Adapters")
    add_library(Meiso::PosixTcpAdapter ALIAS meiso_posix_tcp_adapter)

    add_library(
        meiso_composition_media_posix_tcp_adapter
        adapters/posix/composition_media_posix_tcp.c
    )
    meiso_configure_public_target(
        meiso_composition_media_posix_tcp_adapter
        CompositionMediaPosixTcpAdapter
    )
    target_link_libraries(
        meiso_composition_media_posix_tcp_adapter
        PUBLIC meiso_composition_media_transport
    )
    meiso_enable_warnings(meiso_composition_media_posix_tcp_adapter)
    meiso_set_target_folder(
        meiso_composition_media_posix_tcp_adapter
        "Meiso/Adapters"
    )
    add_library(
        Meiso::CompositionMediaPosixTcpAdapter
        ALIAS meiso_composition_media_posix_tcp_adapter
    )

    add_library(meiso_display_camera_channel integrations/display_camera_channel.c)
    meiso_configure_public_target(meiso_display_camera_channel DisplayCameraChannel)
    target_link_libraries(meiso_display_camera_channel PUBLIC meiso_composition_media_posix_tcp_adapter meiso_display_ingress_profile)
    meiso_enable_warnings(meiso_display_camera_channel)
    add_library(Meiso::DisplayCameraChannel ALIAS meiso_display_camera_channel)

    add_library(
        meiso_display_ingress_media_posix_udp_adapter
        adapters/posix/display_ingress_media_posix_udp.c
    )
    meiso_configure_public_target(
        meiso_display_ingress_media_posix_udp_adapter
        DisplayIngressMediaPosixUdpAdapter
    )
    target_link_libraries(
        meiso_display_ingress_media_posix_udp_adapter
        PUBLIC meiso_display_ingress_media_transport
    )
    meiso_enable_warnings(meiso_display_ingress_media_posix_udp_adapter)
    meiso_set_target_folder(
        meiso_display_ingress_media_posix_udp_adapter
        "Meiso/Adapters"
    )
    add_library(
        Meiso::DisplayIngressMediaPosixUdpAdapter
        ALIAS meiso_display_ingress_media_posix_udp_adapter
    )

endif()

# The K230D Linux adapters (Wi-Fi, device telemetry, V4L2 camera) moved to meiso_edge.

if(WIN32)
    add_library(meiso_windows_tcp_adapter adapters/windows/meiso_windows_tcp.c)
    meiso_configure_public_target(meiso_windows_tcp_adapter WindowsTcpAdapter)
    target_link_libraries(meiso_windows_tcp_adapter PUBLIC meiso_core_session ws2_32)
    meiso_enable_warnings(meiso_windows_tcp_adapter)
    meiso_set_target_folder(meiso_windows_tcp_adapter "Meiso/Adapters")
    add_library(Meiso::WindowsTcpAdapter ALIAS meiso_windows_tcp_adapter)

    add_library(meiso_display_ingress_media_windows_udp_adapter
        adapters/windows/display_ingress_media_windows_udp.c)
    meiso_configure_public_target(meiso_display_ingress_media_windows_udp_adapter
        DisplayIngressMediaWindowsUdpAdapter)
    target_link_libraries(meiso_display_ingress_media_windows_udp_adapter
        PUBLIC meiso_display_ingress_media_transport ws2_32)
    meiso_enable_warnings(meiso_display_ingress_media_windows_udp_adapter)
    meiso_set_target_folder(meiso_display_ingress_media_windows_udp_adapter "Meiso/Adapters")
    add_library(Meiso::DisplayIngressMediaWindowsUdpAdapter
        ALIAS meiso_display_ingress_media_windows_udp_adapter)
endif()
