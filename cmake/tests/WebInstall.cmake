# Composition Editor tooling and installed-package consumer lanes.

# Composition Editor tests are a distinct tool lane. The native bridge test owns a
# child process and fixed deadlines, so it is serialized under parallel CTest.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME meiso_composition_editor_server_test
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/devtools/web/composition_editor/test_server.py"
    )
    set_tests_properties(
        meiso_composition_editor_server_test
        PROPERTIES LABELS "web" TIMEOUT 60
    )
    if(TARGET meiso_composition_host_bridge)
        add_test(
            NAME meiso_composition_editor_native_bridge_test
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "MEISO_COMPOSITION_HOST_BRIDGE=$<TARGET_FILE:meiso_composition_host_bridge>"
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/devtools/web/composition_editor/test_native_bridge.py"
        )
        set_tests_properties(
            meiso_composition_editor_native_bridge_test
            PROPERTIES LABELS "web;service" RUN_SERIAL TRUE TIMEOUT 60
        )
    endif()
endif()

set(meiso_installed_consumer_test_arguments
    "-DMEISO_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DMEISO_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DMEISO_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/installed-consumer-e2e"
    "-DMEISO_GENERATOR=${CMAKE_GENERATOR}"
    "-DMEISO_CONFIG=$<CONFIG>"
    "-DMEISO_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DMEISO_C_FLAGS=${CMAKE_C_FLAGS}"
    "-DMEISO_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
)
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
    list(
        APPEND meiso_installed_consumer_test_arguments
        "-DMEISO_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    )
endif()

set(meiso_have_installed_host FALSE)
if(TARGET meiso_composition_host_runtime)
    set(meiso_have_installed_host TRUE)
endif()
add_test(
    NAME meiso_installed_consumer_e2e
    COMMAND
        "${CMAKE_COMMAND}"
        ${meiso_installed_consumer_test_arguments}
        "-DMEISO_HAVE_HOST=${meiso_have_installed_host}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_installed_consumers.cmake"
)
set_tests_properties(
    meiso_installed_consumer_e2e
    PROPERTIES LABELS "install" RUN_SERIAL TRUE TIMEOUT 120
)
unset(meiso_have_installed_host)
unset(meiso_installed_consumer_test_arguments)

find_program(MEISO_NODE_EXECUTABLE node)
if(MEISO_NODE_EXECUTABLE)
    add_test(
        NAME meiso_composition_editor_model_test
        COMMAND
            "${MEISO_NODE_EXECUTABLE}" --test
            "${CMAKE_CURRENT_SOURCE_DIR}/devtools/web/composition_editor/test_model.mjs"
    )
    set_tests_properties(
        meiso_composition_editor_model_test
        PROPERTIES LABELS "web" TIMEOUT 30
    )
    add_test(
        NAME meiso_composition_editor_javascript_syntax
        COMMAND
            "${MEISO_NODE_EXECUTABLE}" --check
            "${CMAKE_CURRENT_SOURCE_DIR}/devtools/web/composition_editor/static/app.js"
    )
    set_tests_properties(
        meiso_composition_editor_javascript_syntax
        PROPERTIES LABELS "web" TIMEOUT 30
    )
endif()
