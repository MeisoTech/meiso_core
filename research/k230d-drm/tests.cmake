if(TARGET meiso_k230d_drm_scanout)
    meiso_add_c_test(
        meiso_k230d_drm_scanout_test
        tests/test_k230d_drm_scanout.c
        adapter
        meiso_k230d_drm_scanout
    )
    target_include_directories(
        meiso_k230d_drm_scanout_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
    )

    meiso_add_c_test(
        meiso_k230d_drm_single_plane_test_only_test
        tests/test_k230d_drm_single_plane_test_only.c
        adapter
        meiso_k230d_drm_scanout
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
    )

    add_executable(
        meiso_k230d_drm_topology_admission_once_test
        tests/test_k230d_drm_topology_admission_once.c
        drm_topology_admission_once.c
    )
    target_compile_definitions(
        meiso_k230d_drm_topology_admission_once_test
        PRIVATE MEISO_K230D_DRM_TOPOLOGY_ADMISSION_ONCE_NO_MAIN=1
    )
    target_include_directories(
        meiso_k230d_drm_topology_admission_once_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
    )
    target_link_libraries(
        meiso_k230d_drm_topology_admission_once_test
        PRIVATE meiso_k230d_drm_scanout
    )
    meiso_enable_warnings(meiso_k230d_drm_topology_admission_once_test)
    meiso_set_target_folder(
        meiso_k230d_drm_topology_admission_once_test
        "Meiso/Tests/target-policy"
    )
    meiso_register_target_test(
        meiso_k230d_drm_topology_admission_once_test "target-policy"
    )

    add_executable(
        meiso_k230d_drm_single_plane_test_only_once_test
        tests/test_k230d_drm_single_plane_test_only_once.c
        drm_single_plane_test_only_once.c
    )
    target_compile_definitions(
        meiso_k230d_drm_single_plane_test_only_once_test
        PRIVATE MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_ONCE_NO_MAIN=1
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_once_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
    )
    target_link_libraries(
        meiso_k230d_drm_single_plane_test_only_once_test
        PRIVATE meiso_k230d_drm_scanout
    )
    meiso_enable_warnings(meiso_k230d_drm_single_plane_test_only_once_test)
    meiso_set_target_folder(
        meiso_k230d_drm_single_plane_test_only_once_test
        "Meiso/Tests/target-policy"
    )
    meiso_register_target_test(
        meiso_k230d_drm_single_plane_test_only_once_test "target-policy"
    )

    add_executable(
        meiso_k230d_drm_single_plane_test_only_supervisor_test
        tests/test_k230d_drm_single_plane_test_only_supervisor.c
        drm_single_plane_test_only_supervisor.c
    )
    target_include_directories(
        meiso_k230d_drm_single_plane_test_only_supervisor_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
    )
    target_link_libraries(
        meiso_k230d_drm_single_plane_test_only_supervisor_test
        PRIVATE meiso_k230d_drm_scanout
    )
    meiso_enable_warnings(
        meiso_k230d_drm_single_plane_test_only_supervisor_test
    )
    meiso_set_target_folder(
        meiso_k230d_drm_single_plane_test_only_supervisor_test
        "Meiso/Tests/target-policy"
    )
    meiso_register_target_test(
        meiso_k230d_drm_single_plane_test_only_supervisor_test
        "target-policy"
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
        add_executable(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
            tests/test_k230d_drm_single_plane_test_only_supervisor_linux.c
            drm_single_plane_test_only_supervisor.c
        )
        target_include_directories(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
        )
        target_link_libraries(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
            PRIVATE meiso_k230d_drm_scanout rt
        )
        meiso_enable_warnings(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
        )
        meiso_set_target_folder(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
            "Meiso/Tests/target-policy"
        )
        meiso_register_target_test(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
            "target-policy"
        )
        set_tests_properties(
            meiso_k230d_drm_single_plane_test_only_supervisor_linux_test
            PROPERTIES TIMEOUT 30
        )
    endif()
endif()
