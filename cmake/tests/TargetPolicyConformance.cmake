# Aggregate conformance and compiler-variant target-policy lanes.

# The aggregate runner intentionally recompiles the same Core contract sources
# for portable/target execution. CI can select either label `core` or
# `conformance` to avoid running both copies in the same lane.
function(meiso_add_conformance_suite suite_name source_file)
    set(target_name meiso_conformance_${suite_name}_suite)
    add_library(${target_name} OBJECT ${source_file})
    target_link_libraries(${target_name} PRIVATE meiso_core_resources)
    target_include_directories(${target_name} PRIVATE src/core tests)
    target_compile_definitions(
        ${target_name}
        PRIVATE main=meiso_conformance_run_${suite_name}
    )
    meiso_enable_warnings(${target_name})
    meiso_set_target_folder(${target_name} "Meiso/Tests/conformance")
endfunction()

meiso_add_conformance_suite(resource tests/test_resource.c)
meiso_add_conformance_suite(state tests/test_state.c)
meiso_add_conformance_suite(stream tests/test_stream.c)
meiso_add_conformance_suite(action tests/test_action.c)
meiso_add_conformance_suite(blob tests/test_blob.c)
meiso_add_conformance_suite(descriptor_set tests/test_descriptor_set.c)
meiso_add_conformance_suite(four_kind tests/test_four_kind.c)

add_executable(
    meiso_conformance
    targets/conformance/main.c
    $<TARGET_OBJECTS:meiso_conformance_resource_suite>
    $<TARGET_OBJECTS:meiso_conformance_state_suite>
    $<TARGET_OBJECTS:meiso_conformance_stream_suite>
    $<TARGET_OBJECTS:meiso_conformance_action_suite>
    $<TARGET_OBJECTS:meiso_conformance_blob_suite>
    $<TARGET_OBJECTS:meiso_conformance_descriptor_set_suite>
    $<TARGET_OBJECTS:meiso_conformance_four_kind_suite>
)
target_link_libraries(meiso_conformance PRIVATE meiso_core_resources)
meiso_enable_warnings(meiso_conformance)
meiso_set_target_folder(meiso_conformance "Meiso/Tests/conformance")
meiso_register_target_test(meiso_conformance "conformance")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(meiso_resource_short_enum_test tests/test_resource.c)
    target_link_libraries(
        meiso_resource_short_enum_test
        PRIVATE meiso_core_resources
    )
    target_include_directories(
        meiso_resource_short_enum_test
        PRIVATE src/core
    )
    target_compile_options(meiso_resource_short_enum_test PRIVATE -fshort-enums)
    meiso_enable_warnings(meiso_resource_short_enum_test)
    meiso_set_target_folder(
        meiso_resource_short_enum_test
        "Meiso/Tests/compiler-variant"
    )
    meiso_register_target_test(
        meiso_resource_short_enum_test "compiler-variant"
    )
endif()
