# Portable Core, Session, and Runtime contract lanes.

meiso_add_c_test(meiso_sha256_test tests/test_sha256.c core meiso_core_resources)

meiso_add_c_test(
    meiso_resource_test tests/test_resource.c core meiso_core_resources
)
target_include_directories(meiso_resource_test PRIVATE src/core)

meiso_add_c_test(meiso_state_test tests/test_state.c core meiso_core_resources)
meiso_add_c_test(meiso_stream_test tests/test_stream.c core meiso_core_resources)
meiso_add_c_test(meiso_blob_test tests/test_blob.c core meiso_core_resources)
meiso_add_c_test(
    meiso_descriptor_set_test
    tests/test_descriptor_set.c
    core
    meiso_core_resources
)
meiso_add_c_test(meiso_action_test tests/test_action.c core meiso_core_resources)
meiso_add_c_test(
    meiso_four_kind_test tests/test_four_kind.c core meiso_core_resources
)
meiso_add_c_test(
    meiso_session_test tests/test_session.c session meiso_core_session
)
meiso_add_c_test(
    meiso_session_four_kind_test
    tests/test_session_four_kind.c
    session
    meiso_core
)

meiso_add_c_test(
    meiso_runtime_test tests/test_runtime.c runtime meiso_runtime
)
target_include_directories(meiso_runtime_test PRIVATE tests)
meiso_add_c_test(
    meiso_runtime_session_test
    tests/test_runtime_session.c
    runtime
    meiso_wifi_telemetry_profile
)
target_include_directories(meiso_runtime_session_test PRIVATE tests)
meiso_add_c_test(
    meiso_runtime_wire_test
    tests/test_runtime_wire.c
    runtime
    meiso_runtime
)
if(MEISO_BUILD_LEGACY_COMPOSITION)
    meiso_add_c_test(
        meiso_runtime_blob_wire_test
        tests/test_runtime_blob_wire.c
        runtime
        meiso_composition_scene_store
    )
    target_include_directories(
        meiso_runtime_blob_wire_test
        PRIVATE profiles/internal/composition_v2
    )
endif()
