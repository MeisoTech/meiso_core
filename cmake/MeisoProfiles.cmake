# Product Profiles encode domain truth. Platform I/O and process entry points
# belong to Adapter, Integration or Application modules instead.

add_library(
    meiso_wifi_telemetry_profile
    profiles/wifi/wifi_telemetry.c
)
meiso_configure_public_target(meiso_wifi_telemetry_profile WifiTelemetryProfile)
target_link_libraries(meiso_wifi_telemetry_profile PUBLIC meiso_runtime)
meiso_enable_warnings(meiso_wifi_telemetry_profile)
meiso_set_target_folder(meiso_wifi_telemetry_profile "Meiso/Profiles")
add_library(Meiso::WifiTelemetryProfile ALIAS meiso_wifi_telemetry_profile)

add_library(
    meiso_device_telemetry_profile
    profiles/device/device_telemetry.c
)
meiso_configure_public_target(
    meiso_device_telemetry_profile
    DeviceTelemetryProfile
)
target_link_libraries(meiso_device_telemetry_profile PUBLIC meiso_runtime)
meiso_enable_warnings(meiso_device_telemetry_profile)
meiso_set_target_folder(meiso_device_telemetry_profile "Meiso/Profiles")
add_library(Meiso::DeviceTelemetryProfile ALIAS meiso_device_telemetry_profile)

# Profiles retain only the versioned Composition schema/catalog/media contract.
# Scene storage, coordination and rendering are defined by MeisoComposition.
add_library(
    meiso_composition_scene_schema
    profiles/composition/composition_scene.c
    profiles/composition/meiso_composition_scene_desired.c
    profiles/composition/meiso_composition_scene_mesh.c
    profiles/composition/meiso_composition_scene_presentation.c
    profiles/composition/meiso_composition_scene_private.c
    profiles/composition/meiso_composition_scene_result.c
    profiles/composition/composition_scene_identity.c
)
meiso_configure_public_target(
    meiso_composition_scene_schema
    CompositionSceneSchema
)
target_link_libraries(meiso_composition_scene_schema PUBLIC meiso_runtime)
meiso_enable_warnings(meiso_composition_scene_schema)
meiso_set_target_folder(
    meiso_composition_scene_schema
    "Meiso/Profiles/Composition"
)
add_library(
    Meiso::CompositionSceneSchema
    ALIAS meiso_composition_scene_schema
)

if(MEISO_BUILD_LEGACY_COMPOSITION)
    add_library(
        meiso_composition_session_v2_profile
        profiles/internal/composition_v2/composition_session_v2.c
        profiles/internal/composition_v2/composition_profile_fixture.h
    )
    meiso_configure_public_target(
        meiso_composition_session_v2_profile
        CompositionSessionV2Profile
    )
    target_link_libraries(
        meiso_composition_session_v2_profile
        PUBLIC meiso_composition_scene_schema meiso_wifi_telemetry_profile
    )
    meiso_enable_warnings(meiso_composition_session_v2_profile)
    target_include_directories(
        meiso_composition_session_v2_profile
        PRIVATE profiles/internal/composition_v2
    )
    meiso_set_target_folder(
        meiso_composition_session_v2_profile
        "Meiso/Internal/Protocol"
    )
endif()

add_library(
    meiso_composition_profile
    profiles/composition/composition_profile.c
    profiles/composition/meiso_composition_profile_catalog.c
    profiles/composition/meiso_composition_profile_media_state.c
    profiles/composition/meiso_composition_profile_media_stream.c
    profiles/composition/meiso_composition_profile_private.c
)
meiso_configure_public_target(meiso_composition_profile CompositionProfile)
target_link_libraries(
    meiso_composition_profile
    PUBLIC
        meiso_composition_scene_schema
        meiso_wifi_telemetry_profile
        meiso_device_telemetry_profile
)
meiso_enable_warnings(meiso_composition_profile)
meiso_set_target_folder(meiso_composition_profile "Meiso/Profiles")
add_library(Meiso::CompositionProfile ALIAS meiso_composition_profile)

add_library(
    meiso_composition_media_transport
    profiles/composition/composition_media_transport.c
    profiles/composition/composition_media_transport_outbound.c
    profiles/composition/composition_media_transport_receive.c
    profiles/composition/composition_media_transport_private.c
)
meiso_configure_public_target(
    meiso_composition_media_transport
    CompositionMediaTransport
)
target_link_libraries(
    meiso_composition_media_transport
    PUBLIC meiso_composition_profile
)
meiso_enable_warnings(meiso_composition_media_transport)
meiso_set_target_folder(meiso_composition_media_transport "Meiso/Profiles")
add_library(
    Meiso::CompositionMediaTransport
    ALIAS meiso_composition_media_transport
)

add_library(
    meiso_display_ingress_profile
    profiles/display_ingress/display_ingress_profile.c
    profiles/display_ingress/display_ingress_digest.c
    profiles/display_ingress/display_ingress_overlay.c
    profiles/display_ingress/display_ingress_scene.c
    profiles/display_ingress/display_ingress_camera.c
    profiles/display_ingress/display_ingress_application.c
    profiles/display_ingress/display_ingress_planes.c
    profiles/display_ingress/display_ingress_tracking.c
    profiles/display_ingress/display_ingress_profile_codec.c
    profiles/display_ingress/display_ingress_projection.c
)
meiso_configure_public_target(
    meiso_display_ingress_profile
    DisplayIngressProfile
)
target_link_libraries(meiso_display_ingress_profile PUBLIC meiso_runtime meiso_composition_profile)
meiso_enable_warnings(meiso_display_ingress_profile)
meiso_set_target_folder(meiso_display_ingress_profile "Meiso/Profiles")
add_library(
    Meiso::DisplayIngressProfile
    ALIAS meiso_display_ingress_profile
)

add_library(
    meiso_display_ingress_media_transport
    profiles/display_ingress/display_ingress_media_transport.c
    profiles/display_ingress/display_ingress_media_transport_codec.c
    profiles/display_ingress/display_ingress_media_transport_outbound.c
    profiles/display_ingress/display_ingress_media_transport_receive.c
    profiles/display_ingress/display_ingress_media_transport_private.c
)
meiso_configure_public_target(
    meiso_display_ingress_media_transport
    DisplayIngressMediaTransport
)
target_link_libraries(
    meiso_display_ingress_media_transport
    PUBLIC meiso_display_ingress_profile
)
meiso_enable_warnings(meiso_display_ingress_media_transport)
meiso_set_target_folder(
    meiso_display_ingress_media_transport "Meiso/Profiles"
)
add_library(
    Meiso::DisplayIngressMediaTransport
    ALIAS meiso_display_ingress_media_transport
)

if(MEISO_BUILD_LEGACY_COMPOSITION)
    add_library(
        meiso_four_kind_lab_protocol
        profiles/internal/four_kind/lab_protocol.c
        profiles/internal/four_kind/lab_protocol_message.c
        profiles/internal/four_kind/lab_protocol_order.c
    )
    meiso_configure_public_target(meiso_four_kind_lab_protocol FourKindLabProtocol)
    target_link_libraries(meiso_four_kind_lab_protocol PUBLIC meiso_core_resources)
    meiso_enable_warnings(meiso_four_kind_lab_protocol)
    meiso_set_target_folder(meiso_four_kind_lab_protocol "Meiso/Internal/Lab")
endif()
