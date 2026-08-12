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

add_library(meiso_slice src/meiso_slice.c)
target_include_directories(meiso_slice PUBLIC include)

function(meiso_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

meiso_enable_warnings(meiso_slice)

if(UNIX OR MINGW)
    target_link_libraries(meiso_slice PUBLIC m)
endif()

add_library(meiso_latest_state src/meiso_latest_state.c)
target_include_directories(meiso_latest_state PUBLIC include)
meiso_enable_warnings(meiso_latest_state)

add_library(meiso_resource_core
    src/meiso_resource.c
    src/meiso_state.c
    src/meiso_stream.c
    src/meiso_blob.c
)
target_include_directories(meiso_resource_core PUBLIC include)
meiso_enable_warnings(meiso_resource_core)

add_executable(meiso_synthetic_demo demo/synthetic_main.c)
target_link_libraries(meiso_synthetic_demo PRIVATE meiso_slice)
meiso_enable_warnings(meiso_synthetic_demo)
if(MSVC)
    target_compile_definitions(meiso_synthetic_demo PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(meiso_slice_test tests/test_slice.c)
target_link_libraries(meiso_slice_test PRIVATE meiso_slice)
meiso_enable_warnings(meiso_slice_test)

add_executable(meiso_latest_state_test tests/test_latest_state.c)
target_link_libraries(meiso_latest_state_test PRIVATE meiso_latest_state)
meiso_enable_warnings(meiso_latest_state_test)

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

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(meiso_resource_short_enum_test tests/test_resource.c)
    target_link_libraries(meiso_resource_short_enum_test PRIVATE meiso_resource_core)
    target_include_directories(meiso_resource_short_enum_test PRIVATE src)
    target_compile_options(meiso_resource_short_enum_test PRIVATE -fshort-enums)
    meiso_enable_warnings(meiso_resource_short_enum_test)
endif()

enable_testing()
add_test(NAME meiso_slice_test COMMAND meiso_slice_test)
add_test(NAME meiso_latest_state_test COMMAND meiso_latest_state_test)
add_test(NAME meiso_resource_test COMMAND meiso_resource_test)
add_test(NAME meiso_state_test COMMAND meiso_state_test)
add_test(NAME meiso_stream_test COMMAND meiso_stream_test)
add_test(NAME meiso_blob_test COMMAND meiso_blob_test)
if(TARGET meiso_resource_short_enum_test)
    add_test(NAME meiso_resource_short_enum_test COMMAND meiso_resource_short_enum_test)
endif()
add_test(
    NAME meiso_synthetic_demo
    COMMAND meiso_synthetic_demo ${CMAKE_CURRENT_BINARY_DIR}/slice_preview.ppm
)
