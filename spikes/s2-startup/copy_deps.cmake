# Copies the non-Qt, non-system DLLs that EXE needs (directly or transitively) from SEARCH into DEST.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
# Usage: cmake -DEXE=<exe> -DDEST=<dir> -DSEARCH=<vcpkg bin dir> -P copy_deps.cmake
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${EXE}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES ${SEARCH}
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*")
file(TO_CMAKE_PATH "${SEARCH}" search_norm)
string(TOLOWER "${search_norm}" search_lower)
set(n 0)
foreach(f ${resolved})
    file(TO_CMAKE_PATH "${f}" f_norm)
    string(TOLOWER "${f_norm}" f_lower)
    string(FIND "${f_lower}" "${search_lower}/" pos)
    if(pos EQUAL 0)
        file(COPY ${f} DESTINATION ${DEST})
        math(EXPR n "${n}+1")
    endif()
endforeach()
message(STATUS "copy_deps: copied ${n} DLLs from ${SEARCH}")
if(unresolved)
    message(STATUS "copy_deps: unresolved: ${unresolved}")
endif()
