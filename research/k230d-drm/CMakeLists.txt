# Explicit research build only; deliberately absent from install/export.
# Preserve the historical wrapper executable locations.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

# Portable policy/state-machine core only. A native libdrm translator and
# one-shot target runner remain a separate, explicitly staged K230D unit.
add_library(
    meiso_k230d_drm_scanout
    adapters/drm_scanout_adapter.c
    adapters/drm_scanout_admission.c
    adapters/drm_scanout_cleanup.c
    adapters/drm_scanout_convert.c
    adapters/drm_scanout_lifecycle.c
    adapters/drm_scanout_ops.c
    adapters/drm_scanout_sha256.c
    adapters/drm_scanout_single_plane_test_only.c
)
target_include_directories(meiso_k230d_drm_scanout PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(meiso_k230d_drm_scanout PUBLIC meiso_core_resources)
meiso_enable_warnings(meiso_k230d_drm_scanout)
meiso_set_target_folder(
    meiso_k230d_drm_scanout "Meiso/Research/DRM"
)
add_library(
    Meiso::K230dDrmScanout ALIAS meiso_k230d_drm_scanout
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


if(MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER OR
   MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER OR
   MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    if(NOT TARGET meiso_k230d_linux_target_contract OR
       NOT MEISO_BUILD_K230D_LINUX_APPS OR
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
        adapters/drm_scanout_libdrm_topology.c
    )
    target_include_directories(
        meiso_k230d_drm_libdrm_topology
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
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
        "Meiso/Research/DRM/Native"
    )

endif()

if(MEISO_BUILD_K230D_DRM_TOPOLOGY_ADMISSION_RUNNER)

    add_executable(
        meiso_k230d_drm_topology_admission_once
        drm_topology_admission_once.c
    )
    target_include_directories(
        meiso_k230d_drm_topology_admission_once
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
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
        "Meiso/Research/DRM/Evidence"
    )
endif()

if(MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER OR
   MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    add_library(
        meiso_k230d_drm_libdrm_single_plane_test_only STATIC
        adapters/drm_scanout_libdrm_single_plane_test_only.c
    )
    target_include_directories(
        meiso_k230d_drm_libdrm_single_plane_test_only
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
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
        "Meiso/Research/DRM/Native"
    )
endif()

if(MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER)
    add_executable(
        meiso_k230d_drm_single_plane_test_only_once
        drm_single_plane_test_only_once.c
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_once
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
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
        "Meiso/Research/DRM/Evidence"
    )
endif()

if(MEISO_BUILD_K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR)
    add_executable(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        drm_single_plane_test_only_once.c
        drm_single_plane_test_only_supervisor.c
        drm_single_plane_test_only_supervisor_linux.c
    )
    target_compile_definitions(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        PRIVATE MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_ONCE_NO_MAIN=1
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_supervised_once
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
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
        "Meiso/Research/DRM/Evidence"
    )
endif()


if(BUILD_TESTING)
    include(tests.cmake)
endif()
