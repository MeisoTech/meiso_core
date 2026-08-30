# Control binds portable Profile/Runtime work to a concrete control transport.
# The current implementation is POSIX-only and is exported separately from
# portable Composition state and rendering targets.

if(UNIX)
    add_library(
        meiso_composition_control_session
        integrations/composition/composition_control_session.c
        integrations/composition/meiso_composition_control_session_pump.c
        integrations/composition/meiso_composition_control_session_work.c
    )
    meiso_configure_public_target(
        meiso_composition_control_session
        CompositionControlSession
    )
    target_link_libraries(
        meiso_composition_control_session
        PUBLIC
            meiso_composition_profile
            meiso_posix_tcp_adapter
            meiso_runtime_scheduler
    )
    meiso_enable_warnings(meiso_composition_control_session)
    meiso_set_target_folder(meiso_composition_control_session "Meiso/Control")
    add_library(
        Meiso::CompositionControlSession
        ALIAS meiso_composition_control_session
    )
endif()
