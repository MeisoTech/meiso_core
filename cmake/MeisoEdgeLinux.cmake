# Linux Edge integration assembles the K230D runtime from protocol services,
# composition services, and target-specific platform adapters.

if(UNIX)
    add_library(
        meiso_k230d_composition_edge_runtime
        targets/k230d-linux/composition_edge_runtime.c
        targets/k230d-linux/composition_edge_runtime_composition.c
        targets/k230d-linux/composition_edge_runtime_control.c
        targets/k230d-linux/composition_edge_runtime_desired.c
        targets/k230d-linux/composition_edge_runtime_media.c
        targets/k230d-linux/composition_edge_runtime_platform.c
        targets/k230d-linux/composition_edge_runtime_pump.c
        targets/k230d-linux/composition_edge_runtime_transfer.c
    )
    meiso_configure_public_target(
        meiso_k230d_composition_edge_runtime
        K230dCompositionEdgeRuntime
    )
    target_link_libraries(
        meiso_k230d_composition_edge_runtime
        PUBLIC
            meiso_composition_control_session
            meiso_composition_media_posix_tcp_adapter
            meiso_wifi_linux_adapter
            meiso_device_telemetry_linux_adapter
            meiso_camera_v4l2_adapter
            meiso_composition_coordinator
            meiso_composition_cpu_raster_renderer
    )
    meiso_enable_warnings(meiso_k230d_composition_edge_runtime)
    meiso_set_target_folder(
        meiso_k230d_composition_edge_runtime
        "Meiso/Integrations/Edge"
    )
    add_library(
        Meiso::K230dCompositionEdgeRuntime
        ALIAS meiso_k230d_composition_edge_runtime
    )
endif()
