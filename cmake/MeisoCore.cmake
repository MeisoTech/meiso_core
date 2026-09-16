# Portable Core owns values, delivery primitives and Session. Runtime consumes
# Core but Core never depends on a Profile, platform adapter or application.

add_library(meiso_core_resources
    src/core/meiso_resource.c
    src/core/meiso_sha256.c
    src/core/meiso_state.c
    src/core/meiso_stream.c
    src/core/meiso_blob.c
    src/core/meiso_descriptor_set.c
    src/core/meiso_action.c
    src/core/meiso_action_private.c
    src/core/meiso_action_caller.c
    src/core/meiso_action_executor.c
)
meiso_configure_public_target(meiso_core_resources CoreResources)
meiso_enable_warnings(meiso_core_resources)
meiso_set_target_folder(meiso_core_resources "Meiso/Core")
add_library(Meiso::CoreResources ALIAS meiso_core_resources)

add_library(meiso_core_session
    src/core/meiso_session.c
    src/core/meiso_session_wire.c
    src/core/meiso_session_receive.c
    src/core/meiso_session_lifecycle.c
)
meiso_configure_public_target(meiso_core_session CoreSession)
target_link_libraries(meiso_core_session PUBLIC meiso_core_resources)
meiso_enable_warnings(meiso_core_session)
meiso_set_target_folder(meiso_core_session "Meiso/Core")
add_library(Meiso::CoreSession ALIAS meiso_core_session)

add_library(meiso_core INTERFACE)
target_link_libraries(
    meiso_core
    INTERFACE meiso_core_resources meiso_core_session
)
meiso_configure_public_target(meiso_core Core)
add_library(Meiso::Core ALIAS meiso_core)
