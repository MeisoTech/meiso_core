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

    add_library(
        meiso_wifi_linux_adapter
        targets/k230d-linux/adapters/wifi_linux_adapter.c
    )
    meiso_configure_public_target(meiso_wifi_linux_adapter WifiLinuxAdapter)
    target_link_libraries(
        meiso_wifi_linux_adapter
        PUBLIC meiso_wifi_telemetry_profile
    )
    meiso_enable_warnings(meiso_wifi_linux_adapter)
    meiso_set_target_folder(meiso_wifi_linux_adapter "Meiso/Adapters/Linux")

    add_library(
        meiso_device_telemetry_linux_adapter
        targets/k230d-linux/adapters/device_telemetry_linux_adapter.c
    )
    meiso_configure_public_target(
        meiso_device_telemetry_linux_adapter
        DeviceTelemetryLinuxAdapter
    )
    target_link_libraries(
        meiso_device_telemetry_linux_adapter
        PUBLIC meiso_device_telemetry_profile
    )
    meiso_enable_warnings(meiso_device_telemetry_linux_adapter)
    meiso_set_target_folder(
        meiso_device_telemetry_linux_adapter
        "Meiso/Adapters/Linux"
    )

    add_library(
        meiso_camera_v4l2_adapter
        targets/k230d-linux/adapters/camera_v4l2_adapter.c
        targets/k230d-linux/adapters/camera_v4l2_adapter_diagnostics.c
        targets/k230d-linux/adapters/camera_v4l2_adapter_frames.c
        targets/k230d-linux/adapters/camera_v4l2_adapter_syscalls.c
    )
    meiso_configure_public_target(meiso_camera_v4l2_adapter CameraV4l2Adapter)
    target_link_libraries(
        meiso_camera_v4l2_adapter
        PUBLIC meiso_composition_profile
    )
    meiso_enable_warnings(meiso_camera_v4l2_adapter)
    meiso_set_target_folder(meiso_camera_v4l2_adapter "Meiso/Adapters/Linux")

    # Portable policy/state-machine core only. A native libdrm translator and
    # one-shot target runner remain a separate, explicitly staged K230D unit.
    add_library(
        meiso_k230d_drm_scanout
        targets/k230d-linux/adapters/drm_scanout_adapter.c
        targets/k230d-linux/adapters/drm_scanout_admission.c
        targets/k230d-linux/adapters/drm_scanout_cleanup.c
        targets/k230d-linux/adapters/drm_scanout_convert.c
        targets/k230d-linux/adapters/drm_scanout_lifecycle.c
        targets/k230d-linux/adapters/drm_scanout_ops.c
        targets/k230d-linux/adapters/drm_scanout_sha256.c
    )
    meiso_configure_public_target(
        meiso_k230d_drm_scanout K230dDrmScanout
    )
    meiso_enable_warnings(meiso_k230d_drm_scanout)
    meiso_set_target_folder(
        meiso_k230d_drm_scanout "Meiso/Adapters/Linux"
    )
    add_library(
        Meiso::K230dDrmScanout ALIAS meiso_k230d_drm_scanout
    )
endif()
