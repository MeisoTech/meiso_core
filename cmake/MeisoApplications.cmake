# Executables and evidence runners. Reusable SDK behavior belongs in a library;
# process entry points should remain thin and depend on Integration targets.

option(
    MEISO_BUILD_K230D_LINUX_APPS
    "Expose the K230D Linux cross-application target set"
    OFF
)
option(
    MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER
    "Build the explicit K230D read-only libdrm topology admission runner"
    OFF
)
option(
    MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER
    "Build the explicit K230D libdrm single-plane TEST_ONLY-only runner"
    OFF
)
option(
    MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR
    "Build the explicit K230D supervised single-plane TEST_ONLY-only runner"
    OFF
)

set(
    MEISO_K230D_LIBDRM_INCLUDE_DIR
    ""
    CACHE PATH "Explicit target libdrm source/include root for K230D DRM runners"
)
set(
    MEISO_K230D_LIBDRM_LIBRARY
    ""
    CACHE FILEPATH "Explicit target libdrm shared library for K230D DRM runners"
)

if(MEISO_BUILD_K230D_LINUX_APPS)
    if(NOT MEISO_K230_LINUX_TARGET)
        message(
            FATAL_ERROR
            "K230D applications require the K230D Linux toolchain contract"
        )
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
        message(
            FATAL_ERROR
            "K230D applications require a Linux/riscv64 target platform"
        )
    endif()

    add_library(meiso_k230d_linux_target_contract INTERFACE)
    target_compile_definitions(
        meiso_k230d_linux_target_contract
        INTERFACE MEISO_K230_LINUX_TARGET=1
    )
    meiso_set_target_folder(
        meiso_k230d_linux_target_contract
        "Meiso/Targets/K230D Linux"
    )
endif()

if(MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER OR
   MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER OR
   MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    if(NOT MEISO_BUILD_K230D_LINUX_APPS OR
       NOT MEISO_K230_LINUX_TARGET OR
       NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
        message(
            FATAL_ERROR
            "K230D DRM native runners require the K230D Linux/riscv64 application contract"
        )
    endif()
    if(NOT IS_ABSOLUTE "${MEISO_K230D_LIBDRM_INCLUDE_DIR}" OR
       NOT EXISTS "${MEISO_K230D_LIBDRM_INCLUDE_DIR}/xf86drm.h" OR
       NOT EXISTS "${MEISO_K230D_LIBDRM_INCLUDE_DIR}/xf86drmMode.h" OR
       NOT EXISTS "${MEISO_K230D_LIBDRM_INCLUDE_DIR}/include/drm/drm.h" OR
       NOT EXISTS "${MEISO_K230D_LIBDRM_INCLUDE_DIR}/include/drm/drm_mode.h" OR
       NOT EXISTS "${MEISO_K230D_LIBDRM_INCLUDE_DIR}/include/drm/drm_fourcc.h")
        message(
            FATAL_ERROR
            "MEISO_K230D_LIBDRM_INCLUDE_DIR must be an absolute libdrm source/include root"
        )
    endif()
    if(NOT IS_ABSOLUTE "${MEISO_K230D_LIBDRM_LIBRARY}" OR
       NOT EXISTS "${MEISO_K230D_LIBDRM_LIBRARY}" OR
       IS_SYMLINK "${MEISO_K230D_LIBDRM_LIBRARY}" OR
       IS_DIRECTORY "${MEISO_K230D_LIBDRM_LIBRARY}")
        message(
            FATAL_ERROR
            "MEISO_K230D_LIBDRM_LIBRARY must be an absolute regular target library"
        )
    endif()

    add_library(meiso_k230d_target_libdrm SHARED IMPORTED)
    set_target_properties(
        meiso_k230d_target_libdrm
        PROPERTIES IMPORTED_LOCATION "${MEISO_K230D_LIBDRM_LIBRARY}"
    )
    target_include_directories(
        meiso_k230d_target_libdrm
        SYSTEM INTERFACE
            "${MEISO_K230D_LIBDRM_INCLUDE_DIR}"
            "${MEISO_K230D_LIBDRM_INCLUDE_DIR}/include/drm"
    )

    add_library(
        meiso_k230d_drm_libdrm_topology STATIC
        targets/k230d-linux/adapters/drm_scanout_libdrm_topology.c
    )
    target_include_directories(
        meiso_k230d_drm_libdrm_topology
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_libdrm_topology
        PRIVATE
            meiso_k230d_drm_scanout
            meiso_k230d_linux_target_contract
            meiso_k230d_target_libdrm
    )
    meiso_enable_warnings(meiso_k230d_drm_libdrm_topology)
    meiso_set_target_folder(
        meiso_k230d_drm_libdrm_topology
        "Meiso/Adapters/Linux/Native"
    )

endif()

if(MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER)

    add_executable(
        meiso_k230d_drm_topology_admission_once
        targets/k230d-linux/drm_topology_admission_once.c
    )
    target_include_directories(
        meiso_k230d_drm_topology_admission_once
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_topology_admission_once
        PRIVATE
            meiso_k230d_drm_libdrm_topology
            meiso_k230d_drm_scanout
            meiso_k230d_linux_target_contract
    )
    set_target_properties(
        meiso_k230d_drm_topology_admission_once
        PROPERTIES SKIP_BUILD_RPATH TRUE
    )
    meiso_enable_warnings(meiso_k230d_drm_topology_admission_once)
    meiso_set_target_folder(
        meiso_k230d_drm_topology_admission_once
        "Meiso/Applications/Edge/Evidence"
    )
endif()

if(MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER OR
   MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    add_library(
        meiso_k230d_drm_libdrm_single_plane_test_only STATIC
        targets/k230d-linux/adapters/drm_scanout_libdrm_single_plane_test_only.c
    )
    target_include_directories(
        meiso_k230d_drm_libdrm_single_plane_test_only
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_libdrm_single_plane_test_only
        PRIVATE
            meiso_k230d_drm_libdrm_topology
            meiso_k230d_drm_scanout
            meiso_k230d_linux_target_contract
            meiso_k230d_target_libdrm
    )
    meiso_enable_warnings(meiso_k230d_drm_libdrm_single_plane_test_only)
    meiso_set_target_folder(
        meiso_k230d_drm_libdrm_single_plane_test_only
        "Meiso/Adapters/Linux/Native"
    )
endif()

if(MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER)
    add_executable(
        meiso_k230d_drm_single_plane_test_only_once
        targets/k230d-linux/drm_single_plane_test_only_once.c
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_once
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_single_plane_test_only_once
        PRIVATE
            meiso_k230d_drm_libdrm_single_plane_test_only
            meiso_k230d_drm_scanout
            meiso_k230d_linux_target_contract
    )
    set_target_properties(
        meiso_k230d_drm_single_plane_test_only_once
        PROPERTIES SKIP_BUILD_RPATH TRUE
    )
    meiso_enable_warnings(meiso_k230d_drm_single_plane_test_only_once)
    meiso_set_target_folder(
        meiso_k230d_drm_single_plane_test_only_once
        "Meiso/Applications/Edge/Evidence"
    )
endif()

if(MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    add_executable(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        targets/k230d-linux/drm_single_plane_test_only_once.c
        targets/k230d-linux/drm_single_plane_test_only_supervisor.c
        targets/k230d-linux/drm_single_plane_test_only_supervisor_linux.c
    )
    target_compile_definitions(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        PRIVATE MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_ONCE_NO_MAIN=1
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        PRIVATE targets/k230d-linux/adapters
    )
    target_link_libraries(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        PRIVATE
            meiso_k230d_drm_libdrm_single_plane_test_only
            meiso_k230d_drm_scanout
            meiso_k230d_linux_target_contract
    )
    set_target_properties(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        PROPERTIES SKIP_BUILD_RPATH TRUE
    )
    meiso_enable_warnings(
        meiso_k230d_drm_single_plane_test_only_supervised_once
    )
    meiso_set_target_folder(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        "Meiso/Applications/Edge/Evidence"
    )
endif()

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

if(MEISO_BUILD_K230D_LINUX_APPS AND
   TARGET meiso_k230d_composition_edge_runtime)
    find_package(Threads REQUIRED)
    add_executable(
        meiso_k230d_composition_edge_service
        targets/k230d-linux/composition_edge_service.c
        targets/k230d-linux/composition_edge_service_config.c
        targets/k230d-linux/composition_edge_service_lifecycle.c
        targets/k230d-linux/composition_edge_service_run.c
        targets/k230d-linux/composition_edge_service_run_loop.c
    )
    target_link_libraries(
        meiso_k230d_composition_edge_service
        PRIVATE meiso_k230d_composition_edge_runtime Threads::Threads
    )
    target_link_libraries(
        meiso_k230d_composition_edge_service
        PRIVATE meiso_k230d_linux_target_contract
    )
    # FindThreads cannot link-probe while the cross toolchain deliberately
    # uses STATIC_LIBRARY try-compiles. Keep the exact service requirement on
    # its owning target instead of relying on a false libc-pthread probe.
    target_compile_options(
        meiso_k230d_composition_edge_service
        PRIVATE -pthread
    )
    target_link_options(
        meiso_k230d_composition_edge_service
        PRIVATE -pthread
    )
    meiso_enable_warnings(meiso_k230d_composition_edge_service)
    meiso_set_target_folder(
        meiso_k230d_composition_edge_service
        "Meiso/Applications/Edge"
    )
endif()

if(UNIX)
    set(MEISO_DISPLAY_INGRESS_SERVICE_SOURCES
        devtools/display_ingress_simulated_service.c
        devtools/display_ingress_simulated_service_bootstrap.c
        devtools/display_ingress_simulated_service_control.c
        devtools/display_ingress_simulated_service_edge.c
        devtools/display_ingress_simulated_service_frame_input.c
        devtools/display_ingress_simulated_service_host.c
        devtools/display_ingress_simulated_service_loop.c
        devtools/display_ingress_simulated_service_receipt.c
    )
    add_executable(
        meiso_display_ingress_simulated_service
        ${MEISO_DISPLAY_INGRESS_SERVICE_SOURCES}
        devtools/display_ingress_simulated_pair_runner.c
    )
    target_link_libraries(
        meiso_display_ingress_simulated_service
        PRIVATE
            meiso_posix_tcp_adapter
            meiso_display_ingress_media_posix_udp_adapter
            meiso_display_ingress_host_fixture_input
    )
    meiso_enable_warnings(meiso_display_ingress_simulated_service)
    meiso_set_target_folder(
        meiso_display_ingress_simulated_service
        "Meiso/Applications/Lab"
    )

    set(MEISO_VIDEO_SERVICE_SOURCES
        devtools/display_ingress_video_service.c
        devtools/display_ingress_simulated_service_bootstrap.c
        devtools/display_ingress_simulated_service_control.c
        devtools/display_ingress_simulated_service_edge.c
        devtools/display_ingress_simulated_service_host.c
        devtools/display_ingress_simulated_service_loop.c)
    add_executable(meiso_display_ingress_video_host ${MEISO_VIDEO_SERVICE_SOURCES})
    target_compile_definitions(meiso_display_ingress_video_host PRIVATE MEISO_DISPLAY_INGRESS_VIDEO_SERVICE=1)
    target_link_libraries(meiso_display_ingress_video_host PRIVATE meiso_posix_tcp_adapter
        meiso_display_ingress_media_posix_udp_adapter meiso_display_ingress_host_fixture_input)
    meiso_enable_warnings(meiso_display_ingress_video_host)
    option(MEISO_BUILD_DESKTOP_EDGE "Build portable software-decoded Display Ingress Edge" OFF)
    if(MEISO_BUILD_DESKTOP_EDGE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(MEISO_DESKTOP_AV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
        add_executable(meiso_desktop_edge ${MEISO_VIDEO_SERVICE_SOURCES}
            devtools/display_ingress_video_overlay.c
            targets/k230d-linux/display_ingress_overlay_raster.c
            targets/k230d-linux/display_ingress_scene_raster.c
            adapters/desktop/display_ingress_video_backend.c)
        target_compile_definitions(meiso_desktop_edge PRIVATE
            MEISO_DISPLAY_INGRESS_VIDEO_SERVICE=1 MEISO_DISPLAY_INGRESS_VIDEO_EDGE=1
            MEISO_DISPLAY_INGRESS_DESKTOP_EDGE=1 MEISO_DISPLAY_INGRESS_DESKTOP_VIDEO=1)
        target_link_libraries(meiso_desktop_edge PRIVATE meiso_posix_tcp_adapter meiso_display_camera_channel
            meiso_display_ingress_media_posix_udp_adapter meiso_display_ingress_host_fixture_input
            PkgConfig::MEISO_DESKTOP_AV)
        meiso_enable_warnings(meiso_desktop_edge)
        if(BUILD_TESTING)
            add_executable(meiso_desktop_video_backend_test tests/test_desktop_video_backend.c
                adapters/desktop/display_ingress_video_backend.c
                targets/k230d-linux/display_ingress_overlay_raster.c
                targets/k230d-linux/display_ingress_scene_raster.c)
            target_compile_definitions(meiso_desktop_video_backend_test PRIVATE MEISO_DISPLAY_INGRESS_DESKTOP_VIDEO=1)
            target_link_libraries(meiso_desktop_video_backend_test PRIVATE PkgConfig::MEISO_DESKTOP_AV meiso_display_ingress_profile)
            meiso_enable_warnings(meiso_desktop_video_backend_test)
            add_test(NAME meiso_desktop_video_backend_test COMMAND meiso_desktop_video_backend_test)
        endif()

        add_executable(meiso_desktop_host ${MEISO_VIDEO_SERVICE_SOURCES})
        target_compile_definitions(meiso_desktop_host PRIVATE
            MEISO_DISPLAY_INGRESS_VIDEO_SERVICE=1 MEISO_DISPLAY_INGRESS_DESKTOP_VIDEO=1)
        target_link_libraries(meiso_desktop_host PRIVATE meiso_posix_tcp_adapter
            meiso_display_ingress_media_posix_udp_adapter meiso_display_ingress_host_fixture_input)
        meiso_enable_warnings(meiso_desktop_host)
        install(PROGRAMS devtools/desktop/launch.py DESTINATION "${CMAKE_INSTALL_BINDIR}"
            RENAME meiso-desktop-launch COMPONENT DesktopRuntime)
        install(PROGRAMS devtools/desktop/pair.py DESTINATION "${CMAKE_INSTALL_BINDIR}"
            RENAME meiso-desktop-pair COMPONENT DesktopRuntime)
        install(TARGETS meiso_desktop_edge meiso_desktop_host RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT DesktopRuntime)
    endif()

    option(MEISO_BUILD_K230D_VIDEO_SERVICE "Build native K230D MVX/DRM video service" OFF)
    if(MEISO_BUILD_K230D_VIDEO_SERVICE)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "K230D video service requires Linux")
        endif()
        find_path(MEISO_DISPLAY_DRM_INCLUDE_DIR xf86drm.h REQUIRED)
        find_path(MEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR drm.h PATH_SUFFIXES libdrm REQUIRED)
        find_library(MEISO_DISPLAY_DRM_LIBRARY drm REQUIRED)
        add_executable(meiso_display_ingress_video_edge ${MEISO_VIDEO_SERVICE_SOURCES}
            devtools/display_ingress_video_overlay.c
            targets/k230d-linux/display_ingress_overlay_raster.c
            targets/k230d-linux/display_ingress_scene_raster.c
            targets/k230d-linux/display_ingress_video_backend.c)
        target_compile_definitions(meiso_display_ingress_video_edge PRIVATE
            MEISO_DISPLAY_INGRESS_VIDEO_SERVICE=1 MEISO_DISPLAY_INGRESS_VIDEO_EDGE=1)
        target_include_directories(meiso_display_ingress_video_edge SYSTEM PRIVATE
            "${MEISO_DISPLAY_DRM_INCLUDE_DIR}" "${MEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR}")
        target_link_libraries(meiso_display_ingress_video_edge PRIVATE meiso_posix_tcp_adapter
            meiso_display_camera_channel meiso_camera_v4l2_adapter
            meiso_display_ingress_media_posix_udp_adapter meiso_display_ingress_host_fixture_input
            "${MEISO_DISPLAY_DRM_LIBRARY}")
        set_target_properties(meiso_display_ingress_video_edge PROPERTIES SKIP_BUILD_RPATH TRUE)
        meiso_enable_warnings(meiso_display_ingress_video_edge)
        add_executable(meiso_display_ingress_edge_launch
            targets/k230d-linux/display_ingress_edge_launch.c)
        meiso_enable_warnings(meiso_display_ingress_edge_launch)
        install(TARGETS meiso_display_ingress_video_edge meiso_display_ingress_edge_launch
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT K230dVideoRuntime)
        install(PROGRAMS targets/k230d-linux/meiso-edge-reconnect
            DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT K230dVideoRuntime)
        install(PROGRAMS targets/k230d-linux/S90meiso-edge
            DESTINATION "${CMAKE_INSTALL_DATADIR}/meiso/init.d" COMPONENT K230dVideoRuntime)
        install(FILES targets/k230d-linux/edge.conf.example
            DESTINATION "${CMAKE_INSTALL_DATADIR}/meiso" COMPONENT K230dVideoRuntime)
        set(MEISO_K230D_OVERLAY_FONT_FILE "" CACHE FILEPATH "Optional generated CJK16 glyph bank")
        if(MEISO_K230D_OVERLAY_FONT_FILE)
            if(NOT EXISTS "${MEISO_K230D_OVERLAY_FONT_FILE}")
                message(FATAL_ERROR "CJK16 glyph bank does not exist")
            endif()
            install(FILES "${MEISO_K230D_OVERLAY_FONT_FILE}"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/meiso" RENAME overlay-cjk16.bin
                COMPONENT K230dVideoRuntime)
            install(FILES targets/k230d-linux/overlay-font-NOTICE.txt
                targets/k230d-linux/overlay-font-OFL.txt
                DESTINATION "${CMAKE_INSTALL_DATADIR}/meiso" COMPONENT K230dVideoRuntime)
        endif()
    endif()

    option(MEISO_BUILD_DISPLAY_INGRESS_DRM_SERVICE "Build the controlled Linux single-frame DRM service" OFF)
    if(MEISO_BUILD_DISPLAY_INGRESS_DRM_SERVICE)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "Display Ingress DRM service requires Linux")
        endif()
        find_path(MEISO_DISPLAY_DRM_INCLUDE_DIR xf86drm.h REQUIRED)
        find_path(MEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR drm.h PATH_SUFFIXES libdrm REQUIRED)
        find_library(MEISO_DISPLAY_DRM_LIBRARY drm REQUIRED)
        add_executable(meiso_display_ingress_drm_service
            ${MEISO_DISPLAY_INGRESS_SERVICE_SOURCES}
            devtools/display_ingress_service_drm.c)
        target_compile_definitions(meiso_display_ingress_drm_service PRIVATE MEISO_DISPLAY_INGRESS_DRM_SERVICE=1)
        target_include_directories(meiso_display_ingress_drm_service SYSTEM PRIVATE
            "${MEISO_DISPLAY_DRM_INCLUDE_DIR}" "${MEISO_DISPLAY_DRM_UAPI_INCLUDE_DIR}")
        target_link_libraries(meiso_display_ingress_drm_service PRIVATE
            meiso_posix_tcp_adapter meiso_display_ingress_media_posix_udp_adapter
            meiso_display_ingress_host_fixture_input "${MEISO_DISPLAY_DRM_LIBRARY}")
        set_target_properties(meiso_display_ingress_drm_service PROPERTIES SKIP_BUILD_RPATH TRUE)
        meiso_enable_warnings(meiso_display_ingress_drm_service)
        meiso_set_target_folder(meiso_display_ingress_drm_service "Meiso/Applications/Lab")
    endif()

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
    if(MEISO_BUILD_K230D_LINUX_APPS)
        target_link_libraries(
            meiso_four_kind_lab
            PRIVATE meiso_k230d_linux_target_contract
        )
    endif()
    meiso_enable_warnings(meiso_four_kind_lab)
    meiso_set_target_folder(meiso_four_kind_lab "Meiso/Applications/Lab")
endif()

add_executable(
    meiso_composition_offscreen_runner
    targets/k230d-linux/composition_offscreen_runner.c
)
target_link_libraries(
    meiso_composition_offscreen_runner
    PRIVATE
        meiso_composition_coordinator
        meiso_composition_reference_renderer
)
if(MEISO_BUILD_K230D_LINUX_APPS)
    target_link_libraries(
        meiso_composition_offscreen_runner
        PRIVATE meiso_k230d_linux_target_contract
    )
endif()
meiso_enable_warnings(meiso_composition_offscreen_runner)
meiso_set_target_folder(
    meiso_composition_offscreen_runner
    "Meiso/Applications/Evidence"
)

if(MEISO_BUILD_K230D_LINUX_APPS)
    add_executable(
        meiso_k230d_camera_v4l2_capture_once
        targets/k230d-linux/camera_v4l2_capture_once.c
    )
    target_link_libraries(
        meiso_k230d_camera_v4l2_capture_once
        PRIVATE
            meiso_camera_v4l2_adapter
            meiso_k230d_linux_target_contract
    )
    meiso_enable_warnings(meiso_k230d_camera_v4l2_capture_once)
    meiso_set_target_folder(
        meiso_k230d_camera_v4l2_capture_once
        "Meiso/Applications/Edge"
    )

    add_executable(
        meiso_k230d_wifi_runtime_edge
        targets/k230d-linux/wifi_runtime_edge.c
        targets/k230d-linux/wifi_runtime_edge_config.c
        targets/k230d-linux/wifi_runtime_edge_bootstrap.c
        targets/k230d-linux/wifi_runtime_edge_truth.c
        targets/k230d-linux/wifi_runtime_edge_telemetry.c
        targets/k230d-linux/wifi_runtime_edge_pump.c
        targets/k230d-linux/wifi_runtime_edge_lifecycle.c
    )
    target_link_libraries(
        meiso_k230d_wifi_runtime_edge
        PRIVATE
            meiso_wifi_linux_adapter
            meiso_posix_tcp_adapter
            meiso_k230d_linux_target_contract
    )
    meiso_enable_warnings(meiso_k230d_wifi_runtime_edge)
    meiso_set_target_folder(
        meiso_k230d_wifi_runtime_edge
        "Meiso/Applications/Edge"
    )

    add_custom_target(
        meiso_k230d_linux_applications
        DEPENDS
            meiso_k230d_camera_v4l2_capture_once
            meiso_k230d_composition_edge_service
            meiso_composition_offscreen_runner
            meiso_four_kind_lab
            meiso_k230d_wifi_runtime_edge
    )
    meiso_set_target_folder(
        meiso_k230d_linux_applications
        "Meiso/Targets/K230D Linux"
    )
endif()

if(APPLE AND TARGET meiso_posix_tcp_adapter)
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
