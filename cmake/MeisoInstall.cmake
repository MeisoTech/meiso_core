# Relocatable CMake package exports. Components are intentionally layered so
# installing Core alone never copies Runtime/Profile/Host artifacts.

include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/MeisoSourceRevision.cmake")

set(
    MEISO_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/MeisoGlass"
    CACHE STRING "Install location for MeisoGlass CMake package files"
)

install(
    TARGETS
        meiso_core_resources
        meiso_core_session
        meiso_core
    EXPORT MeisoCoreTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Core
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Core
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Core
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
    EXPORT MeisoCoreTargets
    FILE MeisoCoreTargets.cmake
    NAMESPACE Meiso::
    DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
    COMPONENT Core
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/core.h"
        "${MEISO_GENERATED_INCLUDE_DIR}/meiso/version.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso"
    COMPONENT Core
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/core/action.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/core/blob.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/core/descriptor_set.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/core/resource.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/core/session.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/core/state.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/core/stream.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/core"
    COMPONENT Core
)

install(
    TARGETS meiso_runtime
    EXPORT MeisoRuntimeTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
    EXPORT MeisoRuntimeTargets
    FILE MeisoRuntimeTargets.cmake
    NAMESPACE Meiso::
    DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
    COMPONENT Runtime
)
install(
    FILES "${PROJECT_SOURCE_DIR}/include/meiso/runtime.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso"
    COMPONENT Runtime
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/runtime/runtime.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/runtime"
    COMPONENT Runtime
)

if(MEISO_BUILD_LEGACY_COMPOSITION)
    install(TARGETS meiso_runtime_scheduler EXPORT MeisoRuntimeTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/include/meiso/runtime/scheduler.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/runtime" COMPONENT Runtime)
endif()

install(
    TARGETS
        meiso_wifi_telemetry_profile
        meiso_device_telemetry_profile
        meiso_composition_scene_schema
        meiso_composition_profile
        meiso_composition_media_transport
        meiso_display_ingress_profile
        meiso_display_ingress_media_transport
    EXPORT MeisoProfilesTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Profiles
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Profiles
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Profiles
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
    EXPORT MeisoProfilesTargets
    FILE MeisoProfilesTargets.cmake
    NAMESPACE Meiso::
    DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
    COMPONENT Profiles
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/wifi_telemetry.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/device_telemetry.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/profiles"
    COMPONENT Profiles
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/composition/scene.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/composition/profile.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/composition/media_transport.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/profiles/composition"
    COMPONENT Profiles
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/profile.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/overlay.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/scene.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/camera.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/application.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/planes.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/tracking.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/projection.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/profiles/display_ingress/media_transport.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/profiles/display_ingress"
    COMPONENT Profiles
)

install(
    TARGETS meiso_display_ingress_host_fixture_input
    EXPORT MeisoDisplayIngressHostFixtureInputTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT DisplayIngressHostFixtureInput
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        COMPONENT DisplayIngressHostFixtureInput
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT DisplayIngressHostFixtureInput
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
    EXPORT MeisoDisplayIngressHostFixtureInputTargets
    FILE MeisoDisplayIngressHostFixtureInputTargets.cmake
    NAMESPACE Meiso::
    DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
    COMPONENT DisplayIngressHostFixtureInput
)
install(
    FILES
        "${PROJECT_SOURCE_DIR}/include/meiso/host/display_ingress_fixture_input.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/host"
    COMPONENT DisplayIngressHostFixtureInput
)

if(MEISO_BUILD_LEGACY_COMPOSITION)
    install(
        TARGETS meiso_composition_edge
        EXPORT MeisoClientTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Client
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Client
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Client
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        EXPORT MeisoClientTargets
        FILE MeisoClientTargets.cmake
        NAMESPACE Meiso::
        DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
        COMPONENT Client
    )
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/include/meiso/host/edge.h"
            "${PROJECT_SOURCE_DIR}/include/meiso/host/composition.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/host"
        COMPONENT Client
    )

    set(
        meiso_composition_install_targets
        meiso_composition_scene_store
        meiso_composition_coordinator
        meiso_composition_reference_renderer
        meiso_composition_cpu_raster_renderer
    )
    install(
        TARGETS ${meiso_composition_install_targets}
        EXPORT MeisoCompositionTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Composition
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Composition
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Composition
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        EXPORT MeisoCompositionTargets
        FILE MeisoCompositionTargets.cmake
        NAMESPACE Meiso::
        DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
        COMPONENT Composition
    )
    set(
        meiso_composition_headers
        "${PROJECT_SOURCE_DIR}/include/meiso/composition/scene_store.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/composition/coordinator.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/composition/reference_renderer.h"
        "${PROJECT_SOURCE_DIR}/include/meiso/composition/cpu_raster_renderer.h"
    )
    install(
        FILES ${meiso_composition_headers}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/composition"
        COMPONENT Composition
    )

    install(
        TARGETS
            meiso_composition_edge_simulator
            meiso_composition_edge_trace
        EXPORT MeisoSimulationTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Simulation
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Simulation
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Simulation
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        EXPORT MeisoSimulationTargets
        FILE MeisoSimulationTargets.cmake
        NAMESPACE Meiso::
        DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
        COMPONENT Simulation
    )
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/include/meiso/simulation/composition_edge.h"
            "${PROJECT_SOURCE_DIR}/include/meiso/simulation/composition_trace.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/simulation"
        COMPONENT Simulation
    )
endif()

if(TARGET meiso_composition_control_session)
    install(
        TARGETS meiso_composition_control_session
        EXPORT MeisoControlTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Control
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Control
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Control
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        EXPORT MeisoControlTargets
        FILE MeisoControlTargets.cmake
        NAMESPACE Meiso::
        DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
        COMPONENT Control
    )
    install(
        FILES "${PROJECT_SOURCE_DIR}/include/meiso/composition/control_session.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/composition"
        COMPONENT Control
    )
endif()

if(TARGET meiso_posix_tcp_adapter)
    install(
        TARGETS
            meiso_posix_tcp_adapter
            meiso_composition_media_posix_tcp_adapter
            meiso_display_camera_channel
            meiso_display_ingress_media_posix_udp_adapter
        EXPORT MeisoPosixTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Posix
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Posix
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Posix
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        EXPORT MeisoPosixTargets
        FILE MeisoPosixTargets.cmake
        NAMESPACE Meiso::
        DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
        COMPONENT Posix
    )
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/include/meiso/adapters/posix/tcp.h"
            "${PROJECT_SOURCE_DIR}/include/meiso/adapters/posix/composition_media_tcp.h"
            "${PROJECT_SOURCE_DIR}/include/meiso/adapters/posix/display_ingress_media_udp.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/adapters/posix"
        COMPONENT Posix
    )
endif()

if(TARGET meiso_composition_host_runtime)
    install(
        TARGETS
            meiso_composition_host_runtime
            meiso_composition_remote_edge
        EXPORT MeisoHostTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Host
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Host
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Host
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        EXPORT MeisoHostTargets
        FILE MeisoHostTargets.cmake
        NAMESPACE Meiso::
        DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
        COMPONENT Host
    )
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/include/meiso/host/composition_runtime.h"
            "${PROJECT_SOURCE_DIR}/include/meiso/host/remote_edge.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/host"
        COMPONENT Host
    )
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/MeisoGlassConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MeisoGlassConfig.cmake"
    INSTALL_DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
    NO_SET_AND_CHECK_MACRO
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MeisoGlassConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/MeisoGlassConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/MeisoGlassConfigVersion.cmake"
    DESTINATION "${MEISO_INSTALL_CMAKEDIR}"
    COMPONENT Core
)

unset(meiso_composition_headers)
unset(meiso_composition_install_targets)

if(TARGET meiso_display_camera_channel)
    install(FILES include/meiso/integrations/display_camera_channel.h
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/meiso/integrations" COMPONENT Posix)
endif()
