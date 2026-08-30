# Shared test registration helpers. This module is loaded exactly once before
# every lane definition.

function(meiso_register_target_test target label)
    add_test(NAME ${target} COMMAND ${target} ${ARGN})
    set_tests_properties(
        ${target}
        PROPERTIES LABELS "${label}" TIMEOUT 60
    )
endfunction()

function(meiso_add_c_test target source label)
    add_executable(${target} ${source})
    if(ARGN)
        target_link_libraries(${target} PRIVATE ${ARGN})
    endif()
    meiso_enable_warnings(${target})
    meiso_set_target_folder(${target} "Meiso/Tests/${label}")
    meiso_register_target_test(${target} "${label}")
endfunction()
