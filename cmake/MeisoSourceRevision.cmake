# A package revision identifies the source used to build it, not its ABI.
# git archive (including release source archives) substitutes the tracked file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/cmake/source-revision.txt")
file(READ "${PROJECT_SOURCE_DIR}/cmake/source-revision.txt" MEISO_SOURCE_REVISION)
string(STRIP "${MEISO_SOURCE_REVISION}" MEISO_SOURCE_REVISION)
string(LENGTH "${MEISO_SOURCE_REVISION}" revision_length)
if(NOT revision_length EQUAL 40 OR NOT MEISO_SOURCE_REVISION MATCHES "^[0-9a-f]+$")
    set(MEISO_SOURCE_REVISION "unknown")
    if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
        find_package(Git QUIET)
        if(Git_FOUND)
            # Regenerate package provenance when a tracked input or checkout changes.
            execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                OUTPUT_VARIABLE tracked OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(REPLACE "\n" ";" tracked "${tracked}")
            foreach(input IN LISTS tracked)
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${PROJECT_SOURCE_DIR}/${input}")
            endforeach()
            execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                OUTPUT_VARIABLE branch_ref OUTPUT_STRIP_TRAILING_WHITESPACE)
            foreach(input HEAD index "${branch_ref}")
                if(NOT input STREQUAL "")
                    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${input}"
                        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                        OUTPUT_VARIABLE git_input OUTPUT_STRIP_TRAILING_WHITESPACE)
                    get_filename_component(git_input "${git_input}" ABSOLUTE
                        BASE_DIR "${PROJECT_SOURCE_DIR}")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_input}")
                endif()
            endforeach()
            execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE revision_result)
            execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                OUTPUT_VARIABLE changes RESULT_VARIABLE status_result)
            if(revision_result EQUAL 0 AND status_result EQUAL 0)
                set(MEISO_SOURCE_REVISION "${revision}")
                if(NOT changes STREQUAL "")
                    string(APPEND MEISO_SOURCE_REVISION "-dirty")
                endif()
            endif()
        endif()
    endif()
endif()
message(STATUS "Meiso source revision: ${MEISO_SOURCE_REVISION}")
