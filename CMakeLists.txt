cmake_minimum_required(VERSION 3.21)

project(meiso_glass LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)

option(
    MEISO_REQUIRE_MSVC_X64_V143
    "Reject a Windows validation build unless it uses MSVC x64 with the v143 toolset"
    OFF
)
if(MEISO_REQUIRE_MSVC_X64_V143)
    if(NOT MSVC)
        message(FATAL_ERROR "The Windows validation preset requires MSVC.")
    endif()
    if(NOT CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64" OR
       NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The Windows validation preset requires the x64 compiler.")
    endif()
    if(NOT MSVC_TOOLSET_VERSION STREQUAL "143")
        message(FATAL_ERROR "The Windows validation preset requires the v143 toolset.")
    endif()
endif()

function(meiso_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(meiso_resource_core
    src/meiso_resource.c
    src/meiso_state.c
    src/meiso_stream.c
    src/meiso_blob.c
    src/meiso_descriptor_set.c
    src/meiso_action.c
)
target_include_directories(meiso_resource_core PUBLIC include)
meiso_enable_warnings(meiso_resource_core)

add_library(meiso_session_core src/meiso_session.c)
target_include_directories(meiso_session_core PUBLIC include)
target_link_libraries(meiso_session_core PUBLIC meiso_resource_core)
meiso_enable_warnings(meiso_session_core)

add_library(meiso_core INTERFACE)
target_link_libraries(
    meiso_core
    INTERFACE meiso_resource_core meiso_session_core
)

add_library(meiso_runtime src/meiso_runtime.c)
target_include_directories(meiso_runtime PUBLIC include)
target_link_libraries(meiso_runtime PUBLIC meiso_core)
meiso_enable_warnings(meiso_runtime)

add_library(meiso_runtime_scheduler src/meiso_runtime_scheduler.c)
target_include_directories(meiso_runtime_scheduler PUBLIC include)
target_link_libraries(meiso_runtime_scheduler PUBLIC meiso_runtime)
meiso_enable_warnings(meiso_runtime_scheduler)

add_library(
    meiso_wifi_telemetry_profile
    profiles/wifi-telemetry-v0/wifi_telemetry.c
)
target_include_directories(
    meiso_wifi_telemetry_profile
    PUBLIC profiles/wifi-telemetry-v0
)
target_link_libraries(meiso_wifi_telemetry_profile PUBLIC meiso_runtime)
meiso_enable_warnings(meiso_wifi_telemetry_profile)

add_library(
    meiso_composition_scene_profile
    profiles/composition-scene-lab-v0/composition_scene.c
    profiles/composition-scene-lab-v0/scene_store.c
    profiles/composition-scene-lab-v0/composition_coordinator.c
    profiles/composition-scene-lab-v0/composition_reference_renderer.c
)
target_include_directories(
    meiso_composition_scene_profile
    PUBLIC profiles/composition-scene-lab-v0
)
target_link_libraries(meiso_composition_scene_profile PUBLIC meiso_runtime)
meiso_enable_warnings(meiso_composition_scene_profile)

add_library(
    meiso_composition_session_v2_profile
    profiles/composition-scene-lab-v0/composition_session_v2.c
)
target_include_directories(
    meiso_composition_session_v2_profile
    PUBLIC profiles/composition-scene-lab-v0
)
target_link_libraries(
    meiso_composition_session_v2_profile
    PUBLIC meiso_composition_scene_profile meiso_wifi_telemetry_profile
)
meiso_enable_warnings(meiso_composition_session_v2_profile)

add_library(
    meiso_four_kind_lab_protocol
    profiles/four-kind-lab-v0/lab_protocol.c
)
target_include_directories(
    meiso_four_kind_lab_protocol
    PUBLIC profiles/four-kind-lab-v0
)
target_link_libraries(meiso_four_kind_lab_protocol PUBLIC meiso_resource_core)
meiso_enable_warnings(meiso_four_kind_lab_protocol)

if(UNIX)
    add_library(meiso_posix_tcp_adapter adapters/posix/meiso_posix_tcp.c)
    target_include_directories(
        meiso_posix_tcp_adapter
        PUBLIC adapters/posix
    )
    target_link_libraries(meiso_posix_tcp_adapter PUBLIC meiso_session_core)
    meiso_enable_warnings(meiso_posix_tcp_adapter)

    add_executable(
        meiso_four_kind_lab
        devtools/four_kind_lab/four_kind_lab.c
    )
    target_link_libraries(
        meiso_four_kind_lab
        PRIVATE meiso_posix_tcp_adapter meiso_four_kind_lab_protocol
    )
    meiso_enable_warnings(meiso_four_kind_lab)

    add_executable(
        meiso_four_kind_lab_internal_test
        devtools/four_kind_lab/four_kind_lab.c
    )
    target_compile_definitions(
        meiso_four_kind_lab_internal_test
        PRIVATE MEISO_LAB_INTERNAL_TEST=1
    )
    target_link_libraries(
        meiso_four_kind_lab_internal_test
        PRIVATE meiso_posix_tcp_adapter meiso_four_kind_lab_protocol
    )
    meiso_enable_warnings(meiso_four_kind_lab_internal_test)
endif()

add_executable(meiso_resource_test tests/test_resource.c)
target_link_libraries(meiso_resource_test PRIVATE meiso_resource_core)
target_include_directories(meiso_resource_test PRIVATE src)
meiso_enable_warnings(meiso_resource_test)

add_executable(meiso_state_test tests/test_state.c)
target_link_libraries(meiso_state_test PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_state_test)

add_executable(meiso_stream_test tests/test_stream.c)
target_link_libraries(meiso_stream_test PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_stream_test)

add_executable(meiso_blob_test tests/test_blob.c)
target_link_libraries(meiso_blob_test PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_blob_test)

add_executable(meiso_descriptor_set_test tests/test_descriptor_set.c)
target_link_libraries(meiso_descriptor_set_test PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_descriptor_set_test)

add_executable(meiso_action_test tests/test_action.c)
target_link_libraries(meiso_action_test PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_action_test)

add_executable(meiso_four_kind_test tests/test_four_kind.c)
target_link_libraries(meiso_four_kind_test PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_four_kind_test)

add_executable(meiso_session_test tests/test_session.c)
target_link_libraries(meiso_session_test PRIVATE meiso_session_core)
meiso_enable_warnings(meiso_session_test)

add_executable(meiso_session_four_kind_test tests/test_session_four_kind.c)
target_link_libraries(meiso_session_four_kind_test PRIVATE meiso_core)
meiso_enable_warnings(meiso_session_four_kind_test)

add_executable(meiso_runtime_test tests/test_runtime.c)
target_link_libraries(meiso_runtime_test PRIVATE meiso_runtime)
target_include_directories(meiso_runtime_test PRIVATE tests)
meiso_enable_warnings(meiso_runtime_test)

add_executable(meiso_runtime_session_test tests/test_runtime_session.c)
target_link_libraries(
    meiso_runtime_session_test
    PRIVATE meiso_wifi_telemetry_profile
)
target_include_directories(meiso_runtime_session_test PRIVATE tests)
meiso_enable_warnings(meiso_runtime_session_test)

add_executable(meiso_runtime_wire_test tests/test_runtime_wire.c)
target_link_libraries(meiso_runtime_wire_test PRIVATE meiso_runtime)
meiso_enable_warnings(meiso_runtime_wire_test)

add_executable(meiso_runtime_blob_wire_test tests/test_runtime_blob_wire.c)
target_link_libraries(
    meiso_runtime_blob_wire_test
    PRIVATE meiso_runtime meiso_composition_scene_profile
)
meiso_enable_warnings(meiso_runtime_blob_wire_test)

add_executable(
    meiso_wifi_telemetry_profile_test
    tests/test_wifi_telemetry_profile.c
)
target_link_libraries(
    meiso_wifi_telemetry_profile_test
    PRIVATE meiso_wifi_telemetry_profile
)
meiso_enable_warnings(meiso_wifi_telemetry_profile_test)

add_executable(
    meiso_four_kind_lab_protocol_test
    tests/test_four_kind_lab_protocol.c
)
target_link_libraries(
    meiso_four_kind_lab_protocol_test
    PRIVATE meiso_four_kind_lab_protocol
)
meiso_enable_warnings(meiso_four_kind_lab_protocol_test)

add_executable(
    meiso_composition_scene_test
    tests/test_composition_scene.c
)
target_link_libraries(
    meiso_composition_scene_test
    PRIVATE meiso_composition_scene_profile meiso_wifi_telemetry_profile
)
meiso_enable_warnings(meiso_composition_scene_test)

add_executable(
    meiso_composition_scene_resource_test
    tests/test_composition_scene_resource.c
)
target_link_libraries(
    meiso_composition_scene_resource_test
    PRIVATE meiso_composition_scene_profile
)
meiso_enable_warnings(meiso_composition_scene_resource_test)

add_executable(
    meiso_composition_reference_renderer_test
    tests/test_composition_reference_renderer.c
)
target_link_libraries(
    meiso_composition_reference_renderer_test
    PRIVATE meiso_composition_scene_profile
)
meiso_enable_warnings(meiso_composition_reference_renderer_test)

add_executable(
    meiso_composition_coordinator_test
    tests/test_composition_coordinator.c
)
target_link_libraries(
    meiso_composition_coordinator_test
    PRIVATE meiso_composition_scene_profile
)
meiso_enable_warnings(meiso_composition_coordinator_test)

add_executable(
    meiso_composition_tx_scheduler_test
    tests/test_composition_tx_scheduler.c
)
target_link_libraries(
    meiso_composition_tx_scheduler_test
    PRIVATE meiso_runtime_scheduler meiso_composition_session_v2_profile
)
meiso_enable_warnings(meiso_composition_tx_scheduler_test)

if(TARGET meiso_posix_tcp_adapter)
    add_executable(meiso_posix_tcp_test tests/test_posix_tcp.c)
    target_link_libraries(
        meiso_posix_tcp_test
        PRIVATE meiso_posix_tcp_adapter
    )
    meiso_enable_warnings(meiso_posix_tcp_test)

    add_executable(
        meiso_wifi_runtime_tcp_test
        tests/test_wifi_runtime_tcp.c
    )
    target_link_libraries(
        meiso_wifi_runtime_tcp_test
        PRIVATE meiso_posix_tcp_adapter meiso_wifi_telemetry_profile
    )
    meiso_enable_warnings(meiso_wifi_runtime_tcp_test)

    add_executable(
        meiso_composition_runtime_tcp_test
        tests/test_composition_runtime_tcp.c
    )
    target_link_libraries(
        meiso_composition_runtime_tcp_test
        PRIVATE
            meiso_posix_tcp_adapter
            meiso_runtime_scheduler
            meiso_composition_session_v2_profile
    )
    meiso_enable_warnings(meiso_composition_runtime_tcp_test)

    add_executable(
        meiso_composition_runtime_tcp_adversarial_test
        tests/test_composition_runtime_tcp_adversarial.c
    )
    target_link_libraries(
        meiso_composition_runtime_tcp_adversarial_test
        PRIVATE
            meiso_posix_tcp_adapter
            meiso_runtime_scheduler
            meiso_composition_session_v2_profile
    )
    meiso_enable_warnings(meiso_composition_runtime_tcp_adversarial_test)
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

    add_executable(
        meiso_wifi_runtime_host
        devtools/macos/wifi_runtime_host.c
    )
    target_link_libraries(
        meiso_wifi_runtime_host
        PRIVATE meiso_posix_tcp_adapter meiso_wifi_telemetry_profile
    )
    meiso_enable_warnings(meiso_wifi_runtime_host)
endif()

function(meiso_add_conformance_suite suite_name source_file)
    set(target_name meiso_conformance_${suite_name}_suite)
    add_library(${target_name} OBJECT ${source_file})
    target_link_libraries(${target_name} PRIVATE meiso_resource_core)
    target_include_directories(${target_name} PRIVATE src tests)
    target_compile_definitions(
        ${target_name}
        PRIVATE main=meiso_conformance_run_${suite_name}
    )
    meiso_enable_warnings(${target_name})
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
target_link_libraries(meiso_conformance PRIVATE meiso_resource_core)
meiso_enable_warnings(meiso_conformance)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(meiso_resource_short_enum_test tests/test_resource.c)
    target_link_libraries(meiso_resource_short_enum_test PRIVATE meiso_resource_core)
    target_include_directories(meiso_resource_short_enum_test PRIVATE src)
    target_compile_options(meiso_resource_short_enum_test PRIVATE -fshort-enums)
    meiso_enable_warnings(meiso_resource_short_enum_test)
endif()

enable_testing()
add_test(NAME meiso_resource_test COMMAND meiso_resource_test)
add_test(NAME meiso_state_test COMMAND meiso_state_test)
add_test(NAME meiso_stream_test COMMAND meiso_stream_test)
add_test(NAME meiso_blob_test COMMAND meiso_blob_test)
add_test(NAME meiso_descriptor_set_test COMMAND meiso_descriptor_set_test)
add_test(NAME meiso_action_test COMMAND meiso_action_test)
add_test(NAME meiso_four_kind_test COMMAND meiso_four_kind_test)
add_test(NAME meiso_session_test COMMAND meiso_session_test)
add_test(NAME meiso_session_four_kind_test COMMAND meiso_session_four_kind_test)
add_test(NAME meiso_runtime_test COMMAND meiso_runtime_test)
add_test(NAME meiso_runtime_session_test COMMAND meiso_runtime_session_test)
add_test(NAME meiso_runtime_wire_test COMMAND meiso_runtime_wire_test)
add_test(
    NAME meiso_runtime_blob_wire_test
    COMMAND meiso_runtime_blob_wire_test
)
add_test(
    NAME meiso_wifi_telemetry_profile_test
    COMMAND meiso_wifi_telemetry_profile_test
)
add_test(
    NAME meiso_four_kind_lab_protocol_test
    COMMAND meiso_four_kind_lab_protocol_test
)
add_test(
    NAME meiso_composition_scene_test
    COMMAND meiso_composition_scene_test
)
add_test(
    NAME meiso_composition_scene_resource_test
    COMMAND meiso_composition_scene_resource_test
)
add_test(
    NAME meiso_composition_reference_renderer_test
    COMMAND meiso_composition_reference_renderer_test
)
add_test(
    NAME meiso_composition_coordinator_test
    COMMAND meiso_composition_coordinator_test
)
add_test(
    NAME meiso_composition_tx_scheduler_test
    COMMAND meiso_composition_tx_scheduler_test
)
if(TARGET meiso_posix_tcp_test)
    add_test(NAME meiso_posix_tcp_test COMMAND meiso_posix_tcp_test)
endif()
if(TARGET meiso_wifi_runtime_tcp_test)
    add_test(
        NAME meiso_wifi_runtime_tcp_test
        COMMAND meiso_wifi_runtime_tcp_test
    )
endif()
if(TARGET meiso_composition_runtime_tcp_test)
    add_test(
        NAME meiso_composition_runtime_tcp_test
        COMMAND meiso_composition_runtime_tcp_test
    )
endif()

if(TARGET meiso_composition_runtime_tcp_adversarial_test)
    add_test(
        NAME meiso_composition_runtime_tcp_adversarial_test
        COMMAND meiso_composition_runtime_tcp_adversarial_test
    )
endif()
if(TARGET meiso_four_kind_lab_internal_test)
    add_test(
        NAME meiso_four_kind_lab_internal_test
        COMMAND meiso_four_kind_lab_internal_test --self-test
    )
endif()
add_test(NAME meiso_conformance COMMAND meiso_conformance)
if(TARGET meiso_resource_short_enum_test)
    add_test(NAME meiso_resource_short_enum_test COMMAND meiso_resource_short_enum_test)
endif()
