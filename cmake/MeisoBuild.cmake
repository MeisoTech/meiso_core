# Repository-wide compiler policy. Product modules should not add weaker flags.

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
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()

function(meiso_set_target_folder target folder)
    set_property(TARGET ${target} PROPERTY FOLDER "${folder}")
endfunction()

# Every reusable target consumes public headers through the repository include
# root while building and through the configured installation include root as
# an imported target. EXPORT_NAME keeps build-tree aliases and installed target
# names identical.
function(meiso_configure_public_target target export_name)
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(include_scope INTERFACE)
    else()
        set(include_scope PUBLIC)
    endif()

    target_include_directories(
        ${target}
        ${include_scope}
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${MEISO_GENERATED_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
endfunction()
