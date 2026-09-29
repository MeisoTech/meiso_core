cmake_minimum_required(VERSION 3.21)

foreach(required_variable
        MEISO_BUILD_DIR
        MEISO_SOURCE_DIR
        MEISO_TEST_ROOT
        MEISO_GENERATOR
        MEISO_C_COMPILER)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required_variable}")
    endif()
endforeach()
foreach(required_defined_variable MEISO_C_FLAGS MEISO_EXE_LINKER_FLAGS)
    if(NOT DEFINED ${required_defined_variable})
        message(FATAL_ERROR "Missing ${required_defined_variable}")
    endif()
endforeach()

function(meiso_run)
    execute_process(
        COMMAND ${ARGV}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        string(REPLACE ";" " " rendered_command "${ARGV}")
        message(
            FATAL_ERROR
            "Command failed (${result}): ${rendered_command}\n${output}\n${error}"
        )
    endif()
endfunction()

function(meiso_install_component component prefix)
    set(command
        "${CMAKE_COMMAND}"
        --install "${MEISO_BUILD_DIR}"
        --prefix "${prefix}"
        --component "${component}"
    )
    if(DEFINED MEISO_CONFIG AND NOT "${MEISO_CONFIG}" STREQUAL "")
        list(APPEND command --config "${MEISO_CONFIG}")
    endif()
    meiso_run(${command})
endfunction()

function(meiso_build_consumer name source_dir prefix)
    set(build_dir "${MEISO_TEST_ROOT}/${name}-build")
    set(configure_command
        "${CMAKE_COMMAND}"
        -S "${source_dir}"
        -B "${build_dir}"
        -G "${MEISO_GENERATOR}"
        "-DCMAKE_PREFIX_PATH=${prefix}"
        "-DCMAKE_C_COMPILER=${MEISO_C_COMPILER}"
        "-DCMAKE_C_FLAGS=${MEISO_C_FLAGS}"
        "-DCMAKE_EXE_LINKER_FLAGS=${MEISO_EXE_LINKER_FLAGS}"
        -DCMAKE_BUILD_TYPE=Release
    )
    if(DEFINED MEISO_TOOLCHAIN_FILE AND NOT MEISO_TOOLCHAIN_FILE STREQUAL "")
        list(
            APPEND configure_command
            "-DCMAKE_TOOLCHAIN_FILE=${MEISO_TOOLCHAIN_FILE}"
        )
    endif()
    meiso_run(${configure_command})
    set(build_command "${CMAKE_COMMAND}" --build "${build_dir}")
    set(test_command "${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}")
    if(DEFINED MEISO_CONFIG AND NOT "${MEISO_CONFIG}" STREQUAL "")
        list(APPEND build_command --config "${MEISO_CONFIG}")
        list(APPEND test_command -C "${MEISO_CONFIG}")
    endif()
    list(APPEND test_command --output-on-failure)
    meiso_run(${build_command})
    meiso_run(${test_command})
endfunction()

function(meiso_assert_installed_composition_profile_surface prefix)
    set(
        scene_header
        "${prefix}/include/meiso/profiles/composition/scene.h"
    )
    if(NOT EXISTS "${scene_header}")
        message(FATAL_ERROR "Profiles prefix is missing ${scene_header}")
    endif()
    if(EXISTS "${prefix}/include/meiso/internal")
        message(FATAL_ERROR "Profiles prefix leaked build-only internal headers")
    endif()
    file(READ "${scene_header}" scene_header_contents)
    set(old_macro_prefix "MEISO_COMPOSITION_SCENE_")
    set(old_function_prefix "meiso_composition_scene_")
    set(forbidden_tokens
        "${old_macro_prefix}PROFILE_VERSION_V1"
        "${old_macro_prefix}PROFILE_VERSION_V2"
        "${old_macro_prefix}PROFILE_V1_RUNTIME_WIRE_VERSION"
        "${old_macro_prefix}PROFILE_V2_RUNTIME_WIRE_VERSION"
        "${old_function_prefix}profile"
    )
    foreach(forbidden_token IN LISTS forbidden_tokens)
        string(FIND "${scene_header_contents}" "${forbidden_token}" position)
        if(NOT position EQUAL -1)
            message(
                FATAL_ERROR
                "Installed Scene schema leaked old Profile API: ${forbidden_token}"
            )
        endif()
    endforeach()
endfunction()

file(REMOVE_RECURSE "${MEISO_TEST_ROOT}")
file(MAKE_DIRECTORY "${MEISO_TEST_ROOT}")

set(core_stage "${MEISO_TEST_ROOT}/core-stage")
set(core_prefix "${MEISO_TEST_ROOT}/core-relocated")
meiso_install_component(Core "${core_stage}")
file(RENAME "${core_stage}" "${core_prefix}")

foreach(forbidden_path
        "${core_prefix}/include/meiso/runtime.h"
        "${core_prefix}/include/meiso/runtime"
        "${core_prefix}/include/meiso/profiles"
        "${core_prefix}/include/meiso/composition"
        "${core_prefix}/include/meiso/host")
    if(EXISTS "${forbidden_path}")
        message(FATAL_ERROR "Core prefix leaked ${forbidden_path}")
    endif()
endforeach()
file(
    GLOB_RECURSE core_runtime_artifacts
    LIST_DIRECTORIES FALSE
    "${core_prefix}/*meiso_runtime*"
    "${core_prefix}/*MeisoRuntimeTargets*"
)
if(core_runtime_artifacts)
    message(FATAL_ERROR "Core prefix leaked Runtime artifacts: ${core_runtime_artifacts}")
endif()
meiso_build_consumer(
    core-consumer
    "${MEISO_SOURCE_DIR}/tests/installed-consumers/core"
    "${core_prefix}"
)

set(runtime_stage "${MEISO_TEST_ROOT}/runtime-stage")
set(runtime_prefix "${MEISO_TEST_ROOT}/runtime-relocated")
meiso_install_component(Core "${runtime_stage}")
meiso_install_component(Runtime "${runtime_stage}")
file(RENAME "${runtime_stage}" "${runtime_prefix}")

foreach(required_path
        "${runtime_prefix}/include/meiso/core.h"
        "${runtime_prefix}/include/meiso/runtime.h")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Runtime prefix is missing ${required_path}")
    endif()
endforeach()
foreach(forbidden_path
        "${runtime_prefix}/include/meiso/profiles"
        "${runtime_prefix}/include/meiso/composition"
        "${runtime_prefix}/include/meiso/host")
    if(EXISTS "${forbidden_path}")
        message(FATAL_ERROR "Runtime prefix leaked ${forbidden_path}")
    endif()
endforeach()
meiso_build_consumer(
    runtime-consumer
    "${MEISO_SOURCE_DIR}/tests/installed-consumers/runtime"
    "${runtime_prefix}"
)

set(difi_stage "${MEISO_TEST_ROOT}/difi-stage")
set(difi_prefix "${MEISO_TEST_ROOT}/difi-relocated")
foreach(component Core Runtime Profiles DisplayIngressHostFixtureInput)
    meiso_install_component(${component} "${difi_stage}")
endforeach()
file(RENAME "${difi_stage}" "${difi_prefix}")
meiso_assert_installed_composition_profile_surface("${difi_prefix}")
foreach(required_path
        "${difi_prefix}/include/meiso/host/display_ingress_fixture_input.h")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "DisplayIngressHostFixtureInput prefix is missing ${required_path}")
    endif()
endforeach()
foreach(forbidden_path
        "${difi_prefix}/include/meiso/adapters"
        "${difi_prefix}/include/meiso/composition"
        "${difi_prefix}/include/meiso/host/edge.h"
        "${difi_prefix}/include/meiso/host/composition.h"
        "${difi_prefix}/include/meiso/host/composition_runtime.h"
        "${difi_prefix}/include/meiso/host/remote_edge.h"
        "${difi_prefix}/include/meiso/simulation")
    if(EXISTS "${forbidden_path}")
        message(FATAL_ERROR
            "DisplayIngressHostFixtureInput prefix leaked ${forbidden_path}")
    endif()
endforeach()
file(
    GLOB_RECURSE difi_forbidden_artifacts
    LIST_DIRECTORIES FALSE
    "${difi_prefix}/*MeisoPosixTargets*"
    "${difi_prefix}/*MeisoControlTargets*"
    "${difi_prefix}/*MeisoClientTargets*"
    "${difi_prefix}/*MeisoCompositionTargets*"
    "${difi_prefix}/*MeisoHostTargets*"
)
if(difi_forbidden_artifacts)
    message(FATAL_ERROR
        "DisplayIngressHostFixtureInput leaked targets: ${difi_forbidden_artifacts}")
endif()
meiso_build_consumer(
    difi-consumer
    "${MEISO_SOURCE_DIR}/tests/installed-consumers/display-ingress-host-fixture-input"
    "${difi_prefix}"
)
unset(difi_forbidden_artifacts)
unset(difi_prefix)
unset(difi_stage)

if(WIN32)
    set(windows_stage "${MEISO_TEST_ROOT}/windows-stage")
    set(windows_prefix "${MEISO_TEST_ROOT}/windows-relocated")
    foreach(component Core Runtime Profiles Windows)
        meiso_install_component(${component} "${windows_stage}")
    endforeach()
    file(RENAME "${windows_stage}" "${windows_prefix}")
    meiso_build_consumer(windows-consumer
        "${MEISO_SOURCE_DIR}/tests/installed-consumers/windows" "${windows_prefix}")
endif()

if(NOT MEISO_LEGACY)
    set(product_prefix "${MEISO_TEST_ROOT}/product-default")
    meiso_run("${CMAKE_COMMAND}" --install "${MEISO_BUILD_DIR}" --prefix "${product_prefix}")
    foreach(forbidden_path
            include/meiso/runtime/scheduler.h
            include/meiso/composition
            include/meiso/simulation
            include/meiso/host/edge.h
            include/meiso/host/composition.h
            include/meiso/host/composition_runtime.h
            include/meiso/host/remote_edge.h)
        if(EXISTS "${product_prefix}/${forbidden_path}")
            message(FATAL_ERROR "Default package leaked historical API: ${forbidden_path}")
        endif()
    endforeach()
    file(GLOB_RECURSE forbidden_exports
        "${product_prefix}/*MeisoClientTargets*"
        "${product_prefix}/*MeisoCompositionTargets*"
        "${product_prefix}/*MeisoSimulationTargets*"
        "${product_prefix}/*MeisoControlTargets*"
        "${product_prefix}/*MeisoHostTargets*"
        "${product_prefix}/*meiso_runtime_scheduler*")
    if(forbidden_exports)
        message(FATAL_ERROR "Default package leaked historical exports: ${forbidden_exports}")
    endif()
endif()

if(MEISO_LEGACY)
    set(client_stage "${MEISO_TEST_ROOT}/client-stage")
    set(client_prefix "${MEISO_TEST_ROOT}/client-relocated")
    foreach(component Core Runtime Profiles Client)
        meiso_install_component(${component} "${client_stage}")
    endforeach()
    file(RENAME "${client_stage}" "${client_prefix}")
    meiso_assert_installed_composition_profile_surface("${client_prefix}")
    foreach(required_path
            "${client_prefix}/include/meiso/host/edge.h"
            "${client_prefix}/include/meiso/host/composition.h")
        if(NOT EXISTS "${required_path}")
            message(FATAL_ERROR "Client prefix is missing ${required_path}")
        endif()
    endforeach()
    foreach(forbidden_path
            "${client_prefix}/include/meiso/host/composition_runtime.h"
            "${client_prefix}/include/meiso/host/remote_edge.h"
            "${client_prefix}/include/meiso/simulation"
            "${client_prefix}/include/meiso/composition")
        if(EXISTS "${forbidden_path}")
            message(FATAL_ERROR "Client prefix leaked ${forbidden_path}")
        endif()
    endforeach()
    meiso_build_consumer(
        client-consumer
        "${MEISO_SOURCE_DIR}/tests/installed-consumers/client"
        "${client_prefix}"
    )

    set(composition_stage "${MEISO_TEST_ROOT}/composition-stage")
    set(composition_prefix "${MEISO_TEST_ROOT}/composition-relocated")
    foreach(component Core Runtime Profiles Composition)
        meiso_install_component(${component} "${composition_stage}")
    endforeach()
    file(RENAME "${composition_stage}" "${composition_prefix}")
    meiso_assert_installed_composition_profile_surface("${composition_prefix}")
    foreach(forbidden_path
            "${composition_prefix}/include/meiso/adapters"
            "${composition_prefix}/include/meiso/composition/control_session.h"
            "${composition_prefix}/include/meiso/host")
        if(EXISTS "${forbidden_path}")
            message(FATAL_ERROR "Portable Composition prefix leaked ${forbidden_path}")
        endif()
    endforeach()
    meiso_build_consumer(
        composition-consumer
        "${MEISO_SOURCE_DIR}/tests/installed-consumers/composition"
        "${composition_prefix}"
    )

    set(simulation_stage "${MEISO_TEST_ROOT}/simulation-stage")
    set(simulation_prefix "${MEISO_TEST_ROOT}/simulation-relocated")
    foreach(component Core Runtime Profiles Client Composition Simulation)
        meiso_install_component(${component} "${simulation_stage}")
    endforeach()
    file(RENAME "${simulation_stage}" "${simulation_prefix}")
    meiso_assert_installed_composition_profile_surface("${simulation_prefix}")
    foreach(required_path
            "${simulation_prefix}/include/meiso/simulation/composition_edge.h"
            "${simulation_prefix}/include/meiso/simulation/composition_trace.h")
        if(NOT EXISTS "${required_path}")
            message(FATAL_ERROR "Simulation prefix is missing ${required_path}")
        endif()
    endforeach()
    foreach(forbidden_path
            "${simulation_prefix}/include/meiso/adapters"
            "${simulation_prefix}/include/meiso/composition/control_session.h"
            "${simulation_prefix}/include/meiso/host/composition_runtime.h"
            "${simulation_prefix}/include/meiso/host/remote_edge.h")
        if(EXISTS "${forbidden_path}")
            message(FATAL_ERROR "Simulation prefix leaked ${forbidden_path}")
        endif()
    endforeach()
    meiso_build_consumer(
        simulation-consumer
        "${MEISO_SOURCE_DIR}/tests/installed-consumers/simulation"
        "${simulation_prefix}"
    )

    if(DEFINED MEISO_HAVE_HOST AND MEISO_HAVE_HOST)
        set(host_stage "${MEISO_TEST_ROOT}/host-stage")
        set(host_prefix "${MEISO_TEST_ROOT}/host-relocated")
        foreach(component Core Runtime Profiles Client Posix Control Host)
            meiso_install_component(${component} "${host_stage}")
        endforeach()
        file(RENAME "${host_stage}" "${host_prefix}")
        meiso_assert_installed_composition_profile_surface("${host_prefix}")
        foreach(forbidden_path
                "${host_prefix}/include/meiso/simulation"
                "${host_prefix}/include/meiso/internal"
                "${host_prefix}/include/meiso/edge"
                "${host_prefix}/include/meiso/composition/scene_store.h"
                "${host_prefix}/include/meiso/composition/coordinator.h"
                "${host_prefix}/include/meiso/composition/reference_renderer.h"
                "${host_prefix}/include/meiso/composition/cpu_raster_renderer.h")
            if(EXISTS "${forbidden_path}")
                message(FATAL_ERROR "Host prefix leaked mixed SDK API: ${forbidden_path}")
            endif()
        endforeach()
        foreach(required_path
                "${host_prefix}/include/meiso/host/edge.h"
                "${host_prefix}/include/meiso/host/composition.h"
                "${host_prefix}/include/meiso/host/composition_runtime.h"
                "${host_prefix}/include/meiso/host/remote_edge.h")
            if(NOT EXISTS "${required_path}")
                message(FATAL_ERROR "Host prefix is missing ${required_path}")
            endif()
        endforeach()
        file(
            GLOB_RECURSE forbidden_sdk_artifacts
            LIST_DIRECTORIES FALSE
            "${host_prefix}/*meiso_composition_sdk*"
            "${host_prefix}/*meiso_composition_edge_simulator*"
            "${host_prefix}/*meiso_composition_edge_trace*"
            "${host_prefix}/*meiso_composition_session_v2*"
            "${host_prefix}/*meiso_four_kind*"
            "${host_prefix}/*meiso_k230*"
            "${host_prefix}/*meiso_composition_scene_store*"
            "${host_prefix}/*meiso_composition_coordinator*"
            "${host_prefix}/*meiso_composition_reference_renderer*"
            "${host_prefix}/*meiso_composition_cpu_raster_renderer*"
            "${host_prefix}/*MeisoCompositionTargets*"
        )
        if(forbidden_sdk_artifacts)
            message(
                FATAL_ERROR
                "Installed SDK leaked build-only artifacts: ${forbidden_sdk_artifacts}"
            )
        endif()
        meiso_build_consumer(
            host-consumer
            "${MEISO_SOURCE_DIR}/tests/installed-consumers/host"
            "${host_prefix}"
        )
    endif()
endif()
