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

# Pairing v0 (docs/pairing-v0.md): how an Edge and a Host first learn each other's
# PeerId, outside and before a Session. Codecs only; PeerId is Session Core's.
add_library(meiso_pairing src/pairing/meiso_pairing.c)
meiso_configure_public_target(meiso_pairing Pairing)
target_link_libraries(meiso_pairing PUBLIC meiso_core_session)
meiso_enable_warnings(meiso_pairing)
meiso_set_target_folder(meiso_pairing "Meiso/Core")
add_library(Meiso::Pairing ALIAS meiso_pairing)

add_library(meiso_core INTERFACE)
target_link_libraries(
    meiso_core
    INTERFACE meiso_core_resources meiso_core_session
)
meiso_configure_public_target(meiso_core Core)
add_library(Meiso::Core ALIAS meiso_core)
