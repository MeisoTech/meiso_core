# Host integrations bind the portable client contract to POSIX transport and
# control-session services. Operating-system concerns remain in adapters.

if(UNIX)
    add_library(
        meiso_composition_host_runtime
        integrations/host/composition_host_runtime.c
        integrations/host/meiso_composition_host_runtime_commands.c
        integrations/host/meiso_composition_host_runtime_control.c
        integrations/host/meiso_composition_host_runtime_events.c
        integrations/host/meiso_composition_host_runtime_media.c
        integrations/host/meiso_composition_host_runtime_pump.c
    )
    meiso_configure_public_target(
        meiso_composition_host_runtime
        CompositionHostRuntime
    )
    target_link_libraries(
        meiso_composition_host_runtime
        PUBLIC
            meiso_composition_control_session
            meiso_composition_media_posix_tcp_adapter
    )
    meiso_enable_warnings(meiso_composition_host_runtime)
    meiso_set_target_folder(
        meiso_composition_host_runtime
        "Meiso/Integrations/Host"
    )
    add_library(
        Meiso::CompositionHostRuntime
        ALIAS meiso_composition_host_runtime
    )

    add_library(
        meiso_composition_remote_edge
        integrations/host/composition_remote_edge.c
        integrations/host/composition_remote_edge_backend.c
        integrations/host/composition_remote_edge_snapshot.c
    )
    meiso_configure_public_target(
        meiso_composition_remote_edge
        CompositionRemoteEdge
    )
    target_include_directories(
        meiso_composition_remote_edge
        PRIVATE integrations/composition integrations/host
    )
    target_link_libraries(
        meiso_composition_remote_edge
        PUBLIC
            meiso_composition_edge
            meiso_composition_host_runtime
    )
    meiso_enable_warnings(meiso_composition_remote_edge)
    meiso_set_target_folder(
        meiso_composition_remote_edge
        "Meiso/Integrations/Host"
    )
    add_library(
        Meiso::CompositionRemoteEdge
        ALIAS meiso_composition_remote_edge
    )
endif()
